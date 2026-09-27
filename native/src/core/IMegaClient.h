#pragma once
#include "AccountInfo.h"
#include "DownloadOutcome.h"
#include "FileEntry.h"
#include "FolderInfo.h"
#include "NodeInfo.h"
#include "PathSegment.h"
#include "RestoreTarget.h"
#include "Result.h"
#include "SearchFilter.h"
#include "SortOrder.h"
#include "UploadOutcome.h"
#include "UserAttribute.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Two conventions run through this whole interface.
//
// 1. An `isRoot`/`parentIsRoot` flag set to true makes the handle beside it
//    meaningless -- the sentinel for "the Cloud Drive root".
// 2. Callback delivery is not uniform, and a caller that assumes "asynchronous"
//    is wrong in two of the three cases:
//      - Methods returning Result<T> with no callback answer in-stack always:
//        local reads of session/account state, or in-memory queries against the
//        node tree fetchNodes built. Their callers (drag-hover feedback,
//        QAbstractItemModel::hasChildren) have nowhere to put a callback.
//      - Of the methods taking onDone, getRootChildren/getChildren/
//        getRubbishChildren/getPath/getNodeInfo run it synchronously on the
//        calling thread, always -- they too are in-memory reads.
//        FolderNavigationService's lock-free design rests on this.
//      - search/listFavourites/listRecent are the in-memory reads that do not:
//        each walks a whole subtree, so the work happens on a worker and
//        onDone arrives back on the thread that constructed the implementation
//        -- the app's main thread -- in a later turn of its event loop. Same
//        thread as every caller, so the lock-free design above still holds; but
//        a caller with no event loop running never sees the result at all, and
//        neither is safe to treat as answered by the time it returns.
//      - The rest run onDone on an SDK-internal thread, *except* that any
//        method resolving a handle fails in-stack when that handle is already
//        gone. That case is easy to miss: the happy path looks purely async.
//
// So onDone may run inside your own call, before it returns: don't hold a lock
// across the call, and guard against re-entering the code that issued it
// (DownloadService/UploadService/ThumbnailService each carry a trampoline).
class IMegaClient
{
public:
    virtual ~IMegaClient() = default;

    virtual void login(const std::string& email,
                       const std::string& password,
                       std::function<void(Result<void>)> onDone) = 0;

    // MegaApi::fastLogin equivalent: re-authenticates from a token previously
    // obtained via currentSessionToken(), skipping password verification.
    virtual void loginWithSession(const std::string& sessionToken,
                                  std::function<void(Result<void>)> onDone) = 0;

    // Called only after login() failed with MegaErrorCode::kEMfaRequired,
    // resubmitting the same email/password alongside the 6-digit pin.
    virtual void multiFactorAuthLogin(const std::string& email,
                                      const std::string& password,
                                      const std::string& pin,
                                      std::function<void(Result<void>)> onDone) = 0;

    virtual void logout(std::function<void(Result<void>)> onDone) = 0;

    // MegaApi::dumpSession equivalent. Fails if not currently logged in.
    virtual Result<std::string> currentSessionToken() const = 0;

    // MegaApi::getMyUserHandleBinary equivalent. Fails if not logged in. Scopes
    // per-account state (quick-access pins) so identity stays out of ISessionStore.
    virtual Result<std::uint64_t> currentUserHandle() const = 0;

    // Must be called after a successful login(), before getRootChildren().
    //
    // onProgress is narrower than it looks: it is HTTP progress of the `f` API
    // response body, not "nodes processed" -- the decrypt/tree-build that follows
    // has no progress signal at all and was 57% of wall time on a 640k-node
    // account. It also may fire zero times (a valid local state-cache DB skips
    // the network entirely), and its last event need not reach 100%.
    // Measurements: docs/investigations/FETCHNODES_PROGRESS_INVESTIGATION.md.
    virtual void fetchNodes(
        std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
        std::function<void(Result<void>)> onDone) = 0;

    // MegaApi::catchup: applies action packets not yet received, so the getters
    // below read a current tree. The only genuine round-trip among the reads --
    // the getters alone re-read whatever the SDK happens to have been told.
    virtual void syncPendingChanges(std::function<void(Result<void>)> onDone) = 0;

    // Must be called after a successful fetchNodes(). order is forwarded to
    // MegaApi::getChildren's own order argument (server-side sort, SortOrder.h).
    virtual void getRootChildren(SortOrder order,
                                 std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // Same contract as getRootChildren(), for a specific folder.
    virtual void getChildren(std::uint64_t handle,
                             SortOrder order,
                             std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // Name search rooted at ancestorHandle, recursive unless filter.thisFolderOnly
    // asks for that node's direct children only. Must be called after a successful
    // fetchNodes(); order is forwarded to MegaApi::search's own order.
    // An empty query means "no name predicate", so filter alone is a valid search.
    // Answers off-thread -- see the delivery note at the top of this file.
    virtual void search(std::uint64_t ancestorHandle,
                        bool isRoot,
                        const std::string& query,
                        const SearchFilter& filter,
                        SortOrder order,
                        std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // Every favourite under the Cloud Drive root, recursively. Answers off-thread,
    // as search() does -- see the delivery note at the top of this file. Rooting it
    // at the Cloud Drive root is what keeps the Rubbish bin, the Vault and
    // incoming shares out. An empty nameFilter means no name filtering at all --
    // MegaSearchFilter treats an unset name as "match everything", so the
    // favourite flag stays the only criterion. filter narrows further, exactly as in
    // search(); its favouritesOnly facet is redundant here but harmless, and its
    // thisFolderOnly facet is ignored -- this listing has no open folder to scope to.
    virtual void listFavourites(SortOrder order,
                                const std::string& nameFilter,
                                const SearchFilter& filter,
                                std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // Everything added under the Cloud Drive root in the last 30 days, recursively;
    // rooting, name filter and contract are listFavourites'.
    // "Added" is the node's creation time, not its modification time: an upload
    // carries the local file's mtime, so a freshly uploaded old file would
    // otherwise never appear here. Files only -- folders are excluded, so filter's
    // node-type facet is ignored here, and its time window can only narrow the 30 days.
    // thisFolderOnly is ignored for the same reason as in listFavourites().
    virtual void listRecent(SortOrder order,
                            const std::string& nameFilter,
                            const SearchFilter& filter,
                            std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // Every node in the Cloud Drive that carries a public link. Rooting, name filter
    // and contract are listFavourites'.
    // Unlike the two above, this one is served by two different SDK calls: with no
    // name filter and a default SearchFilter it is MegaApi::getPublicLinks, and
    // otherwise a rooted search whose results are narrowed to the exported ones.
    // MegaSearchFilter has no "is exported" facet, so a search is the only way to
    // honour the popup's facets server-side; getPublicLinks is the only way to avoid
    // walking the whole drive when nothing is being asked of it.
    // getPublicLinks accepts neither size nor modification-time order, so those two
    // are sorted here instead -- folders first, then the key, matching what the SDK's
    // own comparators do for every other listing.
    virtual void listPublicLinks(SortOrder order,
                                 const std::string& nameFilter,
                                 const SearchFilter& filter,
                                 std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // The Rubbish bin's own top level, same contract as getRootChildren(). Only the
    // top needs its own call: everything below it is an ordinary node, so going
    // deeper is getChildren() with the handle, exactly as in the Cloud Drive.
    virtual void getRubbishChildren(SortOrder order,
                                    std::function<void(Result<std::vector<FileEntry>>)> onDone) = 0;

    // Where restoring a binned node would put it: the folder it was in when it was
    // binned (MegaNode::getRestoreHandle). Synchronous, like the other in-memory
    // queries. isRoot follows the usual convention, and is the answer whenever the
    // original parent is gone or was never recorded -- MEGA's own clients fall back
    // to the Cloud Drive root rather than refusing, and refusing here would strand
    // the node with no other way out of the bin. Fails only when the node itself is
    // gone.
    virtual Result<RestoreTarget> getRestoreTarget(std::uint64_t handle) const = 0;

    // Downloads to the exact local path destinationPath -- the caller resolves it,
    // since IMegaClient has no filesystem access of its own.
    //
    // MegaApi::startDownload is MegaTransferListener-based, hence the second
    // callback: onProgress may fire zero or more times, onDone exactly once.
    // DownloadOutcome::localPath is the path the SDK *actually* wrote
    // (MegaTransfer::getPath()), which differs from destinationPath when a name
    // collision made the SDK suffix the leaf with "(1)". A name already taken never
    // cancels the download: MegaSdkClient asks for COLLISION_CHECK_ASSUMEDIFFERENT
    // so even a byte-identical file is fetched again under a free name.
    //
    // transferId is the caller's own name for this transfer, and the only handle it
    // gets on it: cancelDownload() takes the same value back. Supplied rather than
    // returned because a cancel can arrive before this call returns, and an id the
    // caller already knows is cancellable from the first instant.
    virtual void download(
        std::uint64_t handle,
        const std::string& destinationPath,
        std::uint64_t transferId,
        std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
        std::function<void(Result<DownloadOutcome>)> onDone) = 0;

    // Uploads the local file or directory at localPath (caller-resolved, absolute,
    // native separators) into the folder parentHandle. A directory goes up whole,
    // recursively, as one transfer -- progress and completion are reported for the
    // tree, not per file inside it.
    //
    // Two-callback shape for the same reason as download(), and transferId means the
    // same thing here.
    virtual void
    upload(const std::string& localPath,
           std::uint64_t parentHandle,
           bool parentIsRoot,
           std::uint64_t transferId,
           std::function<void(std::uint64_t transferredBytes, std::uint64_t totalBytes)> onProgress,
           std::function<void(Result<UploadOutcome>)> onDone) = 0;

    // Abort the transfer started under transferId. Callable from any thread and a
    // no-op when that id names nothing in flight, so a caller never has to check
    // first -- including for an id whose transfer has not been started yet, which is
    // why the caller must re-assert the cancel once download()/upload() returns.
    //
    // Asynchronous like everything else here: the abort is only requested, and it is
    // the transfer's own onDone that reports it -- failing with
    // MegaErrorCode::kEIncomplete, the code the SDK reserves for a cancelled
    // transfer. Callers still get exactly one completion per transfer.
    virtual void cancelDownload(std::uint64_t transferId) = 0;
    virtual void cancelUpload(std::uint64_t transferId) = 0;

    // Fetches the server-side thumbnail into the exact local path destinationPath,
    // which must not end with a path separator: the SDK would then treat it as a
    // directory and derive the leaf name itself (megaapi_impl.cpp's
    // getNodeAttribute). Result value is the path actually written.
    //
    // Fails with API_ENOENT when the node has no server-side thumbnail; callers
    // gate on FileEntry::hasThumbnail rather than branching on that.
    virtual void getThumbnail(std::uint64_t handle,
                              const std::string& destinationPath,
                              std::function<void(Result<std::string>)> onDone) = 0;

    // Same call shape and same separator rule as getThumbnail, but the attribute
    // fetched is the preview: a JPEG scaled to fit inside 1000x1000, not the 200px
    // square crop.
    //
    // Unlike getThumbnail there is no FileEntry flag to gate on, and a preview only
    // exists when the uploading client generated one. Callers must therefore read
    // *any* failure as "no preview" and must not surface it as an error -- the same
    // rule getMyAvatar states.
    virtual void getPreview(std::uint64_t handle,
                            const std::string& destinationPath,
                            std::function<void(Result<std::string>)> onDone) = 0;

    // Reads a whole file into memory instead of writing it anywhere
    // (MegaApi::startStreaming). The only caller is the preview pane's text view,
    // which must leave nothing behind on disk.
    //
    // maxBytes is a hard stop, not a range request: exceeding it aborts the transfer
    // and fails. Callers gate on FileEntry::sizeBytes first, so this is the check
    // against a node bigger than the listing claimed.
    //
    // There is deliberately no way to cancel. The payload is capped in the tens of
    // kilobytes, so there is no bandwidth worth reclaiming, and a cancel channel
    // would need shared state the SDK thread polls -- the cross-thread lifetime
    // problem this design exists to avoid.
    virtual void readFileContent(std::uint64_t handle,
                                 std::uint64_t maxBytes,
                                 std::function<void(Result<std::vector<char>>)> onDone) = 0;

    // A byte range of a file, in memory, under the same no-cancel rule as above.
    // Unlike maxBytes, length is a real range request, and doubles as the buffer
    // cap; a range overrunning the end is clamped, an offset at or past it fails.
    virtual void readFileRange(std::uint64_t handle,
                               std::uint64_t offset,
                               std::uint64_t length,
                               std::function<void(Result<std::vector<char>>)> onDone) = 0;

    // readFileRange without the buffer: onChunk gets the bytes as they arrive, in
    // file order with no gaps, one call at a time on the SDK's thread
    // (STUDY_ARCHIVE_EXTRACTION.md section 2). Returning false aborts the transfer,
    // which is how a caller cancels; onDone then fails, whichever chunk it was.
    virtual void
    readFileRangeStreamed(std::uint64_t handle,
                          std::uint64_t offset,
                          std::uint64_t length,
                          std::function<bool(const char* data, std::size_t size)> onChunk,
                          std::function<void(Result<void>)> onDone) = 0;

    // http://127.0.0.1:<port>/<handle>/<name>, served by the SDK's own local HTTP
    // server, for the case readFileRange cannot cover: handing a whole node to
    // something that only takes a URL. Nothing reaches the disk -- the SDK decrypts
    // in memory -- and the server answers Range requests, so seeking works.
    //
    // Synchronous because the SDK's server start and link building both are. The
    // server is started on the first call and lives as long as the client; only
    // nodes handed out this way are served, which is the SDK's default mode.
    //
    // The URL is a capability: any process on this machine that learns it can read
    // the file. Never log it.
    virtual Result<std::string> streamingUrl(std::uint64_t handle) = 0;

    // Ancestor chain root-first, always including the root as the first element
    // and the node itself as the last. Must follow a successful fetchNodes().
    virtual void getPath(std::uint64_t handle,
                         bool isRoot,
                         std::function<void(Result<std::vector<PathSegment>>)> onDone) = 0;

    // Current identity of the node, without walking its ancestors. Succeeding does
    // *not* mean the node is still in the Cloud Drive -- a deleted node lives on
    // in the Rubbish bin, so check NodeInfo::inCloud for that.
    virtual void getNodeInfo(std::uint64_t handle,
                             std::function<void(Result<NodeInfo>)> onDone) = 0;

    // Recursive file/folder counts and byte total of a folder. The one read here
    // that is a request rather than an in-memory query, so onDone arrives on an
    // SDK thread and it does not belong in the synchronous list at the top of this
    // file -- subtreeSize() stays the cheap answer when only the bytes are wanted.
    // Fails on a file: MegaApi::getFolderInfo has nothing to report for one.
    virtual void getFolderInfo(std::uint64_t handle,
                               bool isRoot,
                               std::function<void(Result<FolderInfo>)> onDone) = 0;

    // First of the mutating calls -- everything above only reads. All are real API
    // round-trips and must follow a successful fetchNodes().
    virtual void renameNode(std::uint64_t handle,
                            const std::string& newName,
                            std::function<void(Result<void>)> onDone) = 0;

    // "Delete" in MEGA terms: moves the node to the Rubbish bin rather than
    // destroying it. The permanent form is removeNode() below.
    virtual void moveToRubbish(std::uint64_t handle, std::function<void(Result<void>)> onDone) = 0;

    // Destroys the node outright (MegaApi::remove). Irreversible -- MEGA keeps no
    // second bin behind the bin -- so every caller must confirm first. The SDK will
    // happily remove a node that is not in the bin, so the restriction to the bin
    // lives in MenuActionResolver rather than here.
    virtual void removeNode(std::uint64_t handle, std::function<void(Result<void>)> onDone) = 0;

    // Empties the Rubbish bin in one request (MegaApi::cleanRubbishBin) rather than
    // a removeNode() per child: it is server-side, so it neither enumerates the bin
    // nor leaves it half-emptied if the app quits mid-way. Irreversible.
    virtual void cleanRubbishBin(std::function<void(Result<void>)> onDone) = 0;

    // newName empty keeps the node's name. Renaming here rather than move-then-rename
    // is one request, not two: the SDK folds the new name into the same command
    // (SPEC_NAME_CONFLICT_COPY_MOVE 1-4). Unlike copyNode below, the name changes
    // nothing about the outcome -- a move never merges or versions, so a colliding
    // name simply leaves two siblings sharing it.
    virtual void moveNode(std::uint64_t handle,
                          std::uint64_t newParentHandle,
                          bool newParentIsRoot,
                          const std::string& newName,
                          std::function<void(Result<void>)> onDone) = 0;

    // Duplicates the node under newParentHandle. A folder is copied with its whole
    // subtree in this one request -- the SDK walks it server-side.
    //
    // newName empty keeps the source's name, and passing one is not cosmetic: if
    // the destination already holds a *file* of that name, the SDK attaches the
    // copy as a new version *over* it instead of creating a sibling, and drops a
    // byte-identical file outright while still reporting success. "Add a second
    // item" therefore requires an unused name (FileOperationService::uniqueCopyName).
    // Folders are unaffected -- two same-named folders coexist, with no kEExist.
    virtual void copyNode(std::uint64_t handle,
                          std::uint64_t newParentHandle,
                          bool newParentIsRoot,
                          const std::string& newName,
                          std::function<void(Result<void>)> onDone) = 0;

    // Creates an empty folder under parentHandle. The duplicate-name check is the
    // *server's* -- onDone reports MegaErrorCode::kEExist -- and deliberately the
    // only one: an in-memory pre-check could go stale between check and call. Note
    // MEGA lets a file and a folder share a name, so an existing *file* of that
    // name is not a conflict.
    virtual void createFolder(std::uint64_t parentHandle,
                              bool parentIsRoot,
                              const std::string& name,
                              std::function<void(Result<void>)> onDone) = 0;

    // Sets or clears the node's "favourite" attribute. Idempotent -- the SDK reports
    // success for a value the node already has, so a caller whose cached flag has
    // drifted still ends up with the state it asked for.
    virtual void setNodeFavourite(std::uint64_t handle,
                                  bool favourite,
                                  std::function<void(Result<void>)> onDone) = 0;

    // Issues -- or re-reads -- the node's public link, handing back the URL. Safe to
    // call on a node that already has one: MEGA returns the existing link rather than
    // minting a second, so this doubles as "get the link" and no caller has to know
    // which of the two it is doing.
    //
    // An expiry already on the link survives: MEGA re-issues the link whenever the
    // requested expiry differs from the stored one, so asking for "never" here would
    // silently strip a date set from another client. setLinkExpiry is the only door
    // that changes it.
    virtual void exportNode(std::uint64_t handle,
                            std::function<void(Result<std::string>)> onDone) = 0;

    // Re-issues the link with a new expiry, handing back the URL like exportNode --
    // the SDK has no separate call for it, and a node with no link yet gets one. Unix
    // seconds; 0 clears the expiry. Rejected with kEAccess on a free account, which
    // is a MEGA plan restriction and not something the caller can retry.
    virtual void setLinkExpiry(std::uint64_t handle,
                               std::int64_t expireTime,
                               std::function<void(Result<std::string>)> onDone) = 0;

    // The expiry currently stored on the node's link, as a local read with no round
    // trip: -1 when the node has no link at all, 0 when the link never expires,
    // otherwise Unix seconds.
    virtual Result<std::int64_t> getLinkExpiry(std::uint64_t handle) const = 0;

    // Wraps an existing public link (key included) into MEGA's password-protected
    // `#P!` form. Computed locally with nothing stored on MEGA, so the original link
    // stays valid and nothing can later report that a password was ever set.
    virtual void encryptLinkWithPassword(const std::string& link,
                                         const std::string& password,
                                         std::function<void(Result<std::string>)> onDone) = 0;

    // exportNode's inverse. Reports success for a node that has no link, so a caller
    // whose view of the export state is stale still ends up where it asked to be.
    virtual void disableExport(std::uint64_t handle, std::function<void(Result<void>)> onDone) = 0;

    // Whether moveNode() with the same arguments would be accepted, as a pure
    // in-memory check -- a drag hovering over a drop target queries it per mouse
    // move. Callers branch on errorCode, never errorMessage (MegaErrorCodes.h):
    // kENoEnt either end is gone, kECircular a folder would become its own
    // descendant, kEAccess insufficient permission, kEArgs the node already sits
    // in that folder. That last case is stricter than the SDK, which treats it as
    // a no-op; it lives here because only an implementation can see a node's
    // actual parent handle.
    virtual Result<void>
    checkMove(std::uint64_t handle, std::uint64_t newParentHandle, bool newParentIsRoot) const = 0;

    // Whether upload() into this folder would be accepted. The SDK has no
    // checkUploadErrorExtended equivalent, so the implementation spells the
    // conditions out itself; same error-code discipline as checkMove:
    //
    //   kENoEnt  the handle no longer resolves, or resolves to a node that
    //            is no longer in the Cloud Drive (Rubbish bin / Vault)
    //   kEArgs   it resolves to a file, not a folder
    //   kEAccess insufficient permission to add children (read-only share)
    virtual Result<void> checkUpload(std::uint64_t parentHandle, bool parentIsRoot) const = 0;

    // Of names, returns those that already name an existing *file* directly under
    // the given parent -- only the hits come back, in no particular order.
    //
    // Same-named *folders* are ignored: uploads are files-only and MEGA lets a
    // file and a folder share a name, so replacing a folder would be destructive
    // and unasked-for.
    virtual Result<std::vector<FileEntry>>
    findChildFiles(std::uint64_t parentHandle,
                   bool parentIsRoot,
                   const std::vector<std::string>& names) const = 0;

    // The folder-typed twin of findChildFiles, used to walk an upload's nested
    // name collisions: a local subfolder is only worth descending into when a
    // folder of that name already exists, and the hit carries the handle the
    // level below has to be checked against.
    virtual Result<std::vector<FileEntry>>
    findChildFolders(std::uint64_t parentHandle,
                     bool parentIsRoot,
                     const std::vector<std::string>& names) const = 0;

    // Whether a sibling of the node -- another child of its own parent -- already
    // carries name, as either a file or a folder. Same in-memory lookup as
    // findChildFiles, so it costs no round-trip (SPEC_NAME_CONFLICT_COPY_MOVE 1-5);
    // the parent is read here because only an implementation can see it, as with
    // checkMove. The node itself never counts as its own collision, so a rename
    // that only changes a name's case still goes through.
    virtual Result<bool> siblingNameTaken(std::uint64_t handle, const std::string& name) const = 0;

    // Whether the node has at least one *folder* child -- the folder tree's own
    // question, since files never appear in the side panel. False for a file.
    virtual Result<bool> hasSubfolders(std::uint64_t handle, bool isRoot) const = 0;

    // Bytes the node is worth to an operation that duplicates it: for a folder the
    // whole sub-tree, for a file its own size. Synchronous and local -- it sums the
    // tree already in memory since fetchNodes(), so it costs no round-trip
    // (SPEC_NAME_CONFLICT_COPY_MOVE 1-6).
    //
    // There is deliberately no recursive file count beside it: the SDK's
    // getNumChildFiles() counts direct children only, and the recursive one
    // (getFolderInfo) is a request, not a read.
    virtual Result<std::uint64_t> subtreeSize(std::uint64_t handle, bool isRoot) const = 0;

    // --- Account-level reads -------------------------------------------------

    // MegaApi::getMyEmail + getMyUserHandle + getUserAvatarColor in one read.
    // Fails if not currently logged in.
    //
    // Not an atomic snapshot: each field is a separate SDK read under its own
    // lock, so a logout landing mid-call can pair one account's email with an
    // empty handle. Acceptable only because the sole consumer paints the account
    // panel that the same logout tears down; anything that *decides* on the pair
    // needs a snapshotting variant.
    virtual Result<AccountIdentity> currentAccountIdentity() const = 0;

    // Fetches the signed-in account's avatar into the exact local path
    // destinationPath, which must not end with a path separator (the SDK would
    // treat it as a directory and synthesize email + "0.jpg" inside it). Result
    // value is the path actually written.
    //
    // Unlike getThumbnail, failure is *not* exceptional: most accounts have no
    // avatar and there is no flag to gate on beforehand. The observed code is
    // kENoEnt, but megaapi.h does not document that, so callers must read *any*
    // failure as "no avatar" -- AccountService converts it to
    // AvatarOutcome::hasAvatar rather than leaving that to each caller.
    virtual void getMyAvatar(const std::string& destinationPath,
                             std::function<void(Result<std::string>)> onDone) = 0;

    // Reads one public attribute of the signed-in account; the value arrives in
    // MegaRequest::getText(). Parameterised by attribute rather than exposing
    // getMyDisplayName, so the first-name/last-name join policy stays in
    // AccountService where a mock can test it. Fails when the attribute has never
    // been set, which is ordinary for names.
    virtual void getMyUserAttribute(UserAttribute attribute,
                                    std::function<void(Result<std::string>)> onDone) = 0;

    // Whether the account keeps an overwritten file's previous content as a version.
    // The port's sense is "enabled"; the SDK's getFileVersionsOption reports the
    // *disabled* flag, and the adapter negates it. Fails with kENoEnt on an account
    // that never touched the setting, which means enabled -- callers apply that
    // default themselves (docs/investigations/SPEC_NAME_CONFLICT_COPY_MOVE.md 1-3).
    virtual void getFileVersioningEnabled(std::function<void(Result<bool>)> onDone) = 0;

    // Storage quota and plan level. MegaApi::getSpecificAccountDetails with only
    // storage+pro: megaapi.h asks callers to request just what they need, and
    // transfer quota is out of scope. A real server round-trip -- never per-frame
    // or from a property getter.
    virtual void getAccountInfo(std::function<void(Result<AccountInfo>)> onDone) = 0;
};
