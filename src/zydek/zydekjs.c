// Zydek: QuickJS-NG (lib/quickjs-ng) for yt-dlp's YouTube challenge solver, run inside the app: Android
// doesn't let apps start programs they unpacked themselves, so the usual `qjs` process isn't an option.
// zydekpython.cpp offers it to Python as _zydekjs.run().

#include "zydekjs.h"

#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buffer;

static void append(Buffer* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char* data = realloc(b->data, cap);
        if (!data) {
            return;
        }
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

// console.log / print: the solver's answer comes out here
static JSValue js_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    Buffer* out = JS_GetContextOpaque(ctx);
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) {
            return JS_EXCEPTION;
        }
        if (i) {
            append(out, " ", 1);
        }
        append(out, s, len);
        JS_FreeCString(ctx, s);
    }
    append(out, "\n", 1);
    return JS_UNDEFINED;
}

static char* copy_string(const char* s) {
    size_t n = strlen(s);
    char* r = malloc(n + 1);
    if (r) {
        memcpy(r, s, n + 1);
    }
    return r;
}

char* zydek_js_run(const char* code, size_t len, size_t stack_size, char** error) {
    Buffer out = {0};
    *error = NULL;
    JSRuntime* rt = JS_NewRuntime();
    if (!rt) {
        *error = copy_string("QuickJS: out of memory");
        return NULL;
    }
    JS_SetMaxStackSize(rt, stack_size);
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        *error = copy_string("QuickJS: out of memory");
        return NULL;
    }
    JS_SetContextOpaque(ctx, &out);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_log, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_log, "print", 1));
    JS_FreeValue(ctx, global);

    // JS_Eval wants a NUL-terminated source
    char* source = malloc(len + 1);
    memcpy(source, code, len);
    source[len] = '\0';
    JSValue result = JS_Eval(ctx, source, len, "<yt-dlp>", JS_EVAL_TYPE_GLOBAL);
    free(source);
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(ctx);
        Buffer msg = {0};
        const char* s = JS_ToCString(ctx, exception);
        append(&msg, s ? s : "JavaScript error", strlen(s ? s : "JavaScript error"));
        JS_FreeCString(ctx, s);
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (JS_IsString(stack)) {
            const char* st = JS_ToCString(ctx, stack);
            if (st) {
                append(&msg, "\n", 1);
                append(&msg, st, strlen(st));
                JS_FreeCString(ctx, st);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exception);
        *error = msg.data ? msg.data : copy_string("JavaScript error");
    } else {
        JSContext* pending;
        while (JS_ExecutePendingJob(rt, &pending) > 0) {
        }
    }
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (*error) {
        free(out.data);
        return NULL;
    }
    return out.data ? out.data : copy_string("");
}
