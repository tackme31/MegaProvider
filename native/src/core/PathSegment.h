#pragma once
#include "ViewKind.h"

#include <cstdint>
#include <string>

// One breadcrumb segment. isRoot follows FolderNavigationService::Location's
// root-sentinel convention (handle is meaningless when isRoot is true).
struct PathSegment
{
    std::string name;
    std::uint64_t handle = 0;
    bool isRoot = false;
    ViewKind kind = ViewKind::CloudDrive;

    // Field-by-field, as elsewhere. Also what the breadcrumb's change detection
    // compares before re-emitting.
    bool operator==(const PathSegment& other) const
    {
        return name == other.name && handle == other.handle && isRoot == other.isRoot &&
               kind == other.kind;
    }
};
