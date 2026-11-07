#include "compiler_stack.h"
#include <cerrno>
#include <cstddef>
#include <exception>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace sokol_slang {
namespace {
constexpr size_t compiler_stack_size = 8 * 1024 * 1024;

// The caller owns this state until join. No exception crosses the thread entry.
struct CompilerStackCall {
    std::function<void()> callback;
    std::exception_ptr exception;
    void run() noexcept {
        try {
            callback();
        } catch (...) {
            exception = std::current_exception();
        }
    }
};

#if defined(_WIN32)
unsigned __stdcall compiler_stack_entry(void *context) {
    static_cast<CompilerStackCall *>(context)->run();
    return 0;
}
#else
void *compiler_stack_entry(void *context) {
    static_cast<CompilerStackCall *>(context)->run();
    return nullptr;
}
#endif
} // namespace

void run_with_compiler_stack(std::function<void()> callback) {
    CompilerStackCall call{std::move(callback), {}};
#if defined(_WIN32)
    const auto thread = _beginthreadex(nullptr, unsigned(compiler_stack_size), compiler_stack_entry,
                                       &call, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread)
        throw std::system_error(errno, std::generic_category(), "Create compiler thread");
    const auto handle = reinterpret_cast<HANDLE>(thread);
    // A failed join cannot release the callback's borrowed state safely.
    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0)
        std::terminate();
    if (!CloseHandle(handle))
        throw std::system_error(int(GetLastError()), std::system_category(),
                                "Close compiler thread");
#else
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error)
        throw std::system_error(error, std::generic_category(),
                                "Initialize compiler thread attributes");
    error = pthread_attr_setstacksize(&attributes, compiler_stack_size);
    if (error) {
        pthread_attr_destroy(&attributes);
        throw std::system_error(error, std::generic_category(), "Set compiler thread stack size");
    }
    pthread_t thread;
    error = pthread_create(&thread, &attributes, compiler_stack_entry, &call);
    const int destroy_error = pthread_attr_destroy(&attributes);
    if (error)
        throw std::system_error(error, std::generic_category(), "Create compiler thread");
    // A failed join cannot release the callback's borrowed state safely.
    if (pthread_join(thread, nullptr))
        std::terminate();
    if (destroy_error)
        throw std::system_error(destroy_error, std::generic_category(),
                                "Destroy compiler thread attributes");
#endif
    if (call.exception)
        std::rethrow_exception(call.exception);
}
} // namespace sokol_slang
