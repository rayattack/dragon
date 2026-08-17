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
  #include <sys/types.h>
  #include <termios.h>
  #include <unistd.h>
  #include <pwd.h>
  #include <errno.h>
#endif

extern "C" const char* dragon_string_alloc(const char* src, int64_t len);

extern "C" {

const char* dragon_getpass_read(const char* prompt) {
#ifdef _WIN32
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    char buf[4096];
    if (!fgets(buf, (int)sizeof(buf), stdin)) return dragon_string_alloc("", 0);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    return dragon_string_alloc(buf, (int64_t)n);
#else
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

    struct termios saved;
    bool restore = false;
    if (tcgetattr(in_fd, &saved) == 0) {
        struct termios noecho = saved;
        noecho.c_lflag &= ~((tcflag_t)ECHO);
        noecho.c_lflag |= (tcflag_t)ECHONL;
        if (tcsetattr(in_fd, TCSAFLUSH, &noecho) == 0) {
            restore = true;
        }
    }

    if (prompt && tty_out) {
        fputs(prompt, tty_out);
        fflush(tty_out);
    }

    char*  line = nullptr;
    size_t cap  = 0;
    ssize_t got = getline(&line, &cap, tty_in);

    if (restore) {
        while (tcsetattr(in_fd, TCSAFLUSH, &saved) != 0 && errno == EINTR) {}
        if (tty_out) { fputc('\n', tty_out); fflush(tty_out); }
    }

    const char* result;
    if (got < 0) {
        result = dragon_string_alloc("", 0);
    } else {
        size_t n = (size_t)got;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        result = dragon_string_alloc(line, (int64_t)n);
    }

    if (line) {
        if (cap) memset(line, 0, cap);
        free(line);
    }
    if (use_tty && opened) fclose(opened);
    return result;
#endif
}

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

}
