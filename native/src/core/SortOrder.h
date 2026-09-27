#pragma once

// Passed down to IMegaClient so sorting happens server-side (getChildren/search's
// order argument) rather than over tens of thousands of entries in app memory.
enum class SortKey
{
    Name,
    Size,
    ModificationTime,
};

struct SortOrder
{
    SortKey key = SortKey::Name;
    bool ascending = true;
};
