/* MIT. Loads shaders solely from the user's DLL; never stores derived files. */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
std::function<std::vector<uint8_t>(const std::string&)> lsfg_user_shaders(const char *path, bool performance = false);
