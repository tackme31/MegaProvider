#pragma once

// src/core and src/qml can't include SDK headers (mega/*.h), so this mirrors
// just the mega::MegaError values this app's auth/session flow needs to
// branch on. Kept in sync with the real SDK values via a static_assert block
// in src/mega/MegaSdkClient.cpp -- the only file allowed to see both this
// header and megaapi.h.
//
// Value ranges, so app-defined codes can never be mistaken for SDK ones:
//   0        API_OK -- never valid on a failed Result
//   negative mirrors of mega::MegaError, listed below
//   positive app-defined sentinels. Allocated so far:
//              1 kNoStoredSession    (src/core/AuthService.h)
//              2 kClientShutDownCode (src/mega/MegaSdkClient.cpp)
//              3 kPreviewSuperseded  (src/core/PreviewService.h)
//              4 kArchiveEntryInvalid (src/core/ZipEntryExtraction.h)
namespace MegaErrorCode
{
constexpr int kEInternal = -1;     // Internal error -- also what an unclassified failure gets
constexpr int kEArgs = -2;         // Invalid argument
constexpr int kEAgain = -3;        // Request failed, retry recommended (transient)
constexpr int kEFailed = -5;       // Permanent failure (2FA PIN rejection may land here too)
constexpr int kETooMany = -6;      // Too many uses for this resource / rate limited
constexpr int kEExpired = -8;      // Expired
constexpr int kENoEnt = -9;        // Bad credentials, or bad 2FA pin
constexpr int kECircular = -10;    // Move would make a folder its own descendant
constexpr int kEAccess = -11;      // Access denied
constexpr int kEExist = -12;       // Resource already exists (e.g. same-named folder)
constexpr int kEIncomplete = -13;  // Transfer aborted -- what a cancelled transfer finishes with
constexpr int kESid = -15;         // Stored session invalid or expired
constexpr int kEBlocked = -16;     // Account blocked/suspended
constexpr int kEMfaRequired = -26; // Two-factor auth required to complete this request
} // namespace MegaErrorCode
