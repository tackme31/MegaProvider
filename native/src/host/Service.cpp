#include "Service.h"

#include "app/Logging.h"
#include "core/MegaErrorCodes.h"
#include "mega/Handles.h"
#include "mega/MegaSdkClient.h"

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>

namespace
{

// Host-level error codes. Positive so they never collide with the SDK's (all <= 0).
constexpr int kBadRequest = 1;
constexpr int kNotLoggedIn = 2;

struct HostError : std::runtime_error
{
    int code;
    HostError(int c, const std::string& message) : std::runtime_error(message), code(c) {}
};

template <typename T> T unwrap(Result<T> r)
{
    if (!r.success)
        throw HostError(r.errorCode, r.errorMessage);
    return std::move(r.value());
}

void unwrap(Result<void> r)
{
    if (!r.success)
        throw HostError(r.errorCode, r.errorMessage);
}

// IMegaClient is callback-based (built for a GUI); each request here just waits for the
// one completion. `start` receives the onDone to hand to the client.
template <typename T, typename Start> Result<T> await(Start start)
{
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    start(std::function<void(Result<T>)>([promise](Result<T> r) { promise->set_value(std::move(r)); }));
    return future.get();
}

const Json& require(const Json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null())
        throw HostError(kBadRequest, std::string("missing '") + key + "'");
    return args[key];
}

std::string requireString(const Json& args, const char* key)
{
    const Json& v = require(args, key);
    if (!v.is_string())
        throw HostError(kBadRequest, std::string("'") + key + "' must be a string");
    return v.get<std::string>();
}

// Missing or null means "not given" (the C# side serializes unset optional fields as null).
std::string optionalString(const Json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null())
        return {};
    return requireString(args, key);
}

std::uint64_t parseHandle(const std::string& text)
{
    if (auto h = base64ToHandle(text))
        return *h;
    throw HostError(kBadRequest, "not a node handle: '" + text + "'");
}

std::uint64_t requireHandle(const Json& args, const char* key) { return parseHandle(requireString(args, key)); }

// A folder argument where null/absent means the Cloud Drive root.
struct FolderRef
{
    std::uint64_t handle = 0;
    bool isRoot = true;
};

FolderRef optionalFolder(const Json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null())
        return {};
    return {parseHandle(args[key].get<std::string>()), false};
}

Json entryToJson(const FileEntry& e)
{
    return {{"handle", handleToBase64(e.handle)},
            {"name", e.name},
            {"folder", e.isFolder},
            {"size", e.sizeBytes},
            {"mtime", e.modificationTime}};
}

Json entriesToJson(const std::vector<FileEntry>& entries)
{
    Json list = Json::array();
    for (const auto& e : entries)
        list.push_back(entryToJson(e));
    return list;
}

const char* kindName(ViewKind kind)
{
    switch (kind)
    {
        case ViewKind::CloudDrive:
            return "cloud";
        case ViewKind::Rubbish:
            return "rubbish";
        default:
            return "other";
    }
}

// Progress for one transfer, at most a few lines a second. A failed write means the
// client disconnected (Ctrl+C in PowerShell), so the transfer is cancelled.
class ProgressRelay
{
public:
    ProgressRelay(const Emit& emit, std::function<void()> cancel) : mEmit(emit), mCancel(std::move(cancel)) {}

    void operator()(std::uint64_t done, std::uint64_t total)
    {
        const auto now = std::chrono::steady_clock::now();
        if (mCancelled || now - mLast < std::chrono::milliseconds(250))
            return;
        mLast = now;
        if (!mEmit({{"progress", {{"done", done}, {"total", total}}}}))
        {
            mCancelled = true;
            mCancel();
        }
    }

private:
    const Emit& mEmit;
    std::function<void()> mCancel;
    std::chrono::steady_clock::time_point mLast{};
    bool mCancelled = false;
};

} // namespace

Service::Service(MegaSdkClient& client) : mClient(client) {}

Json Service::handle(const Json& request, const Emit& emit)
{
    try
    {
        if (!request.is_object() || !request.contains("op") || !request["op"].is_string())
            throw HostError(kBadRequest, "request must be an object with a string 'op'");
        const Json args = request.value("args", Json::object());
        return {{"ok", true}, {"result", dispatch(request["op"].get<std::string>(), args, emit)}};
    }
    catch (const HostError& e)
    {
        // The session was killed elsewhere (logout on another client, password change): stop
        // claiming to be logged in so the client resumes or asks for Connect-MegaAccount.
        if (e.code == MegaErrorCode::kESid)
            mReady = false;
        return {{"ok", false}, {"error", {{"code", e.code}, {"message", e.what()}}}};
    }
    catch (const std::exception& e)
    {
        LOG_WARN("host") << "request failed:" << e.what();
        return {{"ok", false}, {"error", {{"code", MegaErrorCode::kEInternal}, {"message", e.what()}}}};
    }
}

Json Service::dispatch(const std::string& op, const Json& args, const Emit& emit)
{
    if (op == "status")
        return status();
    if (op == "login")
        return login(args);
    if (op == "resume")
        return resume(args);
    if (op == "logout")
        return logout();
    if (op == "list")
        return list(args);
    if (op == "rubbish")
        return rubbish();
    if (op == "path")
        return path(args);
    if (op == "restoreTarget")
        return restoreTarget(args);
    if (op == "mkdir")
        return mkdir(args);
    if (op == "rename")
        return rename(args);
    if (op == "move")
        return move(args);
    if (op == "copy")
        return copy(args);
    if (op == "trash")
        return trash(args);
    if (op == "upload")
        return upload(args, emit);
    if (op == "download")
        return download(args, emit);
    if (op == "shutdown")
    {
        mStopRequested = true;
        return Json::object();
    }
    throw HostError(kBadRequest, "unknown op '" + op + "'");
}

void Service::requireReady() const
{
    if (!mReady)
        throw HostError(kNotLoggedIn, "the host is not logged in");
}

std::string Service::email() const { return unwrap(mClient.currentAccountIdentity()).email; }

void Service::fetchNodes()
{
    unwrap(await<void>([&](auto done) {
        mClient.fetchNodes([](std::uint64_t, std::uint64_t) {}, std::move(done));
    }));
    mReady = true;
}

void Service::dropSession()
{
    if (!mReady)
        return;
    mReady = false;
    unwrap(await<void>([&](auto done) { mClient.localLogout(std::move(done)); }));
}

Json Service::status()
{
    if (mReady)
    {
        // mReady outlives a session invalidated on the server; the SDK then has none to dump.
        Result<std::string> session = mClient.currentSessionToken();
        if (session.success)
            return {{"loggedIn", true}, {"email", email()}, {"session", session.value()}};
        LOG_INFO("host") << "session is gone (" << session.errorMessage << "); reporting logged out";
        mReady = false;
    }
    return {{"loggedIn", false}};
}

Json Service::login(const Json& args)
{
    const std::string user = requireString(args, "email");
    const std::string password = requireString(args, "password");
    const std::string authCode = optionalString(args, "authCode");

    std::lock_guard<std::mutex> lock(mAuthMutex);
    dropSession();
    unwrap(await<void>([&](auto done) {
        if (authCode.empty())
            mClient.login(user, password, std::move(done));
        else
            mClient.multiFactorAuthLogin(user, password, authCode, std::move(done));
    }));
    fetchNodes();
    return {{"email", email()}, {"session", unwrap(mClient.currentSessionToken())}};
}

Json Service::resume(const Json& args)
{
    const std::string session = requireString(args, "session");

    std::lock_guard<std::mutex> lock(mAuthMutex);
    if (mReady && unwrap(mClient.currentSessionToken()) == session)
        return {{"email", email()}};
    dropSession();
    unwrap(await<void>([&](auto done) { mClient.loginWithSession(session, std::move(done)); }));
    fetchNodes();
    return {{"email", email()}};
}

Json Service::logout()
{
    std::lock_guard<std::mutex> lock(mAuthMutex);
    if (!mReady)
        return Json::object();
    mReady = false;
    unwrap(await<void>([&](auto done) { mClient.logout(std::move(done)); }));
    return Json::object();
}

Json Service::list(const Json& args)
{
    requireReady();
    const FolderRef folder = optionalFolder(args, "handle");
    auto entries = unwrap(await<std::vector<FileEntry>>([&](auto done) {
        if (folder.isRoot)
            mClient.getRootChildren(SortOrder{}, std::move(done));
        else
            mClient.getChildren(folder.handle, SortOrder{}, std::move(done));
    }));
    return entriesToJson(entries);
}

Json Service::rubbish()
{
    requireReady();
    auto entries = unwrap(await<std::vector<FileEntry>>([&](auto done) {
        mClient.getRubbishChildren(SortOrder{}, std::move(done));
    }));
    return entriesToJson(entries);
}

Json Service::path(const Json& args)
{
    requireReady();
    const std::uint64_t handle = requireHandle(args, "handle");
    auto segments = unwrap(await<std::vector<PathSegment>>([&](auto done) {
        mClient.getPath(handle, false, std::move(done));
    }));
    // Root-first, root included; the root's kind says whether the node is in the Cloud
    // Drive or the Rubbish Bin.
    Json names = Json::array();
    for (std::size_t i = 1; i < segments.size(); ++i)
        names.push_back(segments[i].name);
    return {{"root", segments.empty() ? "other" : kindName(segments.front().kind)}, {"names", names}};
}

Json Service::restoreTarget(const Json& args)
{
    requireReady();
    const RestoreTarget target = unwrap(mClient.getRestoreTarget(requireHandle(args, "handle")));
    return {{"parent", target.isRoot ? Json(nullptr) : Json(handleToBase64(target.handle))},
            {"fellBackToRoot", target.fellBackToRoot}};
}

Json Service::mkdir(const Json& args)
{
    requireReady();
    const FolderRef parent = optionalFolder(args, "parent");
    const std::string name = requireString(args, "name");
    unwrap(await<void>([&](auto done) { mClient.createFolder(parent.handle, parent.isRoot, name, std::move(done)); }));
    return Json::object();
}

Json Service::rename(const Json& args)
{
    requireReady();
    const std::uint64_t handle = requireHandle(args, "handle");
    const std::string name = requireString(args, "name");
    unwrap(await<void>([&](auto done) { mClient.renameNode(handle, name, std::move(done)); }));
    return Json::object();
}

Json Service::move(const Json& args)
{
    requireReady();
    const std::uint64_t handle = requireHandle(args, "handle");
    const FolderRef parent = optionalFolder(args, "parent");
    const std::string name = optionalString(args, "name");
    unwrap(await<void>([&](auto done) {
        mClient.moveNode(handle, parent.handle, parent.isRoot, name, std::move(done));
    }));
    return Json::object();
}

Json Service::copy(const Json& args)
{
    requireReady();
    const std::uint64_t handle = requireHandle(args, "handle");
    const FolderRef parent = optionalFolder(args, "parent");
    const std::string name = optionalString(args, "name");
    unwrap(await<void>([&](auto done) {
        mClient.copyNode(handle, parent.handle, parent.isRoot, name, std::move(done));
    }));
    return Json::object();
}

Json Service::trash(const Json& args)
{
    requireReady();
    const std::uint64_t handle = requireHandle(args, "handle");
    unwrap(await<void>([&](auto done) { mClient.moveToRubbish(handle, std::move(done)); }));
    return Json::object();
}

Json Service::upload(const Json& args, const Emit& emit)
{
    requireReady();
    const std::string local = requireString(args, "local");
    const FolderRef parent = optionalFolder(args, "parent");
    const std::uint64_t id = mNextTransferId++;
    ProgressRelay relay(emit, [this, id] { mClient.cancelUpload(id); });
    const UploadOutcome outcome = unwrap(await<UploadOutcome>([&](auto done) {
        mClient.upload(local, parent.handle, parent.isRoot, id, std::ref(relay), std::move(done));
    }));
    return {{"handle", handleToBase64(outcome.nodeHandle)}};
}

Json Service::download(const Json& args, const Emit& emit)
{
    requireReady();
    const std::uint64_t handle = requireHandle(args, "handle");
    const std::string local = requireString(args, "local");
    const std::uint64_t id = mNextTransferId++;
    ProgressRelay relay(emit, [this, id] { mClient.cancelDownload(id); });
    const DownloadOutcome outcome = unwrap(await<DownloadOutcome>([&](auto done) {
        mClient.download(handle, local, id, std::ref(relay), std::move(done));
    }));
    return {{"local", outcome.localPath}};
}
