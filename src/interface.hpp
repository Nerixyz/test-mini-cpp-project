#pragma once

#ifndef EXE_EXPORT
#    ifdef EXECUTABLE_IMPL  // We are building this library
#        ifdef _MSC_VER
#            define EXE_EXPORT __declspec(dllexport)
#        else
#            define EXE_EXPORT __attribute__((visibility("default")))
#        endif
#    else  // We are using this library
#        ifdef _MSC_VER
#            define EXE_EXPORT __declspec(dllimport)
#        else
#            define EXE_EXPORT __attribute__((visibility("default")))
#        endif
#    endif
#endif

EXE_EXPORT void exeFn();
