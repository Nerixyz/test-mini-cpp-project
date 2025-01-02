#include "interface.hpp"

#include "loading.hpp"

#include <iostream>

EXE_EXPORT
void exeFn()
{
    std::cout << "Function from executable\n";
}

int main()
{
    loading::loadLibrary("./my-library" DYLIB_EXT);
    std::cout << "After\n";
}
