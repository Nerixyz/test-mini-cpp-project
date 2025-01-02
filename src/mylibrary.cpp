#include <iostream>
#include "interface.hpp"

struct Foo {
    Foo()
    {
        std::cout << "Hello from my library\n";
        exeFn();
    }
};

namespace {

const Foo MY_FOO{};

}  // namespace
