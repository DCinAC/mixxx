#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Runs JavaScript with QuickJS-NG and returns what it printed (console.log / print), or NULL with *error
/// set. Both strings are malloc'd: free() them. stack_size: QuickJS's own limit, below the thread's stack.
char* zydek_js_run(const char* code, size_t len, size_t stack_size, char** error);

#ifdef __cplusplus
}
#endif
