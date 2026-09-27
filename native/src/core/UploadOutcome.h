#pragma once
#include <cstdint>

// Success payload for IMegaClient::upload's completion callback.
struct UploadOutcome
{
    std::uint64_t nodeHandle = 0; // MegaTransfer::getNodeHandle() of the created node
};
