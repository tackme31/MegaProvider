#pragma once
#include <cstdint>
#include <string>

// What the node's public link looks like right now, all local reads. MegaProvider only
// (MegaExplorer reads these one at a time).
struct LinkDetails
{
    bool exported = false;       // the rest is meaningless when false
    std::string url;             // with the key
    std::int64_t created = 0;    // Unix seconds
    std::int64_t expires = 0;    // Unix seconds; 0 = never
    bool expired = false;
    bool takenDown = false;
};
