#pragma once
#include <string>

// Success payload for IMegaClient::download's completion callback.
struct DownloadOutcome
{
    std::string localPath; // actual saved path; differs from the requested
                           // destinationPath when a name collision made the SDK
                           // suffix the leaf with "(1)" (see IMegaClient::download)
};
