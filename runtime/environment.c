#include "flyrt.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static const char* nameCStr(FlyValue v, const char* what) {
    if (v.tag != FLY_TEX) fly_rt_throw(what);
    return fly_rt_text_data(v);
}

// EMP for an unset variable -- matches fly_rt_board_get's existing
// "EMP if absent" convention (flyrt.h) rather than throwing, since an
// unset environment variable is an ordinary, expected outcome.
FlyValue fly_rt_environment_get(FlyValue nameV) {
    const char* name = nameCStr(nameV, "type error: environment.get expects a tex name");
    const char* val = getenv(name);
    return val ? fly_rt_text_from_cstr(val) : fly_rt_emp();
}

FlyValue fly_rt_environment_set(FlyValue nameV, FlyValue valueV) {
    const char* name = nameCStr(nameV, "type error: environment.set expects a tex name");
    if (valueV.tag != FLY_TEX) fly_rt_throw("type error: environment.set expects a tex value");

    const char* value = fly_rt_text_data(valueV);

#ifdef _WIN32
    if (_putenv_s(name, value) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "environment.set('%s'): failed", name);
        fly_rt_throw(buf);
    }
#else
    if (setenv(name, value, /*overwrite=*/1) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "environment.set('%s'): %s", name, strerror(errno));
        fly_rt_throw(buf);
    }
#endif

    return fly_rt_emp();
}

// Declared in flyrt.h and used by the environment.* builtins (`exists` --
// `has` is a Fly keyword, §15.6) but was never implemented, which made any
// environment_api program link-fail with "undefined reference to
// `fly_rt_environment_has'" on EVERY platform -- this also ships the fly
// stdlib's environment.fly doc/declaration to a working state.
FlyValue fly_rt_environment_has(FlyValue nameV) {
    const char* name = nameCStr(nameV, "type error: environment.exists expects a tex name");
    const char* val = getenv(name);
    return val ? fly_rt_yn(1) : fly_rt_yn(0);
}