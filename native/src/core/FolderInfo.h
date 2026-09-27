#pragma once
#include <cstdint>

// Recursive contents of one folder, as MegaApi::getFolderInfo reports them.
//
// Unlike subtreeSize() this is a real request rather than a read of the tree
// already in memory, so it costs a round-trip -- never per row.
struct FolderInfo
{
    std::uint64_t fileCount = 0;
    std::uint64_t folderCount = 0;
    // Current versions only; MegaFolderInfo reports the versions' bytes separately
    // and this project has no screen for them.
    std::uint64_t sizeBytes = 0;

    // Field-by-field, not <=>-defaulted: this project builds at C++17, and gmock
    // builds an Eq() matcher over these.
    bool operator==(const FolderInfo& other) const
    {
        return fileCount == other.fileCount && folderCount == other.folderCount &&
               sizeBytes == other.sizeBytes;
    }
};
