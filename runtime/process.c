#include "flyrt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

// §5.1 Process API. Backs stdlib/process.fly's args/run/spawn/wait/exit

static int g_argc = 0;
static char** g_argv = NULL;

void fly_rt_init_args(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
}

FlyValue fly_rt_args(void) {
    int n = g_argc > 1 ? g_argc - 1 : 0;
    FlyValue coll = fly_rt_coll_new((size_t)(n > 0 ? n : 1));

    for (int i = 1; i < g_argc; i++) {
        FlyValue s = fly_rt_text_from_cstr(g_argv[i]);
        fly_rt_coll_push(coll, s);
        fly_rt_release(s);
    }

    return coll;
}

static const char* programCStr(FlyValue v) {
    if (v.tag != FLY_TEX)
        fly_rt_throw("type error: process.* expects a tex program name/path");

    return fly_rt_text_data(v);
}

static char** buildArgv(const char* program, FlyValue argsV, size_t* outCount) {
    if (argsV.tag != FLY_COLL)
        fly_rt_throw("type error: process.* expects a coll of tex for args");

    FlyColl* args = (FlyColl*)(intptr_t)argsV.payload;

    for (size_t i = 0; i < args->len; i++) {
        if (args->items[i].tag != FLY_TEX)
            fly_rt_throw("type error: process.* args coll must contain only tex");
    }

    size_t n = args->len;

    char** argv = (char**)malloc((n + 2) * sizeof(char*));
    if (!argv)
        fly_rt_throw("out of memory in process.*");

    argv[0] = (char*)program;

    for (size_t i = 0; i < n; i++)
        argv[i + 1] = (char*)fly_rt_text_data(args->items[i]);

    argv[n + 1] = NULL;

    if (outCount)
        *outCount = n;

    return argv;
}

#ifndef _WIN32

static void throwErrno(const char* op, const char* program) {
    char buf[512];

    snprintf(
        buf,
        sizeof(buf),
        "process.%s('%s'): %s",
        op,
        program,
        strerror(errno)
    );

    fly_rt_throw(buf);
}

static pid_t forkExec(const char* program, char** argv) {
    int errPipe[2];

    if (pipe(errPipe) != 0)
        throwErrno("run", program);

    pid_t pid = fork();

    if (pid < 0) {
        close(errPipe[0]);
        close(errPipe[1]);
        throwErrno("run", program);
    }

    if (pid == 0) {
        close(errPipe[0]);

        execvp(program, argv);

        int e = errno;

        ssize_t written = write(errPipe[1], &e, sizeof(e));
        (void)written;

        _exit(127);
    }

    close(errPipe[1]);

    int childErrno = 0;

    ssize_t got = read(
        errPipe[0],
        &childErrno,
        sizeof(childErrno)
    );

    close(errPipe[0]);

    if (got == (ssize_t)sizeof(childErrno)) {
        int status;
        waitpid(pid, &status, 0);

        errno = childErrno;
        throwErrno("run", program);
    }

    return pid;
}

static int64_t statusToExitCode(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return -1;
}

FlyValue fly_rt_process_run(FlyValue programV, FlyValue argsV) {
    const char* program = programCStr(programV);

    char** argv = buildArgv(program, argsV, NULL);

    pid_t pid = forkExec(program, argv);

    free(argv);

    int status;

    if (waitpid(pid, &status, 0) < 0)
        throwErrno("run", program);

    return fly_rt_num(statusToExitCode(status));
}

FlyValue fly_rt_process_spawn(FlyValue programV, FlyValue argsV) {
    const char* program = programCStr(programV);

    char** argv = buildArgv(program, argsV, NULL);

    pid_t pid = forkExec(program, argv);

    free(argv);

    return fly_rt_num((int64_t)pid);
}

FlyValue fly_rt_process_wait(FlyValue childV) {
    if (childV.tag != FLY_NUM)
        fly_rt_throw(
            "type error: process.wait expects the num pid returned by process.spawn"
        );

    pid_t pid = (pid_t)childV.payload;

    int status;

    if (waitpid(pid, &status, 0) < 0) {
        char buf[128];

        snprintf(
            buf,
            sizeof(buf),
            "process.wait(%lld): %s",
            (long long)pid,
            strerror(errno)
        );

        fly_rt_throw(buf);
    }

    return fly_rt_num(statusToExitCode(status));
}

#else

static void throwWindowsError(const char* op, const char* program) {
    DWORD err = GetLastError();

    char sysmsg[256];
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        0,
        sysmsg,
        (DWORD)sizeof(sysmsg),
        NULL
    );

    if (n > 0) {
        while (n > 0 &&
               (sysmsg[n - 1] == '\r' || sysmsg[n - 1] == '\n')) {
            sysmsg[--n] = '\0';
        }
    } else {
        snprintf(
            sysmsg,
            sizeof(sysmsg),
            "Windows error %lu",
            (unsigned long)err
        );
    }

    char buf[512];

    snprintf(
        buf,
        sizeof(buf),
        "process.%s('%s'): %s",
        op,
        program,
        sysmsg
    );

    fly_rt_throw(buf);
}

static char* buildWindowsCommandLine(
    const char* program,
    char** argv
) {
    // Each token is quoted ONLY when it needs to be (empty, or contains a
    // space/tab/quote). This is what lets process.run("cmd.exe", "/c",
    // "exit", "7") work: cmd's own /C rule strips the FIRST and LAST quote
    // of the whole command string, so an always-quoted first token like
    // "\"cmd.exe\" \"/c\"..." loses its trailing quotes and cmd ends up
    // seeing garbage such as '"exit"' as the command to run. Unquoted
    // space-free tokens still parse identically under the standard CRT
    // argv rules (CommandLineToArgvW), so plain exe children are unaffected.

    size_t total = 1;

    for (size_t i = 0; argv[i] != NULL; i++) {
        const char* s = argv[i];

        // Over-reserve quotes (not every token gets them) -- safe upper bound.
        total += 3; // quotes + space margin

        int needQuote = (s[0] == '\0') || strpbrk(s, " \t\"") != NULL;

        for (const char* p = s; *p; p++) {
            total++;

            if (needQuote && (*p == '\\' || *p == '"'))
                total++;
        }
    }

    char* cmd = (char*)malloc(total);

    if (!cmd)
        fly_rt_throw("out of memory in process.*");

    char* out = cmd;

    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i != 0)
            *out++ = ' ';

        const char* s = argv[i];

        int needQuote = (s[0] == '\0') || strpbrk(s, " \t\"") != NULL;

        if (needQuote)
            *out++ = '"';

        size_t backslashes = 0;

        for (const char* p = s; *p; p++) {
            if (*p == '\\') {
                backslashes++;
                continue;
            }

            if (*p == '"') {
                for (size_t j = 0; j < backslashes * 2 + 1; j++)
                    *out++ = '\\';

                *out++ = '"';
                backslashes = 0;
                continue;
            }

            for (size_t j = 0; j < backslashes; j++)
                *out++ = '\\';

            backslashes = 0;
            *out++ = *p;
        }

        // Trailing backslashes are doubled only when a closing quote follows.
        for (size_t j = 0; j < backslashes * (needQuote ? 2 : 1); j++)
            *out++ = '\\';

        if (needQuote)
            *out++ = '"';
    }

    *out = '\0';

    return cmd;
}

typedef struct {
    PROCESS_INFORMATION pi;
} FlyWindowsProcess;

static FlyWindowsProcess spawnWindows(
    const char* program,
    char** argv
) {
    char* cmdline = buildWindowsCommandLine(program, argv);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));

    si.cb = sizeof(si);

    BOOL ok = CreateProcessA(
        NULL,
        cmdline,
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    free(cmdline);

    if (!ok)
        throwWindowsError("run", program);

    CloseHandle(pi.hThread);

    FlyWindowsProcess proc;
    proc.pi = pi;

    return proc;
}

FlyValue fly_rt_process_run(FlyValue programV, FlyValue argsV) {
    const char* program = programCStr(programV);

    char** argv = buildArgv(program, argsV, NULL);

    FlyWindowsProcess proc = spawnWindows(program, argv);

    free(argv);

    DWORD waitResult = WaitForSingleObject(
        proc.pi.hProcess,
        INFINITE
    );

    if (waitResult != WAIT_OBJECT_0) {
        CloseHandle(proc.pi.hProcess);
        throwWindowsError("run", program);
    }

    DWORD exitCode = 0;

    if (!GetExitCodeProcess(proc.pi.hProcess, &exitCode)) {
        CloseHandle(proc.pi.hProcess);
        throwWindowsError("run", program);
    }

    CloseHandle(proc.pi.hProcess);

    return fly_rt_num((int64_t)exitCode);
}

FlyValue fly_rt_process_spawn(FlyValue programV, FlyValue argsV) {
    const char* program = programCStr(programV);

    char** argv = buildArgv(program, argsV, NULL);

    FlyWindowsProcess proc = spawnWindows(program, argv);

    free(argv);

    /*
     * Return the Windows process handle as the Fly process identifier.
     * This keeps process.spawn/wait within a single process object instead
     * of relying on a potentially recycled Windows PID.
     */
    return fly_rt_num((int64_t)(intptr_t)proc.pi.hProcess);
}

FlyValue fly_rt_process_wait(FlyValue childV) {
    if (childV.tag != FLY_NUM)
        fly_rt_throw(
            "type error: process.wait expects the num handle returned by process.spawn"
        );

    HANDLE processHandle =
        (HANDLE)(intptr_t)childV.payload;

    DWORD waitResult = WaitForSingleObject(
        processHandle,
        INFINITE
    );

    if (waitResult != WAIT_OBJECT_0) {
        CloseHandle(processHandle);
        fly_rt_throw("process.wait: WaitForSingleObject failed");
    }

    DWORD exitCode = 0;

    if (!GetExitCodeProcess(processHandle, &exitCode)) {
        CloseHandle(processHandle);
        fly_rt_throw("process.wait: GetExitCodeProcess failed");
    }

    CloseHandle(processHandle);

    return fly_rt_num((int64_t)exitCode);
}

#endif

FlyValue fly_rt_process_exit(FlyValue codeV) {
    if (codeV.tag != FLY_NUM)
        fly_rt_throw("type error: process.exit expects a num exit code");

    exit((int)codeV.payload);

    return fly_rt_emp();
}