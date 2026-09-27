#pragma once
#include <cstdint>

// Where a node in the Rubbish bin would go if it were restored: the (handle,
// isRoot) pair every move in this codebase is addressed by.
//
// fellBackToRoot records that the folder the node was binned from is gone, so the
// answer is the Cloud Drive root rather than where it came from. The caller needs
// that to word the result -- a restore that quietly lands somewhere else is worse
// than one that says so.
struct RestoreTarget
{
    std::uint64_t handle = 0;
    bool isRoot = true;
    bool fellBackToRoot = true;
};
