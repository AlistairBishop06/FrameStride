#pragma once

#include <cstdint>

namespace stride::legacy {
int manifest(void* apps, const char* path, bool remove);
bool installed(void* apps, const char* key);
int identify(void* apps, std::uint32_t pid, const char* key);
const char* errorName(void* apps, int error);
std::uint32_t scene(void* apps);
}
