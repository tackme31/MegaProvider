#pragma once
#include "core/AccountInfo.h"
#include "core/DownloadOutcome.h"
#include "core/FolderInfo.h"
#include "core/MegaErrorCodes.h"
#include "core/Result.h"
#include "core/UploadOutcome.h"

#include <cstdint>
#include <functional>
#include <megaapi.h>
#include <memory>
#include <string>
#include <utility>

// The callback adapters MegaSdkClient hands to MegaApi: each turns one shape of SDK
// completion into the std::function the port declares. Only MegaSdkClient.cpp
// includes this -- a header rather than a .cpp of its own so <megaapi.h> still gets
// compiled exactly once.
namespace megasdk
{

// Listener lifetime, once for every class below: each one is `new`ed at
// the call site, handed to MegaApi, and deletes itself from its finish
// callback. That is the only correct arrangement here -- the SDK never deletes
// listeners (megaapi_impl.cpp:18032,18192 delete the request/transfer, never
// the listener), and it always fires the finish callback, including on abort
// (:20830, :9692), so nothing is ever stranded.
//
// Do NOT "clean up" by calling removeRequestListener/removeTransferListener.
// They do not unsubscribe, they setListener(NULL) (:17895-17903), so the finish
// callback never arrives, `delete this` is never reached, and the listener
// leaks -- the exact opposite of what the call looks like it does.

// Shared by every SDK call whose completion is a bare success/failure with no extra
// payload (login, loginWithSession, multiFactorAuthLogin, logout). fetchNodes has
// its own listener below because it is the one request type that also reports
// progress, which these four must not pay for.
class SimpleResultListener : public mega::MegaRequestListener
{
public:
    explicit SimpleResultListener(std::function<void(Result<void>)> onDone)
        : mOnDone(std::move(onDone))
    {}

    void onRequestFinish(mega::MegaApi* /*api*/,
                         mega::MegaRequest* /*request*/,
                         mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            mOnDone(Result<void>::ok());
        }
        else
        {
            mOnDone(Result<void>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(Result<void>)> mOnDone;
};

// SimpleResultListener plus onRequestUpdate, which the SDK documents as firing for
// TYPE_FETCH_NODES only (megaapi.h:9261) and which reports the `f` response's HTTP
// download progress. Nothing here smooths over the caveats in
// IMegaClient::fetchNodes -- they are the caller's to handle.
class FetchNodesListener : public mega::MegaRequestListener
{
public:
    FetchNodesListener(std::function<void(std::uint64_t, std::uint64_t)> onProgress,
                       std::function<void(Result<void>)> onDone)
        : mOnProgress(std::move(onProgress)), mOnDone(std::move(onDone))
    {}

    void onRequestUpdate(mega::MegaApi* /*api*/, mega::MegaRequest* request) override
    {
        // Both getters return long long and can still be at their -1 default: the SDK
        // only calls setTotalBytes once the response length is known, so early
        // updates carry an unknown total. Casting -1 to uint64_t would hand the UI
        // 1.8e19 and pin its bar at zero forever.
        const long long transferred = request->getTransferredBytes();
        const long long total = request->getTotalBytes();
        mOnProgress(transferred > 0 ? static_cast<std::uint64_t>(transferred) : 0,
                    total > 0 ? static_cast<std::uint64_t>(total) : 0);
    }

    void onRequestFinish(mega::MegaApi* /*api*/,
                         mega::MegaRequest* /*request*/,
                         mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            mOnDone(Result<void>::ok());
        }
        else
        {
            mOnDone(Result<void>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(std::uint64_t, std::uint64_t)> mOnProgress;
    std::function<void(Result<void>)> mOnDone;
};

// Shared by getThumbnail and getMyAvatar: both are TYPE_GET_ATTR_* requests
// whose success payload is the local file the SDK wrote to.
class AttributeFileListener : public mega::MegaRequestListener
{
public:
    explicit AttributeFileListener(std::function<void(Result<std::string>)> onDone)
        : mOnDone(std::move(onDone))
    {}

    void
    onRequestFinish(mega::MegaApi* /*api*/, mega::MegaRequest* request, mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            const char* path = request->getFile(); // destination path the SDK wrote to
            mOnDone(Result<std::string>::ok(path ? path : std::string()));
        }
        else
        {
            mOnDone(Result<std::string>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(Result<std::string>)> mOnDone;
};

// getMyUserAttribute and encryptLinkWithPassword: both answers arrive as text.
class TextResultListener : public mega::MegaRequestListener
{
public:
    explicit TextResultListener(std::function<void(Result<std::string>)> onDone)
        : mOnDone(std::move(onDone))
    {}

    void
    onRequestFinish(mega::MegaApi* /*api*/, mega::MegaRequest* request, mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            const char* text = request->getText();
            mOnDone(Result<std::string>::ok(text ? text : std::string()));
        }
        else
        {
            mOnDone(Result<std::string>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(Result<std::string>)> mOnDone;
};

// TextResultListener's twin for MegaRequest::TYPE_EXPORT, which parks its answer in
// getLink() rather than getText().
class LinkResultListener : public mega::MegaRequestListener
{
public:
    explicit LinkResultListener(std::function<void(Result<std::string>)> onDone)
        : mOnDone(std::move(onDone))
    {}

    void
    onRequestFinish(mega::MegaApi* /*api*/, mega::MegaRequest* request, mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            const char* link = request->getLink();
            mOnDone(Result<std::string>::ok(link ? link : std::string()));
        }
        else
        {
            mOnDone(Result<std::string>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(Result<std::string>)> mOnDone;
};

class AccountDetailsListener : public mega::MegaRequestListener
{
public:
    explicit AccountDetailsListener(std::function<void(Result<AccountInfo>)> onDone)
        : mOnDone(std::move(onDone))
    {}

    void
    onRequestFinish(mega::MegaApi* /*api*/, mega::MegaRequest* request, mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            // getMegaAccountDetails() transfers ownership (megaapi.h).
            const std::unique_ptr<mega::MegaAccountDetails> details(
                request->getMegaAccountDetails());
            if (details)
            {
                AccountInfo info;
                info.storageUsedBytes = static_cast<std::uint64_t>(details->getStorageUsed());
                info.storageMaxBytes = static_cast<std::uint64_t>(details->getStorageMax());
                info.proLevel = details->getProLevel();
                mOnDone(Result<AccountInfo>::ok(info));
            }
            else
            {
                mOnDone(Result<AccountInfo>::fail("Account details missing from response",
                                                  MegaErrorCode::kEInternal));
            }
        }
        else
        {
            mOnDone(Result<AccountInfo>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(Result<AccountInfo>)> mOnDone;
};

class FolderInfoListener : public mega::MegaRequestListener
{
public:
    explicit FolderInfoListener(std::function<void(Result<FolderInfo>)> onDone)
        : mOnDone(std::move(onDone))
    {}

    void
    onRequestFinish(mega::MegaApi* /*api*/, mega::MegaRequest* request, mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            // getMegaFolderInfo() does NOT transfer ownership, unlike
            // getMegaAccountDetails() right above (megaapi.h) -- the SDK frees it with
            // the request, so wrapping it in a unique_ptr here is a double free.
            const mega::MegaFolderInfo* info = request->getMegaFolderInfo();
            if (info)
            {
                FolderInfo folder;
                // All three are signed in the SDK and documented as counts/sums, so a
                // negative can only mean the folder went away mid-walk.
                folder.fileCount =
                    info->getNumFiles() > 0 ? static_cast<std::uint64_t>(info->getNumFiles()) : 0;
                folder.folderCount = info->getNumFolders() > 0
                                         ? static_cast<std::uint64_t>(info->getNumFolders())
                                         : 0;
                folder.sizeBytes = info->getCurrentSize() > 0
                                       ? static_cast<std::uint64_t>(info->getCurrentSize())
                                       : 0;
                mOnDone(Result<FolderInfo>::ok(folder));
            }
            else
            {
                mOnDone(Result<FolderInfo>::fail("Folder info missing from response",
                                                 MegaErrorCode::kEInternal));
            }
        }
        else
        {
            mOnDone(Result<FolderInfo>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(Result<FolderInfo>)> mOnDone;
};

class DownloadListener : public mega::MegaTransferListener
{
public:
    DownloadListener(std::function<void(std::uint64_t, std::uint64_t)> onProgress,
                     std::function<void(Result<DownloadOutcome>)> onDone)
        : mOnProgress(std::move(onProgress)), mOnDone(std::move(onDone))
    {}

    void onTransferUpdate(mega::MegaApi* /*api*/, mega::MegaTransfer* transfer) override
    {
        mOnProgress(static_cast<std::uint64_t>(transfer->getTransferredBytes()),
                    static_cast<std::uint64_t>(transfer->getTotalBytes()));
    }

    void onTransferFinish(mega::MegaApi* /*api*/,
                          mega::MegaTransfer* transfer,
                          mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            const char* path = transfer->getPath();
            DownloadOutcome outcome;
            outcome.localPath = path ? path : std::string();
            mOnDone(Result<DownloadOutcome>::ok(std::move(outcome)));
        }
        else
        {
            mOnDone(Result<DownloadOutcome>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(std::uint64_t, std::uint64_t)> mOnProgress;
    std::function<void(Result<DownloadOutcome>)> mOnDone;
};

// Same shape as DownloadListener.
class UploadListener : public mega::MegaTransferListener
{
public:
    UploadListener(std::function<void(std::uint64_t, std::uint64_t)> onProgress,
                   std::function<void(Result<UploadOutcome>)> onDone)
        : mOnProgress(std::move(onProgress)), mOnDone(std::move(onDone))
    {}

    void onTransferUpdate(mega::MegaApi* /*api*/, mega::MegaTransfer* transfer) override
    {
        mOnProgress(static_cast<std::uint64_t>(transfer->getTransferredBytes()),
                    static_cast<std::uint64_t>(transfer->getTotalBytes()));
    }

    void onTransferFinish(mega::MegaApi* /*api*/,
                          mega::MegaTransfer* transfer,
                          mega::MegaError* e) override
    {
        int code = e->getErrorCode();
        if (code == mega::MegaError::API_OK)
        {
            UploadOutcome outcome;
            outcome.nodeHandle = static_cast<std::uint64_t>(transfer->getNodeHandle());
            mOnDone(Result<UploadOutcome>::ok(std::move(outcome)));
        }
        else
        {
            mOnDone(Result<UploadOutcome>::fail(e->getErrorString(), code));
        }
        delete this;
    }

private:
    std::function<void(std::uint64_t, std::uint64_t)> mOnProgress;
    std::function<void(Result<UploadOutcome>)> mOnDone;
};

// The one listener that collects bytes rather than a path: startStreaming hands
// them over in onTransferData and writes no file at all.
//
// No mutex around mBuffer -- the SDK serializes one transfer's callbacks, the same
// assumption DownloadListener already makes across onTransferUpdate/onTransferFinish.
class StreamingContentListener : public mega::MegaTransferListener
{
public:
    StreamingContentListener(std::uint64_t maxBytes,
                             std::function<void(Result<std::vector<char>>)> onDone)
        : mMaxBytes(maxBytes), mOnDone(std::move(onDone))
    {}

    bool onTransferData(mega::MegaApi* /*api*/,
                        mega::MegaTransfer* /*transfer*/,
                        char* buffer,
                        size_t size) override
    {
        // The SDK owns buffer and reuses it after this returns, so the copy is
        // mandatory, not an optimisation left undone.
        if (mBuffer.size() + size > mMaxBytes)
        {
            mOverflowed = true;
            return false; // aborts the transfer; the only reason we ever refuse
        }
        mBuffer.insert(mBuffer.end(), buffer, buffer + size);
        return true;
    }

    void onTransferFinish(mega::MegaApi* /*api*/,
                          mega::MegaTransfer* /*transfer*/,
                          mega::MegaError* e) override
    {
        // mOverflowed first: refusing above ends the transfer as API_EINCOMPLETE,
        // so the error code alone would report it as a network failure.
        if (mOverflowed)
        {
            mOnDone(Result<std::vector<char>>::fail("File is larger than the preview limit",
                                                    MegaErrorCode::kETooMany));
        }
        else if (e->getErrorCode() == mega::MegaError::API_OK)
        {
            mOnDone(Result<std::vector<char>>::ok(std::move(mBuffer)));
        }
        else
        {
            mOnDone(Result<std::vector<char>>::fail(e->getErrorString(), e->getErrorCode()));
        }
        delete this;
    }

private:
    std::uint64_t mMaxBytes;
    std::function<void(Result<std::vector<char>>)> mOnDone;
    std::vector<char> mBuffer;
    bool mOverflowed = false;
};

// readFileRangeStreamed's listener: hands each piece on instead of keeping it.
class StreamingChunkListener : public mega::MegaTransferListener
{
public:
    StreamingChunkListener(std::function<bool(const char*, std::size_t)> onChunk,
                           std::function<void(Result<void>)> onDone)
        : mOnChunk(std::move(onChunk)), mOnDone(std::move(onDone))
    {}

    bool onTransferData(mega::MegaApi* /*api*/,
                        mega::MegaTransfer* transfer,
                        char* buffer,
                        size_t size) override
    {
        // The piece's offset never reaches us (pread_data drops it), but the SDK adds
        // the piece to getTransferredBytes() first, so a gap or a repeat shows here.
        mReceived += size;
        if (mReceived != static_cast<std::uint64_t>(transfer->getTransferredBytes()))
        {
            mOutOfStep = true;
            return false;
        }
        if (!mOnChunk(buffer, size))
        {
            mRefused = true;
            return false;
        }
        return true;
    }

    void onTransferFinish(mega::MegaApi* /*api*/,
                          mega::MegaTransfer* /*transfer*/,
                          mega::MegaError* e) override
    {
        // Our own refusals before the error code: pread_data reports a transfer whose
        // *last* piece was refused as API_OK.
        if (mOutOfStep)
        {
            mOnDone(Result<void>::fail("Streamed data fell out of step with the transfer",
                                       MegaErrorCode::kEInternal));
        }
        else if (mRefused)
        {
            mOnDone(Result<void>::fail("Stopped by the receiver", MegaErrorCode::kEIncomplete));
        }
        else if (e->getErrorCode() == mega::MegaError::API_OK)
        {
            mOnDone(Result<void>::ok());
        }
        else
        {
            mOnDone(Result<void>::fail(e->getErrorString(), e->getErrorCode()));
        }
        delete this;
    }

private:
    std::function<bool(const char*, std::size_t)> mOnChunk;
    std::function<void(Result<void>)> mOnDone;
    std::uint64_t mReceived = 0;
    bool mOutOfStep = false;
    bool mRefused = false;
};

} // namespace megasdk
