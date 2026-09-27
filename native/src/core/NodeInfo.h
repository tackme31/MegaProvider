#pragma once
#include <cstdint>
#include <string>

// A node's current identity, resolved from its handle alone.
//
// inCloud separates a live Cloud Drive node from one in the Rubbish bin or Vault: a
// MEGA "delete" is a move, so the handle stays resolvable and a caller holding only
// a handle can't otherwise tell. getPath is no substitute -- it normalizes whatever
// root the parent walk ends at into the Cloud Drive sentinel.
struct NodeInfo
{
    std::string name;
    std::uint64_t handle = 0;
    bool isFolder = false;
    bool inCloud = false;

    // Field-by-field, not <=>-defaulted: this project builds at C++17, and gmock
    // builds an Eq() matcher over vectors of these.
    bool operator==(const NodeInfo& other) const
    {
        return name == other.name && handle == other.handle && isFolder == other.isFolder &&
               inCloud == other.inCloud;
    }
};
