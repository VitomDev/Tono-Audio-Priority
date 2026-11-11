#pragma once

#include <filesystem>

namespace tono {

std::filesystem::path ExecutablePath();

// %LOCALAPPDATA%\Tono Audio Priority: only HeadsetControl, when downloaded.
// Settings live in the registry.
std::filesystem::path DataFolder();

}  // namespace tono
