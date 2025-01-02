#pragma once

#include <cstdlib>
#include <filesystem>

#ifdef _MSC_VER

#    define WIN32_LEAN_AND_MEAN
#    include <Windows.h>

#    define DYLIB_EXT ".dll"

namespace loading {

inline void loadLibrary(const std::filesystem::path &path)
{
    auto *addr = LoadLibraryW(path.c_str());
    if (addr == nullptr)
    {
        exit(EXIT_FAILURE);
    }
}

}  // namespace loading

#else

#    include <dlfcn.h>

#    ifdef __APPLE__
#        define DYLIB_EXT ".so"
#    else
#        define DYLIB_EXT ".dylib"
#    endif

namespace loading {

inline void loadLibrary(const std::filesystem::path &path)
{
    auto *addr = ::dlopen(path.c_str(), RTLD_GLOBAL);
    if (addr == nullptr)
    {
        exit(EXIT_FAILURE);
    }
}

}  // namespace loading

#endif
