#pragma once
#include <cstdint>
#include <string>

struct FileEntry
{
    std::string name;
    std::uint64_t handle = 0;
    std::uint64_t sizeBytes = 0;
    bool isFolder = false;
    std::int64_t modificationTime = 0;
    bool hasThumbnail = false;
    bool isFavourite = false;
    bool isExported = false;

    // Field-by-field, not <=>-defaulted: this project builds at C++17, and gmock
    // builds an Eq() matcher over vectors of these.
    bool operator==(const FileEntry& other) const
    {
        return name == other.name && handle == other.handle && sizeBytes == other.sizeBytes &&
               isFolder == other.isFolder && modificationTime == other.modificationTime &&
               hasThumbnail == other.hasThumbnail && isFavourite == other.isFavourite &&
               isExported == other.isExported;
    }
};
