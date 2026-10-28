#pragma once
#include <stdexcept>

inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
