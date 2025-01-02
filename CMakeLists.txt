cmake_minimum_required(VERSION 3.20)

project(mini-cpp-project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "")
add_executable(executable src/main.cpp)

target_compile_definitions(executable PRIVATE EXECUTABLE_IMPL)
set_target_properties(executable PROPERTIES
    ENABLE_EXPORTS On)

add_library(my-library MODULE src/mylibrary.cpp)
target_link_libraries(my-library PRIVATE executable)
set_target_properties(my-library PROPERTIES PREFIX "")
