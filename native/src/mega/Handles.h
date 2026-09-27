#pragma once
#include <cstdint>
#include <optional>
#include <string>

// MEGA's 8-character base64 form of a node handle, as the web client and MEGAcmd show it.
// Here rather than in the host so only src/mega includes <megaapi.h>.
std::string handleToBase64(std::uint64_t handle);
std::optional<std::uint64_t> base64ToHandle(const std::string& text);
