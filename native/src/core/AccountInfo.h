#pragma once
#include <cstdint>
#include <string>

// The three payloads behind the account section of the "More" menu, in one header
// because they are one concept and always used together.

// Everything about the signed-in account that is a local read -- no network.
// Returned synchronously by IMegaClient::currentAccountIdentity().
struct AccountIdentity
{
    std::string email;
    std::string avatarColor;      // "#FF6A19"-style RGB the SDK derives from the user
                                  // handle; what to paint behind the initial when the
                                  // account has no avatar set
    std::uint64_t userHandle = 0; // keys the avatar's on-disk cache file, so two
                                  // accounts can't share one

    bool operator==(const AccountIdentity& other) const
    {
        return email == other.email && avatarColor == other.avatarColor &&
               userHandle == other.userHandle;
    }
};

// Storage quota and plan. The one genuine server round-trip in this feature.
struct AccountInfo
{
    std::uint64_t storageUsedBytes = 0;
    std::uint64_t storageMaxBytes = 0; // 0 is a legitimate answer for Business /
                                       // Pro Flexi accounts, so dividing by it needs
                                       // a guard
    int proLevel = 0;                  // an AccountPlan.h constant

    bool operator==(const AccountInfo& other) const
    {
        return storageUsedBytes == other.storageUsedBytes &&
               storageMaxBytes == other.storageMaxBytes && proLevel == other.proLevel;
    }
};

// Success payload for loadAvatar. Most MEGA accounts have no avatar, so
// hasAvatar == false is the common path and must never travel as Result::fail.
struct AvatarOutcome
{
    std::string localPath;
    bool hasAvatar = false;

    // Diagnostic only, and must never reach the UI: it rides here because src/core
    // is Qt-free and cannot log. Zero when an avatar was actually fetched.
    int errorCode = 0;
    std::string errorMessage;

    bool operator==(const AvatarOutcome& other) const
    {
        return localPath == other.localPath && hasAvatar == other.hasAvatar &&
               errorCode == other.errorCode && errorMessage == other.errorMessage;
    }
};
