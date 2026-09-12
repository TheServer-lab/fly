#include "flyrt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// §5.2. Every function takes/returns tex paths; failures throw a `tex` Fly
// error via fly_rt_throw (see flyrt.h's §5-section comment on ownership
// and error conventions) rather than returning a sentinel, so a Fly
// program handles them with ordinary `do`/`grabe`.

static const char* pathCStr(FlyValue v) {
    if (v.tag != FLY_TEX) fly_rt_throw("type error: filesystem.* path must be tex");
    return fly_rt_text_data(v);
}

static void throwErrno(const char* op, const char* path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "filesystem.%s('%s'): %s", op, path, strerror(errno));
    fly_rt_throw(buf);
}

FlyValue fly_rt_file_exists(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    return fly_rt_yn(access(path, F_OK) == 0);
}

FlyValue fly_rt_file_isfile(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    struct stat st;
    return fly_rt_yn(stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

FlyValue fly_rt_file_isdir(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    struct stat st;
    return fly_rt_yn(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

FlyValue fly_rt_file_read(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    FILE* f = fopen(path, "rb");
    if (!f) throwErrno("read", path);

    if (fseek(f, 0, SEEK_END) != 0) { int e = errno; fclose(f); errno = e; throwErrno("read", path); }
    long size = ftell(f);
    if (size < 0) { int e = errno; fclose(f); errno = e; throwErrno("read", path); }
    rewind(f);

    size_t sz = (size_t)size;
    char* buf = (char*)malloc(sz > 0 ? sz : 1);
    if (!buf) { fclose(f); fly_rt_throw("out of memory reading file"); }
    size_t got = sz > 0 ? fread(buf, 1, sz, f) : 0;
    if (ferror(f)) { fclose(f); free(buf); throwErrno("read", path); }
    fclose(f);

    FlyValue out = fly_rt_text_from_bytes(buf, got);
    free(buf);
    return out;
}

static FlyValue writeOrAppend(FlyValue pathV, FlyValue textV, const char* mode, const char* opName) {
    const char* path = pathCStr(pathV);
    if (textV.tag != FLY_TEX) fly_rt_throw("type error: filesystem write/append expects tex content");
    FILE* f = fopen(path, mode);
    if (!f) throwErrno(opName, path);
    size_t len = fly_rt_text_len(textV);
    size_t written = len > 0 ? fwrite(fly_rt_text_data(textV), 1, len, f) : 0;
    if (written != len) { int e = errno; fclose(f); errno = e; throwErrno(opName, path); }
    if (fclose(f) != 0) throwErrno(opName, path);
    return fly_rt_emp();
}

FlyValue fly_rt_file_write(FlyValue pathV, FlyValue textV) { return writeOrAppend(pathV, textV, "wb", "write"); }
FlyValue fly_rt_file_append(FlyValue pathV, FlyValue textV) { return writeOrAppend(pathV, textV, "ab", "append"); }

FlyValue fly_rt_dir_create(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    #ifdef _WIN32
    if (mkdir(path) != 0 && errno != EEXIST) {
        throwErrno("mkdir", path);
    }
#else
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        throwErrno("mkdir", path);
    }
#endif
    return fly_rt_emp();
}

FlyValue fly_rt_file_remove(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    // Try both: `path` may name a file or an empty directory, and §5.2
    // doesn't distinguish a separate "rmdir" entry point.
    if (remove(path) == 0) return fly_rt_emp();
    int firstErrno = errno;
    if (rmdir(path) == 0) return fly_rt_emp();
    errno = firstErrno;
    throwErrno("remove", path);
    return fly_rt_emp(); // unreachable
}

FlyValue fly_rt_dir_list(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    DIR* d = opendir(path);
    if (!d) throwErrno("list", path);

    FlyValue coll = fly_rt_coll_new(16);
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        FlyValue name = fly_rt_text_from_cstr(ent->d_name);
        fly_rt_coll_push(coll, name);
        fly_rt_release(name);
    }
    closedir(d);
    return coll;
}

FlyValue fly_rt_getcwd(void) {
    size_t cap = 256;
    char* buf = (char*)malloc(cap);
    if (!buf) fly_rt_throw("out of memory in filesystem.cwd");
    while (!getcwd(buf, cap)) {
        if (errno != ERANGE) { free(buf); throwErrno("cwd", "."); }
        cap *= 2;
        char* n = (char*)realloc(buf, cap);
        if (!n) { free(buf); fly_rt_throw("out of memory in filesystem.cwd"); }
        buf = n;
    }
    FlyValue out = fly_rt_text_from_cstr(buf);
    free(buf);
    return out;
}

FlyValue fly_rt_chdir(FlyValue pathV) {
    const char* path = pathCStr(pathV);
    if (chdir(path) != 0) throwErrno("chdir", path);
    return fly_rt_emp();
}
