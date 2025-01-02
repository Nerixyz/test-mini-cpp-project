#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>

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

#    define DYLIB_EXT ".so"

namespace loading {

inline void loadLibrary(const std::filesystem::path &path)
{
    std::cout << "loading " << path << '\n';
    auto *addr = ::dlopen(path.c_str(), 0);
    if (addr == nullptr)
    {
        std::cout << "loading failed:" << ::dlerror() << '\n';
        exit(EXIT_FAILURE);
    }
}

}  // namespace loading

#endif
