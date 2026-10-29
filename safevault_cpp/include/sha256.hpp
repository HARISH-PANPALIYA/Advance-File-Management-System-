#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace fileguard {

std::string sha256_bytes(const uint8_t* data, size_t len);
std::string sha256_buffer(const std::vector<uint8_t>& buf);
std::string sha256_string(const std::string& str);
std::string sha256_file(const std::filesystem::path& path);

} // namespace fileguard
