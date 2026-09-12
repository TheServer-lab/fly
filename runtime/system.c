#include "flyrt.h"
#include <string.h>

#ifdef _WIN32
FlyValue fly_rt_os(void)   { return fly_rt_text_from_cstr("windows"); }
FlyValue fly_rt_arch(void) {
#if defined(_M_X64) || defined(__x86_64__)
    return fly_rt_text_from_cstr("x86_64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    return fly_rt_text_from_cstr("arm64");
#else
    return fly_rt_text_from_cstr("unknown");
#endif
}
#include <winsock2.h>
FlyValue fly_rt_hostname(void) {
    // gethostname() is a Winsock call and therefore needs a prior
    // WSAStartup() -- net.c performs one in its constructor, but a program
    // can call system.hostname() (or system.os()/arch(), whose branches
    // don't start Winsock) without ever touching net.*. WSAStartup is
    // ref-counted, so calling it again when net.c already did is harmless.
    static int wsaReady = 0;
    if (!wsaReady) {
        WSADATA wsa;
        if (WSAStartup(0x0202, &wsa) == 0) wsaReady = 1;
    }
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) fly_rt_throw("system.hostname() failed");
    return fly_rt_text_from_cstr(buf);
}
#else
#include <sys/utsname.h>
#include <unistd.h>

// uname()'s sysname/machine are the actual runtime OS/CPU, more precise
// than a compile-time-only guess would be (e.g. distinguishes an x86_64
// binary running under Rosetta from native arm64) -- normalized to the
// lowercase names §5.5's examples use for the well-known cases, passed
// through as-is otherwise (documented rather than silently guessed).
FlyValue fly_rt_os(void) {
    struct utsname u;
    if (uname(&u) != 0) fly_rt_throw("system.os() failed");
    if (strcmp(u.sysname, "Linux") == 0) return fly_rt_text_from_cstr("linux");
    if (strcmp(u.sysname, "Darwin") == 0) return fly_rt_text_from_cstr("macos");
    return fly_rt_text_from_cstr(u.sysname);
}

FlyValue fly_rt_arch(void) {
    struct utsname u;
    if (uname(&u) != 0) fly_rt_throw("system.arch() failed");
    return fly_rt_text_from_cstr(u.machine);
}

FlyValue fly_rt_hostname(void) {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) fly_rt_throw("system.hostname() failed");
    buf[sizeof(buf) - 1] = '\0';
    return fly_rt_text_from_cstr(buf);
}
#endif
