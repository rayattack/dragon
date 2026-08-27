#ifndef _WIN32
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
#endif

#include "runtime_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
  #include <errno.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  #include <sys/ioctl.h>
  #include <termios.h>
  #include <unistd.h>
#endif

extern "C" {

#ifdef _WIN32

int32_t dragon_term_raw_enable(void) { return -1; }
int32_t dragon_term_raw_disable(void) { return -1; }
int32_t dragon_term_is_raw(void) { return 0; }
int32_t dragon_term_read_byte(int32_t) { return -3; }
int32_t dragon_term_write(const char*) { return -1; }
int32_t dragon_term_flush(void) { return -1; }
int32_t dragon_term_width(void) { return 80; }
int32_t dragon_term_height(void) { return 24; }
int32_t dragon_term_kitty_push(int32_t) { return -1; }
int32_t dragon_term_kitty_pop(void) { return -1; }
int32_t dragon_term_isatty_stdin(void) { return 0; }

#else

static struct termios g_saved;
static int  g_tty_fd = -1;
static int  g_tty_owned = 0;
static int  g_atexit_registered = 0;
static int  g_handlers_installed = 0;
static volatile sig_atomic_t g_raw = 0;
static volatile sig_atomic_t g_kitty = 0;
static volatile sig_atomic_t g_resized = 0;

static unsigned char g_buf[256];
static int g_buf_len = 0;
static int g_buf_pos = 0;

static int term_out_fd(void) {
    return g_tty_fd >= 0 ? g_tty_fd : STDERR_FILENO;
}

static void term_restore(void) {
    const int fd = term_out_fd();

    if (g_kitty) {
        g_kitty = 0;
        (void)!write(fd, "\x1b[<u", 4);
    }
    (void)!write(fd, "\x1b[?25h", 6);

    if (g_raw) {
        g_raw = 0;
        while (tcsetattr(fd, TCSAFLUSH, &g_saved) != 0 && errno == EINTR) {}
    }
}

static void term_fatal_handler(int signo) {
    term_restore();
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(signo, &dfl, nullptr);
    raise(signo);
}

static void term_winch_handler(int) {
    g_resized = 1;
}

static void term_install_handlers(void) {
    if (g_handlers_installed) return;
    g_handlers_installed = 1;

    static const int fatal[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = term_fatal_handler;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i)
        sigaction(fatal[i], &sa, nullptr);

    struct sigaction winch;
    memset(&winch, 0, sizeof(winch));
    winch.sa_handler = term_winch_handler;
    sigemptyset(&winch.sa_mask);
    sigaction(SIGWINCH, &winch, nullptr);
}

static void term_open_tty(void) {
    if (g_tty_fd >= 0) return;
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        g_tty_fd = fd;
        g_tty_owned = 1;
        return;
    }
    g_tty_fd = STDIN_FILENO;
    g_tty_owned = 0;
}

int32_t dragon_term_isatty_stdin(void) {
    return isatty(STDIN_FILENO) ? 1 : 0;
}

int32_t dragon_term_raw_enable(void) {
    if (g_raw) return 0;
    term_open_tty();

    const int fd = g_tty_fd;
    if (tcgetattr(fd, &g_saved) != 0) return -1;

    struct termios raw = g_saved;
    cfmakeraw(&raw);
    raw.c_oflag |= (tcflag_t)(OPOST | ONLCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSAFLUSH, &raw) != 0) return -1;

    g_raw = 1;
    g_buf_len = 0;
    g_buf_pos = 0;

    if (!g_atexit_registered) {
        g_atexit_registered = 1;
        atexit(term_restore);
    }
    term_install_handlers();
    return 0;
}

int32_t dragon_term_raw_disable(void) {
    if (!g_raw && !g_kitty) return 0;
    term_restore();
    g_buf_len = 0;
    g_buf_pos = 0;
    return 0;
}

int32_t dragon_term_is_raw(void) {
    return g_raw ? 1 : 0;
}

int32_t dragon_term_read_byte(int32_t timeout_ms) {
    if (g_buf_pos < g_buf_len) return (int32_t)g_buf[g_buf_pos++];

    const int fd = g_tty_fd >= 0 ? g_tty_fd : STDIN_FILENO;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready == 0) return -1;
    if (ready < 0) {
        if (errno == EINTR) {
            if (g_resized) { g_resized = 0; return -4; }
            return -1;
        }
        return -3;
    }

    const ssize_t got = read(fd, g_buf, sizeof(g_buf));
    if (got == 0) return -2;
    if (got < 0) {
        if (errno == EINTR) {
            if (g_resized) { g_resized = 0; return -4; }
            return -1;
        }
        return -3;
    }

    g_buf_len = (int)got;
    g_buf_pos = 1;
    return (int32_t)g_buf[0];
}

int32_t dragon_term_write(const char* s) {
    if (!s) return 0;
    int64_t len = 0;
    char* encoded = dragon_str_to_utf8_alloc(s, &len);
    const char* bytes = encoded ? encoded : s;

    const int fd = term_out_fd();
    int64_t written = 0;
    int32_t result = 0;
    while (written < len) {
        const ssize_t n = write(fd, bytes + written, (size_t)(len - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        written += n;
    }
    if (encoded) std::free(encoded);
    return result < 0 ? result : (int32_t)written;
}

int32_t dragon_term_flush(void) {
    return 0;
}

int32_t dragon_term_width(void) {
    struct winsize ws;
    if (ioctl(term_out_fd(), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int32_t)ws.ws_col;
    return 80;
}

int32_t dragon_term_height(void) {
    struct winsize ws;
    if (ioctl(term_out_fd(), TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return (int32_t)ws.ws_row;
    return 24;
}

int32_t dragon_term_kitty_push(int32_t flags) {
    if (g_kitty) return 0;
    char seq[32];
    const int n = snprintf(seq, sizeof(seq), "\x1b[>%du", (int)flags);
    if (n <= 0) return -1;
    if (write(term_out_fd(), seq, (size_t)n) != n) return -1;
    g_kitty = 1;
    return 0;
}

int32_t dragon_term_kitty_pop(void) {
    if (!g_kitty) return 0;
    g_kitty = 0;
    return write(term_out_fd(), "\x1b[<u", 4) == 4 ? 0 : -1;
}

#endif

}
