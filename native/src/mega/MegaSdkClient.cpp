#include "MegaSdkClient.h"

#include "app/Logging.h"
#include "core/AccountPlan.h"
#include "core/MegaErrorCodes.h"
#include "MegaSdkListeners.h"
#include "MegaSdkLogger.h"
#include "SerialExecutor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <megaapi.h>
#include <optional>
#include <utility>

// Keeps MegaErrorCodes.h's mirror in sync with the SDK's real values. This is the
// only file that can see both headers -- src/core and src/qml can't include
// megaapi.h.
static_assert(MegaErrorCode::kEInternal == mega::MegaError::API_EINTERNAL,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEArgs == mega::MegaError::API_EARGS, "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEAgain == mega::MegaError::API_EAGAIN,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEFailed == mega::MegaError::API_EFAILED,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kETooMany == mega::MegaError::API_ETOOMANY,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEExpired == mega::MegaError::API_EEXPIRED,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kECircular == mega::MegaError::API_ECIRCULAR,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kENoEnt == mega::MegaError::API_ENOENT,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEAccess == mega::MegaError::API_EACCESS,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEExist == mega::MegaError::API_EEXIST,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEIncomplete == mega::MegaError::API_EINCOMPLETE,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kESid == mega::MegaError::API_ESID, "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEBlocked == mega::MegaError::API_EBLOCKED,
              "MegaErrorCodes.h out of sync");
static_assert(MegaErrorCode::kEMfaRequired == mega::MegaError::API_EMFAREQUIRED,
              "MegaErrorCodes.h out of sync");

// Same arrangement for AccountPlan.h's mirror of MegaAccountDetails'
// ACCOUNT_TYPE_* enum.
static_assert(AccountPlan::kFree == mega::MegaAccountDetails::ACCOUNT_TYPE_FREE,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kProI == mega::MegaAccountDetails::ACCOUNT_TYPE_PROI,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kProII == mega::MegaAccountDetails::ACCOUNT_TYPE_PROII,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kProIII == mega::MegaAccountDetails::ACCOUNT_TYPE_PROIII,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kLite == mega::MegaAccountDetails::ACCOUNT_TYPE_LITE,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kStarter == mega::MegaAccountDetails::ACCOUNT_TYPE_STARTER,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kBasic == mega::MegaAccountDetails::ACCOUNT_TYPE_BASIC,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kEssential == mega::MegaAccountDetails::ACCOUNT_TYPE_ESSENTIAL,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kBusiness == mega::MegaAccountDetails::ACCOUNT_TYPE_BUSINESS,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kProFlexi == mega::MegaAccountDetails::ACCOUNT_TYPE_PRO_FLEXI,
              "AccountPlan.h out of sync");
static_assert(AccountPlan::kFeature == mega::MegaAccountDetails::ACCOUNT_TYPE_FEATURE,
              "AccountPlan.h out of sync");

namespace
{

// What every method reports once shutdown() has run. Nothing surfaces it: the only
// caller left is a teardown callback re-entering from the SDK thread, whose GUI
// event loop has already stopped.
constexpr char kShutDownMessage[] = "the MEGA client has been shut down";

// Its errorCode. Positive so it can never collide with an SDK value, and so it
// falls through every `switch` that branches on one -- see MegaErrorCodes.h for
// the ledger of allocated positive sentinels.
constexpr int kClientShutDownCode = 2;

int toMegaUserAttribute(UserAttribute attribute)
{
    switch (attribute)
    {
        case UserAttribute::LastName:
            return mega::MegaApi::USER_ATTR_LASTNAME;
        case UserAttribute::FirstName:
        default:
            return mega::MegaApi::USER_ATTR_FIRSTNAME;
    }
}

// The window the "Recent" screen covers. 30 days is the SDK's own recommendation
// for getRecentActionsAsync, kept here so both ways of asking agree if the other
// one is ever added (STUDY_RECENTLY_UPDATED_FILES_SDK_API.md).
constexpr std::int64_t kRecentWindowDays = 30;

std::int64_t recentWindowStart()
{
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
               (now - std::chrono::hours(24 * kRecentWindowDays)).time_since_epoch())
        .count();
}

int toMegaOrder(SortOrder order)
{
    switch (order.key)
    {
        case SortKey::Size:
            return order.ascending ? mega::MegaApi::ORDER_SIZE_ASC : mega::MegaApi::ORDER_SIZE_DESC;
        case SortKey::ModificationTime:
            return order.ascending ? mega::MegaApi::ORDER_MODIFICATION_ASC
                                   : mega::MegaApi::ORDER_MODIFICATION_DESC;
        case SortKey::Name:
        default:
            return order.ascending ? mega::MegaApi::ORDER_DEFAULT_ASC
                                   : mega::MegaApi::ORDER_DEFAULT_DESC;
    }
}

FileEntry nodeToEntry(mega::MegaNode* node)
{
    FileEntry entry;
    entry.name = node->getName() ? node->getName() : "";
    entry.handle = node->getHandle();
    entry.sizeBytes = node->isFile() ? static_cast<std::uint64_t>(node->getSize()) : 0;
    entry.isFolder = node->isFolder();
    // MegaProvider: folders have no modification time (the SDK returns 0), so show when they
    // were created, as MEGAcmd's `ls` does.
    entry.modificationTime = node->isFile() ? node->getModificationTime() : node->getCreationTime();
    entry.hasThumbnail = node->hasThumbnail();
    entry.isFavourite = node->isFavourite();
    entry.isExported = node->isExported();
    entry.creationTime = node->getCreationTime();
    entry.label = node->getLabel();
    return entry;
}

// Copies our SearchFilter onto the SDK's. Every facet is only touched when it is
// narrowing something: MegaSearchFilter's "unset" defaults already mean "match
// everything", and byCategory(FILE_TYPE_DEFAULT) is not the same as leaving it alone.
// thisFolderOnly is deliberately absent: it selects the MegaApi call, not a facet.
void applySearchFilter(mega::MegaSearchFilter& target, const SearchFilter& filter)
{
    switch (filter.nodeType)
    {
        case SearchNodeType::Files:
            target.byNodeType(mega::MegaNode::TYPE_FILE);
            break;
        case SearchNodeType::Folders:
            target.byNodeType(mega::MegaNode::TYPE_FOLDER);
            break;
        case SearchNodeType::Any:
            break;
    }

    // Any category other than DEFAULT makes the SDK return files only, whatever
    // byNodeType said -- so "folders" plus a category is an empty result by design,
    // and the popup greys the category picker out rather than letting that happen.
    int category = mega::MegaApi::FILE_TYPE_DEFAULT;
    switch (filter.category)
    {
        case SearchCategory::Photo:
            category = mega::MegaApi::FILE_TYPE_PHOTO;
            break;
        case SearchCategory::Audio:
            category = mega::MegaApi::FILE_TYPE_AUDIO;
            break;
        case SearchCategory::Video:
            category = mega::MegaApi::FILE_TYPE_VIDEO;
            break;
        case SearchCategory::Document:
            category = mega::MegaApi::FILE_TYPE_DOCUMENT;
            break;
        case SearchCategory::Pdf:
            category = mega::MegaApi::FILE_TYPE_PDF;
            break;
        case SearchCategory::Presentation:
            category = mega::MegaApi::FILE_TYPE_PRESENTATION;
            break;
        case SearchCategory::Spreadsheet:
            category = mega::MegaApi::FILE_TYPE_SPREADSHEET;
            break;
        case SearchCategory::Archive:
            category = mega::MegaApi::FILE_TYPE_ARCHIVE;
            break;
        case SearchCategory::Program:
            category = mega::MegaApi::FILE_TYPE_PROGRAM;
            break;
        case SearchCategory::Other:
            category = mega::MegaApi::FILE_TYPE_OTHERS;
            break;
        case SearchCategory::Any:
            break;
    }
    if (category != mega::MegaApi::FILE_TYPE_DEFAULT)
        target.byCategory(category);

    if (filter.favouritesOnly)
        target.byFavourite(mega::MegaSearchFilter::BOOL_FILTER_ONLY_TRUE);

    int windowDays = 0;
    switch (filter.createdWithin)
    {
        case SearchTimeWindow::PastDay:
            windowDays = 1;
            break;
        case SearchTimeWindow::PastWeek:
            windowDays = 7;
            break;
        case SearchTimeWindow::PastMonth:
            windowDays = 30;
            break;
        case SearchTimeWindow::PastYear:
            windowDays = 365;
            break;
        case SearchTimeWindow::Any:
            break;
    }
    if (windowDays > 0)
    {
        const std::int64_t since =
            static_cast<std::int64_t>(std::time(nullptr)) - windowDays * 24 * 60 * 60;
        // Upper limit 0 rather than "now": MegaSearchFilter ignores a 0 limit, and a
        // literal now would drop nodes whose timestamp is a few seconds ahead of ours.
        target.byCreationTime(since, 0);
    }
}

// Orders a listing the way the SDK's own comparators do, for the one listing that
// cannot ask the SDK to do it: folders before files, then the key. Ties are left
// alone, so a caller that fetched in ORDER_DEFAULT_ASC keeps the SDK's natural-name
// order underneath. Name needs nothing -- the SDK already served that order.
void sortEntriesByKey(std::vector<FileEntry>& entries, SortOrder order)
{
    if (order.key == SortKey::Name)
        return;
    std::stable_sort(
        entries.begin(), entries.end(), [order](const FileEntry& a, const FileEntry& b) {
            if (a.isFolder != b.isFolder)
                return a.isFolder;
            const FileEntry& lhs = order.ascending ? a : b;
            const FileEntry& rhs = order.ascending ? b : a;
            return order.key == SortKey::Size ? lhs.sizeBytes < rhs.sizeBytes
                                              : lhs.modificationTime < rhs.modificationTime;
        });
}

std::vector<FileEntry> nodeListToEntries(mega::MegaNodeList* children)
{
    std::vector<FileEntry> entries;
    entries.reserve(children ? static_cast<size_t>(children->size()) : 0);
    if (children)
    {
        for (int i = 0; i < children->size(); ++i)
        {
            // owned by the list, do not delete
            entries.push_back(nodeToEntry(children->get(i)));
        }
    }
    return entries;
}

} // namespace

MegaSdkClient::MegaSdkClient(std::string basePath, std::string userAgent)
    : mLogger(std::make_unique<MegaSdkLogger>()),
      mApi(std::make_unique<mega::MegaApi>(nullptr, basePath.c_str(), userAgent.c_str())),
      mListingCancelToken(mega::MegaCancelToken::createInstance()),
      mListingPool(std::make_unique<SerialExecutor>())
{
    // Static: addLoggerObject/removeLoggerObject register process-wide, not
    // per-MegaApi-instance. Fine here since the app only ever constructs one
    // MegaSdkClient.
    mega::MegaApi::addLoggerObject(mLogger.get());
}

MegaSdkClient::~MegaSdkClient()
{
    // Before removeLoggerObject, so the SDK's own teardown lines still reach the log.
    shutdown();
    mega::MegaApi::removeLoggerObject(mLogger.get());
}

void MegaSdkClient::shutdown()
{
    if (mShuttingDown.exchange(true))
        return; // main.cpp already called it; the destructor is the second call

    // Cuts the walk already under way short; without it the wait below is bounded by
    // that walk, which on a large account is seconds of a window that is already gone.
    mListingCancelToken->cancel();

    // Before mApi.reset(): a listing worker is the one place that reads mApi from
    // outside the SDK's own threads, and it has no null check that would survive the
    // pointer going away underneath it. This drains the queue as well as the running
    // job, but the flag above makes every queued one bail without touching mApi.
    mListingPool->waitForDone();

    // Before mApi.reset(): ~MegaApi does not stop the libuv server, and a request
    // still in flight there would reach an SDK that is already tearing down.
    if (mHttpServerStarted)
        mApi->httpServerStop();

    // ~MegaApi joins the SDK thread, which runs abortPendingActions() on the way
    // out: every pending request and transfer completes with API_EACCESS, so all our
    // listeners fire here, on the SDK thread, while this call blocks. Those
    // callbacks re-enter this object (services start their next queued job from
    // inside onDone), and reset() nulls the pointer before deleting -- which is why
    // every method checks mShuttingDown.
    mApi.reset();
}

void MegaSdkClient::login(const std::string& email,
                          const std::string& password,
                          std::function<void(Result<void>)> onDone)
{
    // Every public method below must open with this same guard: shutdown() nulls
    // mApi from the GUI thread while the SDK thread is still delivering teardown
    // callbacks that re-enter here. One method missing it is one null dereference.
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->login(
        email.c_str(), password.c_str(), new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::loginWithSession(const std::string& sessionToken,
                                     std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->fastLogin(sessionToken.c_str(), new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::multiFactorAuthLogin(const std::string& email,
                                         const std::string& password,
                                         const std::string& pin,
                                         std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->multiFactorAuthLogin(email.c_str(),
                               password.c_str(),
                               pin.c_str(),
                               new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::logout(std::function<void(Result<void>)> onDone)
{
    // ENABLE_SYNC is a PUBLIC define from the SDK's own CMake and always on here, so
    // the 2-argument overload is the only one that exists -- no #ifdef branch
    // needed. keepSyncConfigsFile=false: this app never syncs.
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->logout(false, new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::localLogout(std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->localLogout(new megasdk::SimpleResultListener(std::move(onDone)));
}

Result<std::string> MegaSdkClient::currentSessionToken() const
{
    if (mShuttingDown)
        return Result<std::string>::fail(kShutDownMessage, kClientShutDownCode);
    char* session = mApi->dumpSession(); // non-const, but callable from a const member via
                                         // unique_ptr's operator->
    if (!session)
    {
        // The only caller saves the token best-effort and silently skips on failure,
        // so without this line the user just gets an unexplained password prompt.
        LOG_WARN("session") << "dumpSession returned null -- session token not persisted";
        return Result<std::string>::fail("not logged in", MegaErrorCode::kEInternal);
    }
    std::string token(session);
    delete[] session; // megaapi.h: "Use delete[] to release the memory"
    return Result<std::string>::ok(std::move(token));
}

Result<std::uint64_t> MegaSdkClient::currentUserHandle() const
{
    if (mShuttingDown)
        return Result<std::uint64_t>::fail(kShutDownMessage, kClientShutDownCode);
    const mega::MegaHandle handle = mApi->getMyUserHandleBinary();
    if (handle == mega::INVALID_HANDLE)
        return Result<std::uint64_t>::fail("not logged in", MegaErrorCode::kEInternal);
    return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(handle));
}

void MegaSdkClient::fetchNodes(
    std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
    std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->fetchNodes(new megasdk::FetchNodesListener(std::move(onProgress), std::move(onDone)));
}

void MegaSdkClient::syncPendingChanges(std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->catchup(new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::getRootChildren(SortOrder order,
                                    std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    listChildren(resolveNode(0, true),
                 "No root node (not logged in / nodes not fetched)",
                 order,
                 std::move(onDone));
}

void MegaSdkClient::getRubbishChildren(SortOrder order,
                                       std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    // Not resolveNode(): its isRoot branch is hardcoded to the Cloud Drive root,
    // and the bin is a third root alongside it and the Vault.
    listChildren(std::unique_ptr<mega::MegaNode>(mApi->getRubbishNode()),
                 "No rubbish node (not logged in / nodes not fetched)",
                 order,
                 std::move(onDone));
}

Result<RestoreTarget> MegaSdkClient::getRestoreTarget(std::uint64_t handle) const
{
    if (mShuttingDown)
        return Result<RestoreTarget>::fail(kShutDownMessage, kClientShutDownCode);

    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
        return Result<RestoreTarget>::fail("No node with the given handle", MegaErrorCode::kENoEnt);

    const mega::MegaHandle restore = node->getRestoreHandle();
    if (restore != mega::INVALID_HANDLE)
    {
        // Recorded when the node was binned, so it can name a folder that has since
        // been deleted itself -- resolving it is the only way to tell.
        std::unique_ptr<mega::MegaNode> parent(mApi->getNodeByHandle(restore));
        if (parent)
        {
            std::unique_ptr<mega::MegaNode> root(mApi->getRootNode());
            const bool isRoot = root && restore == root->getHandle();
            return Result<RestoreTarget>::ok(
                RestoreTarget{isRoot ? 0u : static_cast<std::uint64_t>(restore), isRoot, false});
        }
    }

    return Result<RestoreTarget>::ok(RestoreTarget{0, true, true});
}

void MegaSdkClient::getChildren(std::uint64_t handle,
                                SortOrder order,
                                std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    listChildren(
        resolveNode(handle, false),
        "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
        order,
        std::move(onDone));
}

void MegaSdkClient::search(std::uint64_t ancestorHandle,
                           bool isRoot,
                           const std::string& query,
                           const SearchFilter& searchFilter,
                           SortOrder order,
                           std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    runOffThread(
        [this, ancestorHandle, isRoot, query, searchFilter, order]()
            -> Result<std::vector<FileEntry>> {
            std::unique_ptr<mega::MegaNode> ancestor = resolveNode(ancestorHandle, isRoot);
            if (!ancestor)
                return Result<std::vector<FileEntry>>::fail(
                    "No ancestor node for search (not logged in / nodes not fetched / invalid "
                    "handle)",
                    MegaErrorCode::kENoEnt);

            std::unique_ptr<mega::MegaSearchFilter> filter(
                mega::MegaSearchFilter::createInstance());
            // Left unset when empty, as in listFavourites(): an advanced-search filter with
            // no typed query is a valid search, and the name predicate must not narrow it.
            if (!query.empty())
                filter->byName(query.c_str());
            filter->byLocationHandle(ancestor->getHandle());
            applySearchFilter(*filter, searchFilter);

            // Recursion is chosen by which call takes the filter, not by anything on it:
            // MegaApi::search walks the subtree, getChildren stops at the location handle.
            std::unique_ptr<mega::MegaNodeList> results(
                searchFilter.thisFolderOnly
                    ? mApi->getChildren(filter.get(), toMegaOrder(order), mListingCancelToken.get())
                    : mApi->search(filter.get(), toMegaOrder(order), mListingCancelToken.get()));
            return Result<std::vector<FileEntry>>::ok(nodeListToEntries(results.get()));
        },
        std::move(onDone));
}

void MegaSdkClient::runOffThread(std::function<Result<std::vector<FileEntry>>()> work,
                                 std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    mListingPool->start([this, work = std::move(work), onDone = std::move(onDone)]() mutable {
        Result<std::vector<FileEntry>> result =
            mShuttingDown
                ? Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode)
                : work();
        // Re-checked after work(): a walk cut short by mListingCancelToken hands back a
        // truncated node list through the ordinary return, with no error channel of its
        // own, so without this a cancelled listing is indistinguishable from a complete
        // one. Today nothing drains the queue past shutdown(), but that is a property of
        // main.cpp's ordering, not of this class.
        if (mShuttingDown)
            result = Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode);
        // MegaProvider: delivered on the worker thread itself. The host has no event
        // loop to post back to, and its callers only wait on the result.
        onDone(std::move(result));
    });
}

void MegaSdkClient::listFavourites(SortOrder order,
                                   const std::string& nameFilter,
                                   const SearchFilter& searchFilter,
                                   std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    runOffThread(
        [this, order, nameFilter, searchFilter]() -> Result<std::vector<FileEntry>> {
            std::unique_ptr<mega::MegaNode> root = resolveNode(0, true);
            if (!root)
                return Result<std::vector<FileEntry>>::fail(
                    "No root node (not logged in / nodes not fetched)", MegaErrorCode::kENoEnt);

            std::unique_ptr<mega::MegaSearchFilter> filter(
                mega::MegaSearchFilter::createInstance());
            filter->byFavourite(mega::MegaSearchFilter::BOOL_FILTER_ONLY_TRUE);
            filter->byLocationHandle(root->getHandle());
            // Left unset when empty on purpose: MegaSearchFilter's name predicate is
            // skipped entirely for an empty pattern, so byName("") would not mean
            // "match nothing".
            if (!nameFilter.empty())
                filter->byName(nameFilter.c_str());
            applySearchFilter(*filter, searchFilter);

            std::unique_ptr<mega::MegaNodeList> results(
                mApi->search(filter.get(), toMegaOrder(order), mListingCancelToken.get()));
            return Result<std::vector<FileEntry>>::ok(nodeListToEntries(results.get()));
        },
        std::move(onDone));
}

void MegaSdkClient::listRecent(SortOrder order,
                               const std::string& nameFilter,
                               const SearchFilter& searchFilter,
                               std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    runOffThread(
        [this, order, nameFilter, searchFilter]() -> Result<std::vector<FileEntry>> {
            std::unique_ptr<mega::MegaNode> root = resolveNode(0, true);
            if (!root)
                return Result<std::vector<FileEntry>>::fail(
                    "No root node (not logged in / nodes not fetched)", MegaErrorCode::kENoEnt);

            std::unique_ptr<mega::MegaSearchFilter> filter(
                mega::MegaSearchFilter::createInstance());
            filter->byLocationHandle(root->getHandle());
            // Left unset when empty, for listFavourites' reason.
            if (!nameFilter.empty())
                filter->byName(nameFilter.c_str());
            applySearchFilter(*filter, searchFilter);

            // Both facets below are ones applySearchFilter also writes, so they are
            // set after it: what this listing means has to win over the popup's
            // selection. Upper limit 0, which MegaSearchFilter reads as "no bound on
            // that side" rather than as the epoch -- a clock skewed ahead on the
            // uploading device must not hide a node from this listing. The later of
            // the two lower bounds wins, so "past 24 hours" narrows the window but
            // "past year" cannot widen it.
            filter->byCreationTime(
                std::max(recentWindowStart(), filter->byCreationTimeLowerLimit()), 0);
            // Files only: this is a listing of recently added files, and a folder's
            // creation time would drag its whole subtree's container into it.
            filter->byNodeType(mega::MegaNode::TYPE_FILE);

            std::unique_ptr<mega::MegaNodeList> results(
                mApi->search(filter.get(), toMegaOrder(order), mListingCancelToken.get()));
            return Result<std::vector<FileEntry>>::ok(nodeListToEntries(results.get()));
        },
        std::move(onDone));
}

void MegaSdkClient::listPublicLinks(SortOrder order,
                                    const std::string& nameFilter,
                                    const SearchFilter& searchFilter,
                                    std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    runOffThread(
        [this, order, nameFilter, searchFilter]() -> Result<std::vector<FileEntry>> {
            std::unique_ptr<mega::MegaNode> root = resolveNode(0, true);
            if (!root)
                return Result<std::vector<FileEntry>>::fail(
                    "No root node (not logged in / nodes not fetched)", MegaErrorCode::kENoEnt);

            if (!nameFilter.empty() || !searchFilter.isDefault())
            {
                // MegaSearchFilter has no "is exported" facet, so the narrowing to
                // linked nodes is the only part of this that cannot stay server-side.
                std::unique_ptr<mega::MegaSearchFilter> filter(
                    mega::MegaSearchFilter::createInstance());
                filter->byLocationHandle(root->getHandle());
                // Left unset when empty, for listFavourites' reason.
                if (!nameFilter.empty())
                    filter->byName(nameFilter.c_str());
                applySearchFilter(*filter, searchFilter);

                std::unique_ptr<mega::MegaNodeList> results(
                    mApi->search(filter.get(), toMegaOrder(order), mListingCancelToken.get()));
                std::vector<FileEntry> entries = nodeListToEntries(results.get());
                entries.erase(std::remove_if(entries.begin(),
                                             entries.end(),
                                             [](const FileEntry& entry) {
                                                 return !entry.isExported;
                                             }),
                              entries.end());
                return Result<std::vector<FileEntry>>::ok(std::move(entries));
            }

            // ORDER_DEFAULT_ASC even when the caller asked for size or time:
            // getPublicLinks rejects those two orders, and fetching in the default one
            // leaves sortEntriesByKey's ties in the SDK's natural-name order.
            const int sdkOrder =
                order.key == SortKey::Name ? toMegaOrder(order) : mega::MegaApi::ORDER_DEFAULT_ASC;
            std::unique_ptr<mega::MegaNodeList> results(mApi->getPublicLinks(sdkOrder));
            std::vector<FileEntry> entries;
            entries.reserve(results ? static_cast<std::size_t>(results->size()) : 0);
            for (int i = 0; results && i < results->size(); ++i)
            {
                // getPublicLinks spans every root, unlike the rooted search above.
                // Without this a link left on a binned node would appear here and then
                // vanish the moment anything was typed into the search box.
                mega::MegaNode* node = results->get(i); // owned by the list
                if (node && mApi->isInCloud(node))
                    entries.push_back(nodeToEntry(node));
            }
            sortEntriesByKey(entries, order);
            return Result<std::vector<FileEntry>>::ok(std::move(entries));
        },
        std::move(onDone));
}

void MegaSdkClient::download(std::uint64_t handle,
                             const std::string& destinationPath,
                             std::uint64_t transferId,
                             std::function<void(std::uint64_t, std::uint64_t)> onProgress,
                             std::function<void(Result<DownloadOutcome>)> onDone)
{
    // Wrapped before any early return so every exit path forgets the id, whether or
    // not a token was ever made for it.
    auto forgetThenReport =
        [this, transferId, onDone = std::move(onDone)](Result<DownloadOutcome> result) {
            {
                std::lock_guard<std::mutex> lock(mCancelTokenMutex);
                mDownloadCancelTokens.erase(transferId);
            }
            onDone(std::move(result)); // outside the guard: it re-enters download()
        };

    if (mShuttingDown)
    {
        forgetThenReport(Result<DownloadOutcome>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        forgetThenReport(Result<DownloadOutcome>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    auto* listener =
        new megasdk::DownloadListener(std::move(onProgress), std::move(forgetThenReport));

    mega::MegaCancelToken* cancelTokenRaw = nullptr;
    {
        std::lock_guard<std::mutex> lock(mCancelTokenMutex);
        auto& slot = mDownloadCancelTokens[transferId];
        slot.reset(mega::MegaCancelToken::createInstance());
        cancelTokenRaw = slot.get();
    }

    mApi->startDownload(node.get(),
                        destinationPath.c_str(),
                        /*customName*/ nullptr,
                        /*appData*/ nullptr,
                        /*startFirst*/ false,
                        cancelTokenRaw,
                        // ASSUMEDIFFERENT, not FINGERPRINT: the check decides whether the
                        // SDK skips an identical file outright, and a skip writes nothing,
                        // so the user who asked for a download gets no new file. Assuming
                        // difference always downloads and lets NEW_WITH_N suffix "(1)".
                        mega::MegaTransfer::COLLISION_CHECK_ASSUMEDIFFERENT,
                        mega::MegaTransfer::COLLISION_RESOLUTION_NEW_WITH_N,
                        /*undelete*/ false,
                        listener);
}

void MegaSdkClient::upload(const std::string& localPath,
                           std::uint64_t parentHandle,
                           bool parentIsRoot,
                           std::uint64_t transferId,
                           std::function<void(std::uint64_t, std::uint64_t)> onProgress,
                           std::function<void(Result<UploadOutcome>)> onDone)
{
    // Same wrapper as download() -- see there.
    auto forgetThenReport =
        [this, transferId, onDone = std::move(onDone)](Result<UploadOutcome> result) {
            {
                std::lock_guard<std::mutex> lock(mCancelTokenMutex);
                mUploadCancelTokens.erase(transferId);
            }
            onDone(std::move(result));
        };

    if (mShuttingDown)
    {
        forgetThenReport(Result<UploadOutcome>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> parent = resolveNode(parentHandle, parentIsRoot);
    if (!parent)
    {
        forgetThenReport(
            Result<UploadOutcome>::fail("No destination folder with the given handle (nodes not "
                                        "fetched / folder deleted)",
                                        MegaErrorCode::kENoEnt));
        return;
    }

    auto* listener =
        new megasdk::UploadListener(std::move(onProgress), std::move(forgetThenReport));

    mega::MegaCancelToken* cancelTokenRaw = nullptr;
    {
        std::lock_guard<std::mutex> lock(mCancelTokenMutex);
        auto& slot = mUploadCancelTokens[transferId];
        slot.reset(mega::MegaCancelToken::createInstance());
        cancelTokenRaw = slot.get();
    }

    // options == nullptr means all defaults; megaapi.cpp only copies the struct when
    // it is non-null, so there is nothing to construct here.
    mApi->startUpload(localPath, parent.get(), cancelTokenRaw, /*options*/ nullptr, listener);
}

void MegaSdkClient::cancelDownload(std::uint64_t transferId)
{
    std::lock_guard<std::mutex> lock(mCancelTokenMutex);
    auto it = mDownloadCancelTokens.find(transferId);
    if (it != mDownloadCancelTokens.end() && it->second)
        it->second->cancel();
}

void MegaSdkClient::cancelUpload(std::uint64_t transferId)
{
    // No mShuttingDown guard on either: the token is ours, not the SDK's, so setting
    // it never re-enters mApi.
    std::lock_guard<std::mutex> lock(mCancelTokenMutex);
    auto it = mUploadCancelTokens.find(transferId);
    if (it != mUploadCancelTokens.end() && it->second)
        it->second->cancel();
}

void MegaSdkClient::getThumbnail(std::uint64_t handle,
                                 const std::string& destinationPath,
                                 std::function<void(Result<std::string>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<std::string>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    // Safe to let node die on return: getNodeAttribute copies what it needs
    // into the request before queueing it.
    mApi->getThumbnail(
        node.get(), destinationPath.c_str(), new megasdk::AttributeFileListener(std::move(onDone)));
}

void MegaSdkClient::getPreview(std::uint64_t handle,
                               const std::string& destinationPath,
                               std::function<void(Result<std::string>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<std::string>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    // Same TYPE_GET_ATTR_FILE request as getThumbnail above, so the listener is
    // reused as-is; only the attribute type differs.
    mApi->getPreview(
        node.get(), destinationPath.c_str(), new megasdk::AttributeFileListener(std::move(onDone)));
}

void MegaSdkClient::readFileContent(std::uint64_t handle,
                                    std::uint64_t maxBytes,
                                    std::function<void(Result<std::vector<char>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<char>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<std::vector<char>>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    // Without this the SDK aborts a streaming transfer with API_EAGAIN whenever it
    // decides the rate is too low -- which a few tens of kilobytes easily looks like.
    mApi->setStreamingMinimumRate(0);
    mApi->startStreaming(node.get(),
                         0,
                         node->getSize(),
                         new megasdk::StreamingContentListener(maxBytes, std::move(onDone)));
}

// The range actually readable from a file node, or nullopt when offset is at or past
// its end. getSize() is negative for anything that isn't a file, so the signed
// compare has to happen before the width cast. Non-const: getSize() is not const.
static std::optional<std::uint64_t>
readableRangeLength(mega::MegaNode& node, std::uint64_t offset, std::uint64_t length)
{
    const std::int64_t size = node.getSize();
    if (size <= 0 || offset >= static_cast<std::uint64_t>(size))
        return std::nullopt;
    return std::min<std::uint64_t>(length, static_cast<std::uint64_t>(size) - offset);
}

void MegaSdkClient::readFileRange(std::uint64_t handle,
                                  std::uint64_t offset,
                                  std::uint64_t length,
                                  std::function<void(Result<std::vector<char>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<char>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<std::vector<char>>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    const std::optional<std::uint64_t> clamped = readableRangeLength(*node, offset, length);
    if (!clamped)
    {
        onDone(Result<std::vector<char>>::fail("Range starts at or past the end of the file",
                                               MegaErrorCode::kEArgs));
        return;
    }
    if (*clamped == 0)
    {
        onDone(Result<std::vector<char>>::ok({}));
        return;
    }

    // Same rate-check trap as readFileContent: without this a few tens of kilobytes
    // are read as "too slow" and the transfer dies with API_EAGAIN.
    mApi->setStreamingMinimumRate(0);
    mApi->startStreaming(node.get(),
                         static_cast<std::int64_t>(offset),
                         static_cast<std::int64_t>(*clamped),
                         new megasdk::StreamingContentListener(*clamped, std::move(onDone)));
}

void MegaSdkClient::readFileRangeStreamed(std::uint64_t handle,
                                          std::uint64_t offset,
                                          std::uint64_t length,
                                          std::function<bool(const char*, std::size_t)> onChunk,
                                          std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    const std::optional<std::uint64_t> clamped = readableRangeLength(*node, offset, length);
    if (!clamped)
    {
        onDone(Result<void>::fail("Range starts at or past the end of the file",
                                  MegaErrorCode::kEArgs));
        return;
    }
    if (*clamped == 0)
    {
        onDone(Result<void>::ok());
        return;
    }

    // The rate-check trap again, and here it bites harder: a receiver that inflates
    // and writes each piece slows the transfer down by exactly what the check measures.
    mApi->setStreamingMinimumRate(0);
    mApi->startStreaming(
        node.get(),
        static_cast<std::int64_t>(offset),
        static_cast<std::int64_t>(*clamped),
        new megasdk::StreamingChunkListener(std::move(onChunk), std::move(onDone)));
}

Result<std::string> MegaSdkClient::streamingUrl(std::uint64_t handle)
{
    if (mShuttingDown)
        return Result<std::string>::fail(kShutDownMessage, kClientShutDownCode);

    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
        return Result<std::string>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt);

    if (!mHttpServerStarted)
    {
        // port 0 asks for a free one. The SDK's default of 4443 fails when another
        // process already holds it, and the failure is a bare false.
        if (!mApi->httpServerStart(true, 0, false))
            return Result<std::string>::fail("Could not start the local HTTP server",
                                             MegaErrorCode::kEInternal);

        // Same rate-check trap as readFileRange: the server reads through the same
        // streaming transfers, so the SDK's default minimum rate aborts a slow one
        // mid-way with API_EAGAIN.
        mApi->setStreamingMinimumRate(0);
        mHttpServerStarted = true;
    }

    char* link = mApi->httpServerGetLocalLink(node.get());
    if (!link)
        return Result<std::string>::fail("Could not build a local link for the node",
                                         MegaErrorCode::kEInternal);
    std::string url(link);
    delete[] link; // megaapi.h: "Use delete[] to release the memory"
    return Result<std::string>::ok(std::move(url));
}

void MegaSdkClient::getPath(std::uint64_t handle,
                            bool isRoot,
                            std::function<void(Result<std::vector<PathSegment>>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::vector<PathSegment>>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, isRoot);
    if (!node)
    {
        onDone(Result<std::vector<PathSegment>>::fail(
            "No node with the given handle (not logged in / nodes not fetched / invalid handle)",
            MegaErrorCode::kENoEnt));
        return;
    }

    std::vector<PathSegment> segments;
    while (node)
    {
        PathSegment segment;
        segment.name = node->getName() ? node->getName() : "";
        segment.handle = static_cast<std::uint64_t>(node->getHandle());
        segments.push_back(std::move(segment));
        // MegaNode has no getParentNode() of its own -- it's MegaApi's, and
        // returns NULL both for a not-found node and for an actual root node
        // (megaapi.h's doc comment on MegaApi::getParentNode).
        node = std::unique_ptr<mega::MegaNode>(mApi->getParentNode(node.get()));
    }

    std::reverse(segments.begin(), segments.end());
    // The walk stops at whichever root the node lives under, and the Rubbish bin is
    // a root alongside the Cloud Drive. Without recording which one it was, the two
    // collapse into the same "isRoot" segment below and a binned folder reports
    // itself as living in the Drive.
    std::unique_ptr<mega::MegaNode> rubbish(mApi->getRubbishNode());
    if (rubbish && segments.front().handle == static_cast<std::uint64_t>(rubbish->getHandle()))
    {
        // Every segment, not just the top one: FolderNavigationController reads the
        // *last* segment's kind to decide what the screen allows, so tagging only
        // the root would leave a folder inside the bin claiming to be a Cloud Drive
        // folder -- and offering "Move to Rubbish" on already-binned nodes.
        for (PathSegment& segment : segments)
            segment.kind = ViewKind::Rubbish;
    }
    segments.front().isRoot = true;
    segments.front().handle = 0;

    onDone(Result<std::vector<PathSegment>>::ok(std::move(segments)));
}

void MegaSdkClient::getNodeInfo(std::uint64_t handle, std::function<void(Result<NodeInfo>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<NodeInfo>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<NodeInfo>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    NodeInfo info;
    info.name = node->getName() ? node->getName() : "";
    info.handle = static_cast<std::uint64_t>(node->getHandle());
    info.isFolder = node->isFolder();
    // A deleted node still resolves -- it lives under the Rubbish bin now -- so this
    // is the only reliable "still usable" test.
    info.inCloud = mApi->isInCloud(node.get());

    onDone(Result<NodeInfo>::ok(std::move(info)));
}

void MegaSdkClient::getFolderInfo(std::uint64_t handle,
                                  bool isRoot,
                                  std::function<void(Result<FolderInfo>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<FolderInfo>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, isRoot);
    if (!node)
    {
        onDone(Result<FolderInfo>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }
    // Checked here rather than left to the SDK: getFolderInfo on a file finishes
    // with API_OK and an all-zero MegaFolderInfo, which reads as an empty folder.
    if (!node->isFolder())
    {
        onDone(Result<FolderInfo>::fail("Not a folder", MegaErrorCode::kEArgs));
        return;
    }

    mApi->getFolderInfo(node.get(), new megasdk::FolderInfoListener(std::move(onDone)));
}

void MegaSdkClient::renameNode(std::uint64_t handle,
                               const std::string& newName,
                               std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    mApi->renameNode(
        node.get(), newName.c_str(), new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::moveToRubbish(std::uint64_t handle, std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    // Deleting in MEGA is a move to the Rubbish bin, not MegaApi::remove().
    std::unique_ptr<mega::MegaNode> rubbish(mApi->getRubbishNode());
    if (!rubbish)
    {
        onDone(Result<void>::fail("Rubbish bin not available (nodes not fetched)",
                                  MegaErrorCode::kENoEnt));
        return;
    }

    mApi->moveNode(node.get(), rubbish.get(), new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::removeNode(std::uint64_t handle, std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    mApi->remove(node.get(), new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::cleanRubbishBin(std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }

    // Succeeds on an already-empty bin, so the caller needs no emptiness pre-check.
    mApi->cleanRubbishBin(new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::setNodeFavourite(std::uint64_t handle,
                                     bool favourite,
                                     std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    mApi->setNodeFavourite(
        node.get(), favourite, new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::exportNode(std::uint64_t handle,
                               std::function<void(Result<std::string>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<std::string>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    // The plain read-only link, which is the only one the UI offers: not writable and
    // not MEGA-hosted -- the writable variants also hand back a key the caller would
    // have to keep, and nothing here does.
    //
    // The expiry is echoed back rather than passed as 0: exportNode re-issues the
    // link whenever the value differs from the stored one, so a hardcoded "never"
    // here would strip a date set elsewhere every time the link was merely read.
    // getExpirationTime answers -1 for a node that has no link yet, which is not a
    // value exportNode accepts.
    const std::int64_t keepExpiry = std::max<std::int64_t>(node->getExpirationTime(), 0);
    mApi->exportNode(
        node.get(), keepExpiry, false, false, new megasdk::LinkResultListener(std::move(onDone)));
}

void MegaSdkClient::setLinkExpiry(std::uint64_t handle,
                                  std::int64_t expireTime,
                                  std::function<void(Result<std::string>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<std::string>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    mApi->exportNode(
        node.get(), expireTime, false, false, new megasdk::LinkResultListener(std::move(onDone)));
}

Result<std::int64_t> MegaSdkClient::getLinkExpiry(std::uint64_t handle) const
{
    if (mShuttingDown)
        return Result<std::int64_t>::fail(kShutDownMessage, kClientShutDownCode);

    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
        return Result<std::int64_t>::fail("No node with the given handle", MegaErrorCode::kENoEnt);

    return Result<std::int64_t>::ok(node->getExpirationTime());
}

Result<LinkDetails> MegaSdkClient::getLinkDetails(std::uint64_t handle) const
{
    if (mShuttingDown)
        return Result<LinkDetails>::fail(kShutDownMessage, kClientShutDownCode);

    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
        return Result<LinkDetails>::fail("No node with the given handle", MegaErrorCode::kENoEnt);

    LinkDetails details;
    details.exported = node->isExported();
    if (!details.exported)
        return Result<LinkDetails>::ok(details);
    std::unique_ptr<char[]> url(node->getPublicLink(true));
    details.url = url ? url.get() : "";
    details.created = node->getPublicLinkCreationTime();
    details.expires = std::max<std::int64_t>(node->getExpirationTime(), 0);
    details.expired = node->isExpired();
    details.takenDown = node->isTakenDown();
    return Result<LinkDetails>::ok(details);
}

void MegaSdkClient::encryptLinkWithPassword(const std::string& link,
                                            const std::string& password,
                                            std::function<void(Result<std::string>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->encryptLinkWithPassword(
        link.c_str(), password.c_str(), new megasdk::TextResultListener(std::move(onDone)));
}

void MegaSdkClient::disableExport(std::uint64_t handle, std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail(
            "No node with the given handle (not logged in / nodes not fetched / node deleted)",
            MegaErrorCode::kENoEnt));
        return;
    }

    // A node with no link is already where the caller wants it; the SDK would answer
    // this with an error, which would put a failure toast on screen for a no-op.
    if (!node->isExported())
    {
        onDone(Result<void>::ok());
        return;
    }

    mApi->disableExport(node.get(), new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::moveNode(std::uint64_t handle,
                             std::uint64_t newParentHandle,
                             bool newParentIsRoot,
                             const std::string& newName,
                             std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail("No node with the given handle (not logged in / nodes not "
                                  "fetched / node deleted)",
                                  MegaErrorCode::kENoEnt));
        return;
    }

    std::unique_ptr<mega::MegaNode> parent = resolveNode(newParentHandle, newParentIsRoot);
    if (!parent)
    {
        onDone(Result<void>::fail("No destination folder with the given handle (nodes not "
                                  "fetched / folder deleted)",
                                  MegaErrorCode::kENoEnt));
        return;
    }

    // Same split as copyNode below, and for the same reason: the named overload
    // pushes newName through as an attribute update, so "keep the name" has to go
    // through the unnamed one rather than through an empty string.
    if (newName.empty())
        mApi->moveNode(
            node.get(), parent.get(), new megasdk::SimpleResultListener(std::move(onDone)));
    else
        mApi->moveNode(node.get(),
                       parent.get(),
                       newName.c_str(),
                       new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::copyNode(std::uint64_t handle,
                             std::uint64_t newParentHandle,
                             bool newParentIsRoot,
                             const std::string& newName,
                             std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
    {
        onDone(Result<void>::fail("No node with the given handle (not logged in / nodes not "
                                  "fetched / node deleted)",
                                  MegaErrorCode::kENoEnt));
        return;
    }

    std::unique_ptr<mega::MegaNode> parent = resolveNode(newParentHandle, newParentIsRoot);
    if (!parent)
    {
        onDone(Result<void>::fail("No destination folder with the given handle (nodes not "
                                  "fetched / folder deleted)",
                                  MegaErrorCode::kENoEnt));
        return;
    }

    // The branch is load-bearing, not stylistic: the named overload rejects an
    // empty string with API_EARGS, so "keep the source name" has to go through
    // the unnamed one.
    if (newName.empty())
        mApi->copyNode(
            node.get(), parent.get(), new megasdk::SimpleResultListener(std::move(onDone)));
    else
        mApi->copyNode(node.get(),
                       parent.get(),
                       newName.c_str(),
                       new megasdk::SimpleResultListener(std::move(onDone)));
}

void MegaSdkClient::createFolder(std::uint64_t parentHandle,
                                 bool parentIsRoot,
                                 const std::string& name,
                                 std::function<void(Result<void>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<void>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    std::unique_ptr<mega::MegaNode> parent = resolveNode(parentHandle, parentIsRoot);
    if (!parent)
    {
        onDone(Result<void>::fail("No parent folder with the given handle (nodes not "
                                  "fetched / folder deleted)",
                                  MegaErrorCode::kENoEnt));
        return;
    }

    // No pre-check for an existing same-named folder: the API answers that
    // itself with API_EEXIST (see IMegaClient::createFolder).
    mApi->createFolder(
        name.c_str(), parent.get(), new megasdk::SimpleResultListener(std::move(onDone)));
}

Result<void> MegaSdkClient::checkMove(std::uint64_t handle,
                                      std::uint64_t newParentHandle,
                                      bool newParentIsRoot) const
{
    if (mShuttingDown)
        return Result<void>::fail(kShutDownMessage, kClientShutDownCode);
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    std::unique_ptr<mega::MegaNode> parent = resolveNode(newParentHandle, newParentIsRoot);
    if (!node || !parent)
        return Result<void>::fail("Source or destination no longer exists", MegaErrorCode::kENoEnt);

    // Stricter than the SDK, which accepts a move to where the node already is.
    // Detected here because this is the only layer holding real nodes: a caller
    // pointing at the root has the isRoot sentinel, never the root's actual handle.
    if (node->getParentHandle() == parent->getHandle())
        return Result<void>::fail("Already in that folder", MegaErrorCode::kEArgs);

    // Ownership of the returned MegaError is the caller's (megaapi.h's
    // checkMoveErrorExtended docs), unlike the borrowed MegaError* handed to
    // MegaRequestListener::onRequestFinish.
    std::unique_ptr<mega::MegaError> error(mApi->checkMoveErrorExtended(node.get(), parent.get()));
    if (!error || error->getErrorCode() == mega::MegaError::API_OK)
        return Result<void>::ok();

    return Result<void>::fail(error->getErrorString(), error->getErrorCode());
}

Result<void> MegaSdkClient::checkUpload(std::uint64_t parentHandle, bool parentIsRoot) const
{
    if (mShuttingDown)
        return Result<void>::fail(kShutDownMessage, kClientShutDownCode);
    std::unique_ptr<mega::MegaNode> parent = resolveNode(parentHandle, parentIsRoot);
    if (!parent)
        return Result<void>::fail("Destination folder no longer exists", MegaErrorCode::kENoEnt);

    if (!parent->isFolder())
        return Result<void>::fail("Destination is not a folder", MegaErrorCode::kEArgs);

    // A deleted folder still resolves -- it lives under the Rubbish bin now -- so
    // this is the only reliable "still usable destination" test.
    if (!mApi->isInCloud(parent.get()))
        return Result<void>::fail("Destination folder is no longer in the Cloud Drive",
                                  MegaErrorCode::kENoEnt);

    if (mApi->getAccess(parent.get()) < mega::MegaShare::ACCESS_READWRITE)
        return Result<void>::fail("No permission to add files to that folder",
                                  MegaErrorCode::kEAccess);

    return Result<void>::ok();
}

Result<std::vector<FileEntry>>
MegaSdkClient::findChildrenOfType(std::uint64_t parentHandle,
                                  bool parentIsRoot,
                                  const std::vector<std::string>& names,
                                  int nodeType) const
{
    if (mShuttingDown)
        return Result<std::vector<FileEntry>>::fail(kShutDownMessage, kClientShutDownCode);
    std::unique_ptr<mega::MegaNode> parent = resolveNode(parentHandle, parentIsRoot);
    if (!parent)
        return Result<std::vector<FileEntry>>::fail("Destination folder no longer exists",
                                                    MegaErrorCode::kENoEnt);

    std::vector<FileEntry> hits;
    for (const std::string& name : names)
    {
        // getChildNodeOfType, not getChildNode(), which prefers a same-named
        // folder whichever type the caller asked about. It answers from memory
        // or local SQLite, so this loop never goes to the network.
        std::unique_ptr<mega::MegaNode> child(
            mApi->getChildNodeOfType(parent.get(), name.c_str(), nodeType));
        if (child)
            hits.push_back(nodeToEntry(child.get()));
    }
    return Result<std::vector<FileEntry>>::ok(std::move(hits));
}

Result<std::vector<FileEntry>> MegaSdkClient::findChildFiles(
    std::uint64_t parentHandle, bool parentIsRoot, const std::vector<std::string>& names) const
{
    return findChildrenOfType(parentHandle, parentIsRoot, names, mega::MegaNode::TYPE_FILE);
}

Result<std::vector<FileEntry>> MegaSdkClient::findChildFolders(
    std::uint64_t parentHandle, bool parentIsRoot, const std::vector<std::string>& names) const
{
    return findChildrenOfType(parentHandle, parentIsRoot, names, mega::MegaNode::TYPE_FOLDER);
}

Result<bool> MegaSdkClient::siblingNameTaken(std::uint64_t handle, const std::string& name) const
{
    if (mShuttingDown)
        return Result<bool>::fail(kShutDownMessage, kClientShutDownCode);
    // isRoot false unconditionally: the root has no parent to hold a sibling, and
    // nothing offers to rename it.
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, false);
    if (!node)
        return Result<bool>::fail("No node with the given handle", MegaErrorCode::kENoEnt);

    std::unique_ptr<mega::MegaNode> parent(mApi->getParentNode(node.get()));
    if (!parent)
        return Result<bool>::fail("The node has no parent folder", MegaErrorCode::kENoEnt);

    // Both types, unlike findChildrenOfType's single one: MEGA lets a file and a
    // folder share a name, and this check exists to refuse exactly that.
    for (const int nodeType : {mega::MegaNode::TYPE_FILE, mega::MegaNode::TYPE_FOLDER})
    {
        std::unique_ptr<mega::MegaNode> sibling(
            mApi->getChildNodeOfType(parent.get(), name.c_str(), nodeType));
        if (sibling && sibling->getHandle() != node->getHandle())
            return Result<bool>::ok(true);
    }
    return Result<bool>::ok(false);
}

Result<bool> MegaSdkClient::hasSubfolders(std::uint64_t handle, bool isRoot) const
{
    if (mShuttingDown)
        return Result<bool>::fail(kShutDownMessage, kClientShutDownCode);
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, isRoot);
    if (!node)
        return Result<bool>::fail("No node with the given handle", MegaErrorCode::kENoEnt);

    // getNumChildFolders rather than walking getChildren(): it counts against
    // the node tree already in memory since fetchNodes(), so this costs no
    // round-trip and no MegaNodeList allocation.
    return Result<bool>::ok(node->isFolder() && mApi->getNumChildFolders(node.get()) > 0);
}

Result<std::uint64_t> MegaSdkClient::subtreeSize(std::uint64_t handle, bool isRoot) const
{
    if (mShuttingDown)
        return Result<std::uint64_t>::fail(kShutDownMessage, kClientShutDownCode);
    std::unique_ptr<mega::MegaNode> node = resolveNode(handle, isRoot);
    if (!node)
        return Result<std::uint64_t>::fail("No node with the given handle", MegaErrorCode::kENoEnt);

    // Signed in the SDK, and documented to be a sum rather than a status, so a
    // negative can only mean the node went away between resolve and read.
    const long long size = mApi->getSize(node.get());
    return Result<std::uint64_t>::ok(size > 0 ? static_cast<std::uint64_t>(size) : 0);
}

Result<AccountIdentity> MegaSdkClient::currentAccountIdentity() const
{
    if (mShuttingDown)
        return Result<AccountIdentity>::fail(kShutDownMessage, kClientShutDownCode);
    // Three caller-owned char* in one function; all three need releasing.
    const std::unique_ptr<char[]> email(mApi->getMyEmail());
    if (!email)
        return Result<AccountIdentity>::fail("Not logged in", MegaErrorCode::kEInternal);

    // Base64, not the binary handle currentUserHandle() returns: getUserAvatarColor
    // is documented to take that form, and a stringified integer still yields a
    // plausible-looking colour, so the mistake would never show up by eye.
    const std::unique_ptr<char[]> userHandleBase64(mApi->getMyUserHandle());

    AccountIdentity identity;
    identity.email = email.get();
    identity.userHandle = static_cast<std::uint64_t>(mApi->getMyUserHandleBinary());
    if (userHandleBase64)
    {
        const std::unique_ptr<char[]> color(
            mega::MegaApi::getUserAvatarColor(userHandleBase64.get()));
        if (color)
            identity.avatarColor = color.get();
    }
    return Result<AccountIdentity>::ok(std::move(identity));
}

void MegaSdkClient::getMyAvatar(const std::string& destinationPath,
                                std::function<void(Result<std::string>)> onDone)
{
    // No resolveNode pre-flight, unlike getThumbnail: the avatar belongs to
    // the account, not to a node.
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->getUserAvatar(destinationPath.c_str(),
                        new megasdk::AttributeFileListener(std::move(onDone)));
}

void MegaSdkClient::getMyUserAttribute(UserAttribute attribute,
                                       std::function<void(Result<std::string>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<std::string>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->getUserAttribute(toMegaUserAttribute(attribute),
                           new megasdk::TextResultListener(std::move(onDone)));
}

void MegaSdkClient::getFileVersioningEnabled(std::function<void(Result<bool>)> onDone)
{
    if (mShuttingDown)
    {
        onDone(Result<bool>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    // getText() is "1" when versioning is *off* (megaapi.h:21429-21447), so the
    // port's "enabled" is its negation -- don't "fix" this to `== "1"`.
    mApi->getFileVersionsOption(
        new megasdk::TextResultListener([onDone = std::move(onDone)](Result<std::string> result) {
            if (!result.success)
            {
                onDone(Result<bool>::fail(result.errorMessage, result.errorCode));
                return;
            }
            onDone(Result<bool>::ok(result.value() != "1"));
        }));
}

void MegaSdkClient::getAccountInfo(std::function<void(Result<AccountInfo>)> onDone)
{
    // storage + pro only; transfer quota is out of scope and megaapi.h asks
    // callers to request no more than they need.
    if (mShuttingDown)
    {
        onDone(Result<AccountInfo>::fail(kShutDownMessage, kClientShutDownCode));
        return;
    }
    mApi->getSpecificAccountDetails(
        true, false, true, -1, new megasdk::AccountDetailsListener(std::move(onDone)));
}

std::unique_ptr<mega::MegaNode> MegaSdkClient::resolveNode(std::uint64_t handle, bool isRoot) const
{
    // Second entry point that touches mApi, so it carries the same guard as the
    // public methods -- the callers all treat a null node as "gone".
    if (mShuttingDown)
        return nullptr;
    if (isRoot)
        return std::unique_ptr<mega::MegaNode>(mApi->getRootNode());
    return std::unique_ptr<mega::MegaNode>(
        mApi->getNodeByHandle(static_cast<mega::MegaHandle>(handle)));
}

void MegaSdkClient::listChildren(std::unique_ptr<mega::MegaNode> node,
                                 const char* notFoundMessage,
                                 SortOrder order,
                                 std::function<void(Result<std::vector<FileEntry>>)> onDone)
{
    // No mShuttingDown guard: a non-null node means resolveNode ran before the
    // flag was set, and the null case already bails out below.
    if (!node)
    {
        onDone(Result<std::vector<FileEntry>>::fail(notFoundMessage, MegaErrorCode::kENoEnt));
        return;
    }

    std::unique_ptr<mega::MegaNodeList> children(mApi->getChildren(node.get(), toMegaOrder(order)));
    onDone(Result<std::vector<FileEntry>>::ok(nodeListToEntries(children.get())));
}
