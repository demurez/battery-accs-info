#pragma once

#include <optional>
#include <string>
#include <string_view>

// Reads only public, fixed-path files from the Battery Monitor repository.
std::optional<std::string> ReadRepositoryFile(std::wstring_view path, size_t limit);
