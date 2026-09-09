// Windows stand-ins for the POSIX process/credential calls that stdlib/os/os.dr
// and stdlib/subprocess.dr declare with `extern "C" def`. Dragon has no
// conditional compilation, so those declarations exist on every platform and
// the link needs these symbols to resolve on Windows.
//
// Where Windows has a real equivalent (getppid, kill) it is implemented.
// Where it does not, the call fails the POSIX way -- returns -1 and sets errno
// to ENOSYS -- rather than inventing a plausible-looking answer.
//
// NOTE the deliberate choice on the credential calls: they return -1, not 0.
// On POSIX getuid() cannot fail, so 0 would be the tempting stub, but 0 means
// root and any `if os.uid() == 0` privilege check would silently invert on
// Windows. -1 is the conventional "no valid uid" and fails such checks closed.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <errno.h>
#include <process.h>

// Signal numbers as stdlib/signal.dr uses them.
#define DRAGON_SIGKILL 9
#define DRAGON_SIGTERM 15

extern "C" {

// No Windows equivalent: there is no way to duplicate a process image.
// os.fork_proc() turns the -1 into OSError("fork failed").
int fork(void) {
    errno = ENOSYS;
    return -1;
}

// sig 0 is the POSIX existence probe; SIGTERM/SIGKILL both map to
// TerminateProcess, which is the only kill Windows offers. Anything else has no
// meaning here, so it is refused rather than silently treated as a terminate.
int kill(int pid, int sig) {
    if (pid <= 0) {              // process groups: see setpgid below
        errno = ENOSYS;
        return -1;
    }
    if (sig == 0) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!h) { errno = ESRCH; return -1; }
        CloseHandle(h);
        return 0;
    }
    if (sig != DRAGON_SIGTERM && sig != DRAGON_SIGKILL) {
        errno = EINVAL;
        return -1;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) { errno = (GetLastError() == ERROR_ACCESS_DENIED) ? EPERM : ESRCH; return -1; }
    BOOL ok = TerminateProcess(h, (UINT)(128 + sig));
    CloseHandle(h);
    if (!ok) { errno = EPERM; return -1; }
    return 0;
}

// Windows tracks the parent pid, but only via a snapshot walk.
int getppid(void) {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { errno = ENOSYS; return -1; }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int parent = -1;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) {
                parent = (int)pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (parent < 0) errno = ESRCH;
    return parent;
}

// Windows security is SID-based; there is no uid/gid to report. See the note at
// the top of this file for why these are -1 rather than 0.
int getuid(void)  { errno = ENOSYS; return -1; }
int geteuid(void) { errno = ENOSYS; return -1; }
int getgid(void)  { errno = ENOSYS; return -1; }
int getegid(void) { errno = ENOSYS; return -1; }

// Windows has job objects, not process groups or sessions. Nothing here maps
// cleanly, so all three report unsupported instead of pretending to succeed.
int getpgrp(void) { errno = ENOSYS; return -1; }
int setpgid(int pid, int pgid) { (void)pid; (void)pgid; errno = ENOSYS; return -1; }
int setsid(void)  { errno = ENOSYS; return -1; }

}  // extern "C"

#endif  // _WIN32
