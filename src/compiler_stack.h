#pragma once
#include <functional>

namespace sokol_slang {
// Borrows the callback until its private CPU thread joins, then rethrows any
// callback exception on the caller. Callers control the number of active calls.
void run_with_compiler_stack(std::function<void()> callback);
} // namespace sokol_slang
