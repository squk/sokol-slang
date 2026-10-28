#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "test_support.h"
#include <cstdio>
#include <string>

template<class T> struct ComOwner {
    T* value = nullptr;
    ~ComOwner() { if (value) value->Release(); }
};

static void check_hr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char error[32];
        std::snprintf(error, sizeof(error), " (HRESULT 0x%08lx)", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(operation) + error);
    }
}
