#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class MegaSdkClient;
struct PathSegment;

using Json = nlohmann::json;

// Sends an intermediate line (progress) for the request being handled. Returns false
// once the client has gone away, which is how a transfer learns it was cancelled.
using Emit = std::function<bool(const Json&)>;

// One request in, one response out. The protocol is described in docs/HOST.md.
class Service
{
public:
    // buildId: whatever the starting module passed, echoed by `status` so a module can tell
    // whether this host is its own build.
    Service(MegaSdkClient& client, std::string buildId);

    // Returns {"ok":true,"result":...} or {"ok":false,"error":{"code":..,"message":..}};
    // the caller adds the request id.
    Json handle(const Json& request, const Emit& emit);

    bool stopRequested() const { return mStopRequested; }

private:
    Json dispatch(const std::string& op, const Json& args, const Emit& emit);

    Json status();
    Json login(const Json& args, const Emit& emit);
    Json resume(const Json& args, const Emit& emit);
    Json logout();
    Json list(const Json& args);
    Json search(const Json& args);
    Json rubbish();
    Json path(const Json& args);
    Json restoreTarget(const Json& args);
    Json mkdir(const Json& args);
    Json rename(const Json& args);
    Json move(const Json& args);
    Json copy(const Json& args);
    Json trash(const Json& args);
    Json upload(const Json& args, const Emit& emit);
    Json download(const Json& args, const Emit& emit);
    Json link(const Json& args);
    Json links();
    Json exportLink(const Json& args);
    Json unexport(const Json& args);
    Json protectLink(const Json& args);
    Json plan();

    void requireReady() const;
    std::vector<PathSegment> segmentsOf(std::uint64_t handle);
    Json namesOf(std::uint64_t handle);
    void dropSession(); // local logout; caller holds mAuthMutex
    void fetchNodes(const Emit& emit);
    std::string email() const;

    MegaSdkClient& mClient;
    // Serialises everything that changes which session the client holds.
    std::mutex mAuthMutex;
    // True once a login/resume has finished fetchNodes; readable without the mutex.
    std::atomic<bool> mReady{false};
    std::atomic<bool> mStopRequested{false};
    std::atomic<std::uint64_t> mNextTransferId{1};
    const std::string mBuildId;
    // Requests that must not be cut short by `shutdown` (transfers, logging in).
    std::atomic<int> mBusy{0};
};
