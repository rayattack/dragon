/// Dragon Runtime - getpass: no-echo password entry, POSIX termios.
///
/// Its OWN translation unit (mirroring runtime_ed25519.cpp / runtime_crypto.cpp):
/// the security-critical terminal-state dance is done entirely in C so it is
/// atomic and the ECHO flag is *always* restored - even on EOF, EINTR, or a
/// read error. A program that never prompts for a password pays nothing.
///
/// The .dr side (stdlib/getpass.dr) owns the policy: prompt default, the
/// GetPassWarning fallback when stdin is not a tty, and getuser()'s env lookup.
/// This TU exposes the two primitives that genuinely need C:
///   dragon_getpass_read  - read one line from /dev/tty (or stderr/stdin) with
///                          ECHO cleared, restoring the saved termios on every
///                          exit path.
///   dragon_getpass_pwname - getpwuid(getuid())->pw_name, the last-resort
///                          username source for getuser() (matches CPython).
///
/// Windows is deferred per D019; on _WIN32 these degrade to an echoing read and
/// an empty username so the module still links.

// Request POSIX.1-2008 so glibc declares getline()/ssize_t even when the TU is
// compiled as strict -std=c++17 (no _GNU_SOURCE). Harmless if already defined
// by the build's gnu++ extensions.
#ifndef _WIN32
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
#endif

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

//===----------------------------------------------------------------------===//
// Runtime helpers borrowed from other TUs (declared here, NOT in
// runtime_internal.h - this TU owns its own forward declarations).
//
// dragon_string_alloc copies `len` bytes into a fresh heap DragonString and
// returns the `.data` pointer Dragon treats as a `str` (refcount = 1, owned by
// the caller). The empty-string and error paths route through it so every
// return value is a real Dragon str, never a borrowed C literal.
//===----------------------------------------------------------------------===//
extern "C" const char* dragon_string_alloc(const char* src, int64_t len);

extern "C" {

/// Read one line of input with terminal echo disabled, then restore the
/// terminal to its prior state. Returns a freshly-allocated Dragon `str` with
/// the trailing newline stripped (CPython strips the '\n', keeps the rest).
///
/// Behavior, matching CPython's _getpass on POSIX:
///   * The prompt is written to /dev/tty when it can be opened (so the prompt
///     is visible even with stdout redirected); otherwise to stderr.
///   * Input is read from /dev/tty when available, else from stdin (fd 0).
///   * tcgetattr saves the current attrs; ECHO is cleared (ECHONL kept so the
///     user's Enter still produces a visible newline) and applied with
///     TCSAFLUSH (discards any typed-ahead echoed bytes). The saved attrs are
///     restored on EVERY exit path - EOF, EINTR, or error included.
///
/// If stdin is not a terminal, tcgetattr fails with ENOTTY; the .dr layer is
/// expected to have already taken the GetPassWarning echo path, but we still
/// degrade gracefully here by reading with echo (no termios change to undo).
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

    // ALWAYS restore the terminal before returning - this is the security
    // invariant. Done before building the result so an allocation fault can't
    // leave echo disabled.
    if (restore) {
        // Retry across EINTR so a signal can't leave the terminal silent.
        while (tcsetattr(in_fd, TCSAFLUSH, &saved) != 0 && errno == EINTR) {}
        // tcsetattr with TCSAFLUSH already drained; emit a trailing newline to
        // mirror the Enter the user pressed (their newline was swallowed by the
        // flush in some shells), matching CPython's getpass which prints '\n'.
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

/// Last-resort username for getuser(): getpwuid(getuid())->pw_name.
/// Returns "" when the uid has no passwd entry (the .dr layer falls back to
/// env vars first, this only when those are unset). Matches CPython's
/// getpass.getuser() final branch.
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
