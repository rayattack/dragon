/// Dragon Runtime - getpass: no-echo password entry, POSIX termios. Own TU so
/// the terminal-state dance stays atomic in C, with ECHO always restored (even
/// on EOF/EINTR/error). stdlib/getpass.dr owns policy (prompt default,
/// GetPassWarning fallback, env lookup); Windows degrades to an echoing read (D019).

// Request POSIX.1-2008 so glibc declares getline()/ssize_t under strict
// -std=c++17. Harmless if already defined by the build's gnu++ extensions.
#ifndef _WIN32
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
#endif

#include "runtime_internal.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdio>

#ifndef _WIN32
  #include <sys/types.h>   // ssize_t
  #include <termios.h>
  #include <unistd.h>
  #include <pwd.h>
  #include <errno.h>
#endif

// Forward-declared here (not in runtime_internal.h): copies `len` bytes into
// a fresh heap DragonString, refcount 1, so every return is a real owned str.
extern "C" const char* dragon_string_alloc(const char* src, int64_t len);

extern "C" {

/// Read one line with terminal echo disabled (via /dev/tty, else stderr/stdin),
/// restoring the saved termios on every exit path (EOF/EINTR/error included).
/// If stdin isn't a tty, tcgetattr fails and we degrade to an echoing read.
const char* dragon_getpass_read(const char* prompt) {
#ifdef _WIN32
    // Windows deferred (D019): echo the prompt and read a line with echo on.
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    char buf[4096];
    if (!fgets(buf, (int)sizeof(buf), stdin)) return dragon_string_alloc("", 0);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    return dragon_string_alloc(buf, (int64_t)n);
#else
    // Prefer the controlling terminal so a redirected stdout/stdin doesn't hide
    // the prompt or leak the password into a pipe. Fall back to stderr+stdin.
    FILE* tty_in  = nullptr;
    FILE* tty_out = nullptr;
    bool  use_tty = false;

    FILE* opened = fopen("/dev/tty", "w+");
    if (opened) {
        tty_in  = opened;
        tty_out = opened;
        use_tty = true;
    } else {
        tty_in  = stdin;
        tty_out = stderr;
    }

    int in_fd = fileno(tty_in);

    // Save current terminal attributes; if this fd is not a tty we cannot (and
    // need not) toggle echo - read straight through.
    struct termios saved;
    bool restore = false;
    if (tcgetattr(in_fd, &saved) == 0) {
        struct termios noecho = saved;
        noecho.c_lflag &= ~((tcflag_t)ECHO);   // hide typed characters
        noecho.c_lflag |= (tcflag_t)ECHONL;    // but still echo the final '\n'
        // TCSAFLUSH: change takes effect after queued output drains and any
        // queued (already-echoed) input is discarded.
        if (tcsetattr(in_fd, TCSAFLUSH, &noecho) == 0) {
            restore = true;
        }
    }

    // Emit the prompt (after echo is off so nothing the user typed early shows).
    if (prompt && tty_out) {
        fputs(prompt, tty_out);
        fflush(tty_out);
    }

    // Read one line. getline grows its own buffer; we own and free it.
    char*  line = nullptr;
    size_t cap  = 0;
    ssize_t got = getline(&line, &cap, tty_in);

    // Security invariant: restore the terminal before building the result, so
    // an allocation fault can't leave echo disabled.
    if (restore) {
        while (tcsetattr(in_fd, TCSAFLUSH, &saved) != 0 && errno == EINTR) {}  // retry: EINTR must not leave echo off
        // Mirror the swallowed Enter, matching CPython's getpass trailing '\n'.
        if (tty_out) { fputc('\n', tty_out); fflush(tty_out); }
    }

    const char* result;
    if (got < 0) {
        // EOF or error: CPython raises EOFError on empty EOF; the .dr layer
        // decides policy, so hand back "" and let it react.
        result = dragon_string_alloc("", 0);
    } else {
        // Strip the trailing newline (and a CR if present from CRLF input).
        size_t n = (size_t)got;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        result = dragon_string_alloc(line, (int64_t)n);
    }

    // Wipe the secret out of the temporary buffer before freeing it.
    if (line) {
        if (cap) memset(line, 0, cap);
        free(line);
    }
    if (use_tty && opened) fclose(opened);
    return result;
#endif
}

/// Last-resort username for getuser(): getpwuid(getuid())->pw_name, or ""
/// when the uid has no passwd entry. Matches CPython's getuser() final branch.
const char* dragon_getpass_pwname(void) {
#ifdef _WIN32
    return dragon_string_alloc("", 0);
#else
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        return dragon_string_alloc(pw->pw_name, (int64_t)strlen(pw->pw_name));
    }
    return dragon_string_alloc("", 0);
#endif
}

}  // extern "C"
