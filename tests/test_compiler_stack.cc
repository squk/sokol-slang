#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "compiler_stack.h"
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

static size_t stack_size() {
#if defined(_WIN32)
    char marker;
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(&marker, &memory, sizeof(memory)) != 0, "Cannot query compiler stack");
    // The marker is near the stack top; allocation base includes its reservation.
    return uintptr_t(&marker) - uintptr_t(memory.AllocationBase);
#elif defined(__APPLE__)
    return pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t attributes;
    require(pthread_getattr_np(pthread_self(), &attributes) == 0,
            "Cannot query compiler attributes");
    size_t size = 0;
    const int error = pthread_attr_getstacksize(&attributes, &size);
    pthread_attr_destroy(&attributes);
    require(error == 0, "Cannot query compiler stack size");
    return size;
#endif
}

static void check_call() {
    const auto caller = std::this_thread::get_id();
    bool completed = false;
    sokol_slang::run_with_compiler_stack([&] {
        require(std::this_thread::get_id() != caller, "Compiler must leave the caller stack");
#if defined(_WIN32)
        require(stack_size() >= 7 * 1024 * 1024, "Compiler needs an 8 MiB stack reservation");
#else
        require(stack_size() >= 8 * 1024 * 1024, "Compiler needs at least 8 MiB of stack");
#endif
        completed = true;
    });
    require(completed, "Compiler callback must finish before returning");
}

int main() {
    try {
        check_call();
        bool rethrown = false;
        try {
            sokol_slang::run_with_compiler_stack(
                [] { throw std::runtime_error("worker failure"); });
        } catch (const std::runtime_error &error) {
            rethrown = std::string(error.what()) == "worker failure";
        }
        require(rethrown, "Compiler exceptions must return to the caller");
        auto first = std::async(std::launch::async, check_call);
        auto second = std::async(std::launch::async, check_call);
        first.get();
        second.get();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
