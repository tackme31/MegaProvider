#pragma once
#include "core/IMegaClient.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

// forward declarations; only files under src/mega need <megaapi.h>
namespace mega
{
class MegaApi;
class MegaCancelToken;
class MegaNode;
} // namespace mega

class MegaSdkLogger;
class SerialExecutor;

// Only files under src/mega (this class and MegaSdkLogger) may include
// <megaapi.h> or touch mega::* types.
class MegaSdkClient : public IMegaClient
{
public:
    // basePath is where the SDK unconditionally creates its state-cache DB.
    // Deliberately has no default: a relative one moves the DB whenever the app
    // starts from a different directory, and a missed DB means re-downloading the
    // whole node tree (385s vs 0.6s on a 640k-node account).
    explicit MegaSdkClient(std::string basePath, std::string userAgent = "MegaExplorer");
    ~MegaSdkClient() override;

    // Explicit stop point for the SDK thread. Destroying MegaApi fires every pending
    // request as a failure before joining, and this object outlives every service
    // and controller those callbacks touch, so leaving it to the destructor delivers
    // them to freed memory. Call it while everything is still alive.
    //
    // Afterwards this object is inert: every method fails immediately. Idempotent,
    // and the destructor calls it too, so forgetting only loses the ordering.
    void shutdown();

    void login(const std::string& email,
               const std::string& password,
               std::function<void(Result<void>)> onDone) override;

    void loginWithSession(const std::string& sessionToken,
                          std::function<void(Result<void>)> onDone) override;

    void multiFactorAuthLogin(const std::string& email,
                              const std::string& password,
                              const std::string& pin,
                              std::function<void(Result<void>)> onDone) override;

    void logout(std::function<void(Result<void>)> onDone) override;

    // MegaProvider addition: drops the session from this process only, leaving it valid
    // on the server (MegaApi::localLogout), so it can be resumed later or elsewhere.
    void localLogout(std::function<void(Result<void>)> onDone);

    Result<std::string> currentSessionToken() const override;

    Result<std::uint64_t> currentUserHandle() const override;

    void fetchNodes(
        std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
        std::function<void(Result<void>)> onDone) override;

    void syncPendingChanges(std::function<void(Result<void>)> onDone) override;

    void getRootChildren(SortOrder order,
                         std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    void getChildren(std::uint64_t handle,
                     SortOrder order,
                     std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    void search(std::uint64_t ancestorHandle,
                bool isRoot,
                const std::string& query,
                const SearchFilter& filter,
                SortOrder order,
                std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    void listFavourites(SortOrder order,
                        const std::string& nameFilter,
                        const SearchFilter& filter,
                        std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    void listRecent(SortOrder order,
                    const std::string& nameFilter,
                    const SearchFilter& filter,
                    std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    void listPublicLinks(SortOrder order,
                         const std::string& nameFilter,
                         const SearchFilter& filter,
                         std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    void getRubbishChildren(SortOrder order,
                            std::function<void(Result<std::vector<FileEntry>>)> onDone) override;

    Result<RestoreTarget> getRestoreTarget(std::uint64_t handle) const override;

    void download(
        std::uint64_t handle,
        const std::string& destinationPath,
        std::uint64_t transferId,
        std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
        std::function<void(Result<DownloadOutcome>)> onDone) override;

    void
    upload(const std::string& localPath,
           std::uint64_t parentHandle,
           bool parentIsRoot,
           std::uint64_t transferId,
           std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
           std::function<void(Result<UploadOutcome>)> onDone) override;

    void cancelDownload(std::uint64_t transferId) override;
    void cancelUpload(std::uint64_t transferId) override;

    void getThumbnail(std::uint64_t handle,
                      const std::string& destinationPath,
                      std::function<void(Result<std::string>)> onDone) override;

    void getPreview(std::uint64_t handle,
                    const std::string& destinationPath,
                    std::function<void(Result<std::string>)> onDone) override;

    void readFileContent(std::uint64_t handle,
                         std::uint64_t maxBytes,
                         std::function<void(Result<std::vector<char>>)> onDone) override;

    void readFileRange(std::uint64_t handle,
                       std::uint64_t offset,
                       std::uint64_t length,
                       std::function<void(Result<std::vector<char>>)> onDone) override;

    void readFileRangeStreamed(std::uint64_t handle,
                               std::uint64_t offset,
                               std::uint64_t length,
                               std::function<bool(const char* data, std::size_t size)> onChunk,
                               std::function<void(Result<void>)> onDone) override;

    Result<std::string> streamingUrl(std::uint64_t handle) override;

    void getPath(std::uint64_t handle,
                 bool isRoot,
                 std::function<void(Result<std::vector<PathSegment>>)> onDone) override;

    void getNodeInfo(std::uint64_t handle, std::function<void(Result<NodeInfo>)> onDone) override;

    void getFolderInfo(std::uint64_t handle,
                       bool isRoot,
                       std::function<void(Result<FolderInfo>)> onDone) override;

    void renameNode(std::uint64_t handle,
                    const std::string& newName,
                    std::function<void(Result<void>)> onDone) override;

    void moveToRubbish(std::uint64_t handle, std::function<void(Result<void>)> onDone) override;

    void removeNode(std::uint64_t handle, std::function<void(Result<void>)> onDone) override;

    void cleanRubbishBin(std::function<void(Result<void>)> onDone) override;

    void moveNode(std::uint64_t handle,
                  std::uint64_t newParentHandle,
                  bool newParentIsRoot,
                  const std::string& newName,
                  std::function<void(Result<void>)> onDone) override;

    void copyNode(std::uint64_t handle,
                  std::uint64_t newParentHandle,
                  bool newParentIsRoot,
                  const std::string& newName,
                  std::function<void(Result<void>)> onDone) override;

    void createFolder(std::uint64_t parentHandle,
                      bool parentIsRoot,
                      const std::string& name,
                      std::function<void(Result<void>)> onDone) override;

    void setNodeFavourite(std::uint64_t handle,
                          bool favourite,
                          std::function<void(Result<void>)> onDone) override;

    void exportNode(std::uint64_t handle, std::function<void(Result<std::string>)> onDone) override;

    void setLinkExpiry(std::uint64_t handle,
                       std::int64_t expireTime,
                       std::function<void(Result<std::string>)> onDone) override;

    Result<std::int64_t> getLinkExpiry(std::uint64_t handle) const override;

    Result<LinkDetails> getLinkDetails(std::uint64_t handle) const override;

    void encryptLinkWithPassword(const std::string& link,
                                 const std::string& password,
                                 std::function<void(Result<std::string>)> onDone) override;

    void disableExport(std::uint64_t handle, std::function<void(Result<void>)> onDone) override;

    Result<void> checkMove(std::uint64_t handle,
                           std::uint64_t newParentHandle,
                           bool newParentIsRoot) const override;

    Result<void> checkUpload(std::uint64_t parentHandle, bool parentIsRoot) const override;

    Result<std::vector<FileEntry>>
    findChildFiles(std::uint64_t parentHandle,
                   bool parentIsRoot,
                   const std::vector<std::string>& names) const override;

    Result<std::vector<FileEntry>>
    findChildFolders(std::uint64_t parentHandle,
                     bool parentIsRoot,
                     const std::vector<std::string>& names) const override;

    Result<bool> siblingNameTaken(std::uint64_t handle, const std::string& name) const override;

    Result<bool> hasSubfolders(std::uint64_t handle, bool isRoot) const override;
    Result<std::uint64_t> subtreeSize(std::uint64_t handle, bool isRoot) const override;

    Result<AccountIdentity> currentAccountIdentity() const override;

    void getMyAvatar(const std::string& destinationPath,
                     std::function<void(Result<std::string>)> onDone) override;

    void getMyUserAttribute(UserAttribute attribute,
                            std::function<void(Result<std::string>)> onDone) override;

    void getFileVersioningEnabled(std::function<void(Result<bool>)> onDone) override;

    void getAccountInfo(std::function<void(Result<AccountInfo>)> onDone) override;

private:
    // const so the const checkMove() can use it: unique_ptr::operator->() is
    // const-qualified but hands back a non-const MegaApi*.
    std::unique_ptr<mega::MegaNode> resolveNode(std::uint64_t handle, bool isRoot) const;

    // nodeType is a mega::MegaNode::TYPE_* value; the header the interface lives
    // in must not include the SDK's, so the public pair spells it out instead.
    Result<std::vector<FileEntry>> findChildrenOfType(std::uint64_t parentHandle,
                                                      bool parentIsRoot,
                                                      const std::vector<std::string>& names,
                                                      int nodeType) const;

    void listChildren(std::unique_ptr<mega::MegaNode> node,
                      const char* notFoundMessage,
                      SortOrder order,
                      std::function<void(Result<std::vector<FileEntry>>)> onDone);

    // Runs work on mListingPool and hands its result to onDone back on the thread
    // that *constructed* this client -- not the one that called, which is the same
    // thread for every caller today but is not what is implemented. Only the tree
    // walks take this route -- the two cross-drive listings and search: each walks a
    // whole subtree, so on a large account the synchronous form blocked the GUI for
    // seconds.
    //
    // It buys less than it looks: MegaApiImpl::search holds the SDK's sdkMutex for
    // the whole walk, so a GUI-thread call that also needs the SDK still waits it
    // out. Idle painting and input do not, which is the case this is for.
    void runOffThread(std::function<Result<std::vector<FileEntry>>()> work,
                      std::function<void(Result<std::vector<FileEntry>>)> onDone);

    // Declared before mApi so it is destroyed last, which is what lets the SDK's own
    // teardown lines reach the log. It does not help at startup: registration happens
    // in the constructor body, after MegaApiImpl::init has already logged.
    std::unique_ptr<MegaSdkLogger> mLogger;
    std::unique_ptr<mega::MegaApi> mApi;

    // Set before mApi is destroyed, so teardown callbacks on the SDK thread bail out
    // instead of dereferencing null. A mutex is *not usable* here: the SDK delivers
    // callbacks holding its own sdkMutex, which our synchronous methods take from the
    // GUI thread, so any lock wrapping both sides inverts the order and deadlocks.
    std::atomic<bool> mShuttingDown{false};

    // The SDK's local HTTP server is started lazily by streamingUrl() and stopped in
    // shutdown(). Plain bool rather than atomic: both are called from the thread that
    // constructed this client, never from an SDK callback. httpServerIsRunning() is
    // not usable in its place -- it answers with the port, which is a valid 0 too.
    bool mHttpServerStarted = false;

    // One token per in-flight transfer, keyed by the caller's transferId, so
    // cancelDownload()/cancelUpload() can name one of several running transfers.
    // Owned here rather than by the listener because the listener deletes itself on
    // the SDK thread the moment its transfer ends -- a pointer to it could not be
    // held safely. The entry is erased by the onDone wrapper instead, before the
    // caller's own onDone runs, so the map tracks what is actually in flight.
    //
    // A mutex is usable here, unlike around mShuttingDown, only because nothing
    // under it re-enters the SDK: MegaCancelToken::cancel() just writes a shared
    // flag (mega/types.h's CancelToken) and takes no lock, so this one is a leaf and
    // cannot invert against sdkMutex. Keep it that way -- download() deliberately
    // calls startDownload *outside* the guard, and it can arrive on the SDK's own
    // callback thread with sdkMutex already held.
    std::mutex mCancelTokenMutex;
    std::map<std::uint64_t, std::unique_ptr<mega::MegaCancelToken>> mDownloadCancelTokens;
    std::map<std::uint64_t, std::unique_ptr<mega::MegaCancelToken>> mUploadCancelTokens;

    // Handed to every tree walk runOffThread() runs, and cancelled once in shutdown(),
    // so closing the window does not wait out a walk nobody will see. One long-lived
    // token rather than one per call because it is never re-armed: past shutdown()
    // every queued job already bails on mShuttingDown. Declared before the pool so it
    // outlives the worker reading it, and cancel() takes no lock (see the
    // mCancelTokenMutex note), so the GUI thread cannot invert against sdkMutex here.
    std::unique_ptr<mega::MegaCancelToken> mListingCancelToken;

    // Capped at one thread, and that cap is load-bearing rather than frugal: it makes
    // the pool answer in issue order, which is what lets a caller treat the last
    // listing it asked for as the last one it will be handed. It is also what keeps
    // the single mListingCancelToken inside the SDK's "one operation at a time" rule.
    std::unique_ptr<SerialExecutor> mListingPool;
};
