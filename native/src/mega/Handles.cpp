#include "Handles.h"

#include <megaapi.h>
#include <memory>

std::string handleToBase64(std::uint64_t handle)
{
    std::unique_ptr<char[]> text(mega::MegaApi::handleToBase64(handle)); // new[]-allocated
    return text ? std::string(text.get()) : std::string();
}

std::optional<std::uint64_t> base64ToHandle(const std::string& text)
{
    const mega::MegaHandle handle = mega::MegaApi::base64ToHandle(text.c_str());
    if (handle == mega::INVALID_HANDLE)
        return std::nullopt;
    return handle;
}
