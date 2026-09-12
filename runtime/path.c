#include "flyrt.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// §5.3: pure string manipulation over tex paths, using platform-appropriate
// separator rules (§5.3: "should not require programs to hard-code platform
// separators"). Reached from Fly via `path.join(...)` etc. dotted-call
// syntax -- see flyrt.h's §5.3 section comment for why these are compiler
// builtins rather than `native job` declarations.
#ifdef _WIN32
#define FLY_PATH_SEP '\\'
#else
#define FLY_PATH_SEP '/'
#endif

static FlyText* asPathText(FlyValue v) {
    if (v.tag != FLY_TEX) fly_rt_throw("type error: path.* expects tex operands");
    return (FlyText*)(intptr_t)v.payload;
}

static int isSep(char c) {
#ifdef _WIN32
    return c == '\\' || c == '/'; // Windows tolerates both; we normalize to FLY_PATH_SEP on output
#else
    return c == '/';
#endif
}

FlyValue fly_rt_path_separator(void) {
    char s[2] = { FLY_PATH_SEP, '\0' };
    return fly_rt_text_from_bytes(s, 1);
}

// components: coll of tex, joined with FLY_PATH_SEP. A component that
// starts with a separator is treated as absolute and resets the
// accumulator (matches the common os.path.join convention: "a later
// absolute component discards everything before it"). Empty components
// are skipped. Requires >= 1 element (enforced by codegen.cpp's genCall,
// which is the only caller -- there's no other way to reach this from Fly
// source since `path.join()` with zero args is rejected there).
FlyValue fly_rt_path_join(FlyValue componentsV) {
    if (componentsV.tag != FLY_COLL) fly_rt_throw("type error: path.join expects a coll of tex");
    FlyColl* c = (FlyColl*)(intptr_t)componentsV.payload;

    size_t cap = 64, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) fly_rt_throw("out of memory in path.join");
    buf[0] = '\0';

    for (size_t i = 0; i < c->len; i++) {
        FlyText* comp = asPathText(c->items[i]);
        if (comp->len == 0) continue;

        size_t start = 0;
        if (isSep(comp->data[0])) {
            len = 0; // absolute component: discard everything accumulated so far
        } else if (len > 0 && !isSep(buf[len - 1])) {
            if (len + 1 + 1 > cap) { cap = (len + 2) * 2; buf = (char*)realloc(buf, cap); if (!buf) fly_rt_throw("out of memory in path.join"); }
            buf[len++] = FLY_PATH_SEP;
        }
        size_t addLen = comp->len - start;
        if (len + addLen + 1 > cap) {
            cap = (len + addLen + 1) * 2;
            buf = (char*)realloc(buf, cap);
            if (!buf) fly_rt_throw("out of memory in path.join");
        }
        memcpy(buf + len, comp->data + start, addLen);
        len += addLen;
        buf[len] = '\0';
    }

    FlyValue out = fly_rt_text_from_bytes(buf, len);
    free(buf);
    return out;
}

// Index of the last separator, or -1 if there isn't one.
static long lastSepIndex(FlyText* t) {
    for (long i = (long)t->len - 1; i >= 0; i--)
        if (isSep(t->data[i])) return i;
    return -1;
}

FlyValue fly_rt_path_basename(FlyValue pathV) {
    FlyText* t = asPathText(pathV);
    long sep = lastSepIndex(t);
    const char* start = t->data + (sep + 1);
    size_t len = t->len - (size_t)(sep + 1);
    return fly_rt_text_from_bytes(start, len);
}

FlyValue fly_rt_path_dirname(FlyValue pathV) {
    FlyText* t = asPathText(pathV);
    long sep = lastSepIndex(t);
    if (sep < 0) return fly_rt_text_from_cstr("."); // no separator -> "the current directory"
    if (sep == 0) return fly_rt_text_from_bytes(t->data, 1); // root ("/foo" -> "/")
    return fly_rt_text_from_bytes(t->data, (size_t)sep);
}

// Finds the extension's leading '.' within `base` (already the basename,
// so no separators to worry about). A leading dot (dotfiles like
// ".bashrc") does NOT count as an extension, matching the common
// path-library convention that a dotfile's whole name is its stem.
static long extensionDotIndex(const char* base, size_t len) {
    for (long i = (long)len - 1; i > 0; i--)
        if (base[i] == '.') return i;
    return -1;
}

FlyValue fly_rt_path_extension(FlyValue pathV) {
    FlyValue baseV = fly_rt_path_basename(pathV);
    FlyText* base = (FlyText*)(intptr_t)baseV.payload;
    long dot = extensionDotIndex(base->data, base->len);
    FlyValue out = dot < 0 ? fly_rt_text_from_cstr("")
                            : fly_rt_text_from_bytes(base->data + dot, base->len - (size_t)dot);
    fly_rt_release(baseV);
    return out;
}

FlyValue fly_rt_path_stem(FlyValue pathV) {
    FlyValue baseV = fly_rt_path_basename(pathV);
    FlyText* base = (FlyText*)(intptr_t)baseV.payload;
    long dot = extensionDotIndex(base->data, base->len);
    FlyValue out = dot < 0 ? fly_rt_retain(baseV) : fly_rt_text_from_bytes(base->data, (size_t)dot);
    fly_rt_release(baseV);
    return out;
}

FlyValue fly_rt_path_absolute(FlyValue pathV) {
    FlyText* t = asPathText(pathV);
    if (t->len > 0 && isSep(t->data[0])) return fly_rt_retain(pathV); // already absolute

    size_t cwdCap = 256;
    char* cwd = (char*)malloc(cwdCap);
    if (!cwd) fly_rt_throw("out of memory in path.absolute");
    while (!getcwd(cwd, cwdCap)) {
        cwdCap *= 2;
        char* n = (char*)realloc(cwd, cwdCap);
        if (!n) { free(cwd); fly_rt_throw("out of memory in path.absolute"); }
        cwd = n;
    }

    FlyValue components = fly_rt_coll_new(2);
    FlyValue cwdV = fly_rt_text_from_cstr(cwd);
    free(cwd);
    fly_rt_coll_push(components, cwdV);
    fly_rt_release(cwdV);
    fly_rt_coll_push(components, pathV);
    FlyValue out = fly_rt_path_join(components);
    fly_rt_release(components);
    return out;
}
