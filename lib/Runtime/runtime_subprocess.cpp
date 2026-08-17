#include "runtime_internal.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

#ifndef _WIN32
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <poll.h>
  #include <time.h>
  #include <signal.h>
  #include <pthread.h>
  #include <sys/wait.h>
#endif

#ifndef _WIN32
static int make_pipe_cloexec(int fds[2]) {
#if defined(__linux__) || defined(__FreeBSD__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; ++i) {
        int fl = fcntl(fds[i], F_GETFD);
        if (fl == -1 || fcntl(fds[i], F_SETFD, fl | FD_CLOEXEC) == -1) {
            int e = errno; close(fds[0]); close(fds[1]); errno = e; return -1;
        }
    }
    return 0;
#endif
}
#endif

extern "C" {

void dragon_vthread_yield(void);

DragonList* dragon_subprocess_spawn(DragonList* argv, int cap_in, int cap_out,
                                    int cap_err, const char* cwd) {
    DragonList* result = dragon_list_new_tagged(4, TAG_INT);
#ifdef _WIN32
    (void)argv; (void)cap_in; (void)cap_out; (void)cap_err; (void)cwd;
    dragon_list_append(result, -1);
    dragon_list_append(result, -1);
    dragon_list_append(result, -1);
    dragon_list_append(result, -1);
    return result;
#else
    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};

    if (cap_in && make_pipe_cloexec(in_pipe) != 0) {
        dragon_list_append(result, -1); dragon_list_append(result, errno);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }
    if (cap_out && make_pipe_cloexec(out_pipe) != 0) {
        int e = errno;
        if (cap_in) { close(in_pipe[0]); close(in_pipe[1]); }
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }
    if (cap_err && make_pipe_cloexec(err_pipe) != 0) {
        int e = errno;
        if (cap_in)  { close(in_pipe[0]);  close(in_pipe[1]); }
        if (cap_out) { close(out_pipe[0]); close(out_pipe[1]); }
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }

    int exec_pipe[2] = {-1, -1};
    if (make_pipe_cloexec(exec_pipe) != 0) {
        int e = errno;
        if (cap_in)  { close(in_pipe[0]);  close(in_pipe[1]); }
        if (cap_out) { close(out_pipe[0]); close(out_pipe[1]); }
        if (cap_err) { close(err_pipe[0]); close(err_pipe[1]); }
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }

    int n = (int)dragon_list_len(argv);
    char** args = (char**)dragon_malloc_nullable((size_t)(n + 1) * sizeof(char*));
    char** owned = (char**)dragon_calloc_nullable((size_t)(n > 0 ? n : 1), sizeof(char*));
    if (!args || !owned) {
        int e = ENOMEM;
        free(args); free(owned);
        if (cap_in)  { close(in_pipe[0]);  close(in_pipe[1]); }
        if (cap_out) { close(out_pipe[0]); close(out_pipe[1]); }
        if (cap_err) { close(err_pipe[0]); close(err_pipe[1]); }
        close(exec_pipe[0]); close(exec_pipe[1]);
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }
    for (int i = 0; i < n; i++) {
        const char* raw = (const char*)(uintptr_t)dragon_list_load(argv, i);
        int64_t blen = 0;
        char* enc = dragon_str_to_utf8_alloc(raw, &blen);
        if (enc) { owned[i] = enc; args[i] = enc; }
        else     { args[i] = (char*)(uintptr_t)raw; }
    }
    args[n] = nullptr;

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        for (int i = 0; i < n; i++) free(owned[i]);
        free(owned);
        free(args);
        if (cap_in)  { close(in_pipe[0]);  close(in_pipe[1]); }
        if (cap_out) { close(out_pipe[0]); close(out_pipe[1]); }
        if (cap_err) { close(err_pipe[0]); close(err_pipe[1]); }
        close(exec_pipe[0]); close(exec_pipe[1]);
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }

    if (pid == 0) {
        if (cap_in) {
            dup2(in_pipe[0], 0);
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        if (cap_out) {
            dup2(out_pipe[1], 1);
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        if (cap_err) {
            dup2(err_pipe[1], 2);
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        if (cwd && cwd[0] != '\0') {
            if (chdir(cwd) != 0) {
                int ce = errno;
                ssize_t w = write(exec_pipe[1], &ce, sizeof(ce)); (void)w;
                _exit(127);
            }
        }
        execvp(args[0], args);
        int ee = errno;
        ssize_t w = write(exec_pipe[1], &ee, sizeof(ee)); (void)w;
        _exit(127);
    }

    for (int i = 0; i < n; i++) free(owned[i]);
    free(owned);
    free(args);
    int64_t stdin_w  = -1;
    int64_t stdout_r = -1;
    int64_t stderr_r = -1;
    if (cap_in)  { close(in_pipe[0]);  stdin_w  = in_pipe[1]; }
    if (cap_out) { close(out_pipe[1]); stdout_r = out_pipe[0]; }
    if (cap_err) { close(err_pipe[1]); stderr_r = err_pipe[0]; }

    close(exec_pipe[1]);
    int child_err = 0;
    ssize_t got = read(exec_pipe[0], &child_err, sizeof(child_err));
    close(exec_pipe[0]);
    if (got > 0) {
        int status; waitpid(pid, &status, 0);
        if (stdin_w  >= 0) close((int)stdin_w);
        if (stdout_r >= 0) close((int)stdout_r);
        if (stderr_r >= 0) close((int)stderr_r);
        dragon_list_append(result, -1); dragon_list_append(result, child_err);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }

    dragon_list_append(result, (int64_t)pid);
    dragon_list_append(result, stdin_w);
    dragon_list_append(result, stdout_r);
    dragon_list_append(result, stderr_r);
    return result;
#endif
}

DragonBytes* dragon_subprocess_drain(int fd) {
#ifdef _WIN32
    (void)fd;
    return dragon_bytes_new((const uint8_t*)"", 0);
#else
    if (fd < 0) return dragon_bytes_new((const uint8_t*)"", 0);
    size_t cap = 4096;
    size_t len = 0;
    uint8_t* buf = (uint8_t*)dragon_xmalloc(cap);
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t* nbuf = (uint8_t*)dragon_realloc_nullable(buf, ncap);
            if (!nbuf) { free(buf); dragon_raise_oom(); }
            buf = nbuf;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r > 0) {
            len += (size_t)r;
            continue;
        }
        if (r == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)len);
    free(buf);
    return out;
#endif
}

int64_t dragon_subprocess_write(int fd, DragonBytes* data) {
#ifdef _WIN32
    (void)fd; (void)data;
    return -1;
#else
    if (fd < 0) return -1;
    if (!data || data->len == 0) return 0;
    const uint8_t* p = data->data;
    size_t total = 0;
    size_t want = (size_t)data->len;
    while (total < want) {
        ssize_t w = write(fd, p + total, want - total);
        if (w > 0) { total += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return (int64_t)total;
#endif
}

void dragon_subprocess_close(int fd) {
#ifndef _WIN32
    if (fd >= 0) close(fd);
#else
    (void)fd;
#endif
}

#ifndef _WIN32
static void pump_set_nonblock(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int64_t pump_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct { bool had_blocked; } SigpipeGuard;

static SigpipeGuard pump_block_sigpipe(void) {
    SigpipeGuard g;
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    g.had_blocked = sigismember(&cur, SIGPIPE) == 1;
    if (!g.had_blocked) {
        sigset_t add;
        sigemptyset(&add);
        sigaddset(&add, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &add, NULL);
    }
    return g;
}

static void pump_drain_sigpipe(void) {
    sigset_t pend;
    sigpending(&pend);
    if (sigismember(&pend, SIGPIPE) == 1) {
        sigset_t only;
        sigemptyset(&only);
        sigaddset(&only, SIGPIPE);
#ifdef __APPLE__
        int consumed = 0;
        sigwait(&only, &consumed);
#else
        struct timespec zero = {0, 0};
        sigtimedwait(&only, NULL, &zero);
#endif
    }
}

static void pump_restore_sigpipe(SigpipeGuard g) {
    if (!g.had_blocked) {
        sigset_t del;
        sigemptyset(&del);
        sigaddset(&del, SIGPIPE);
        pthread_sigmask(SIG_UNBLOCK, &del, NULL);
    }
}

typedef struct {
    uint8_t* buf;
    size_t   len;
    size_t   cap;
} PumpBuf;

static bool pumpbuf_append(PumpBuf* b, const uint8_t* src, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < b->len + n) ncap *= 2;
        uint8_t* nbuf = (uint8_t*)dragon_realloc_nullable(b->buf, ncap);
        if (!nbuf) return false;
        b->buf = nbuf;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
    return true;
}
#endif

DragonList* dragon_subprocess_pump(int in_fd, DragonBytes* stdin_data,
                                   int out_fd, int err_fd, int64_t timeout_ms) {
    DragonListPtr* result = dragon_list_new_ptr(3, TAG_BYTES);
#ifdef _WIN32
    (void)in_fd; (void)stdin_data; (void)out_fd; (void)err_fd; (void)timeout_ms;
    dragon_list_append_ptr(result, (void*)dragon_bytes_new((const uint8_t*)"", 0));
    dragon_list_append_ptr(result, (void*)dragon_bytes_new((const uint8_t*)"", 0));
    uint8_t flag = 0;
    dragon_list_append_ptr(result, (void*)dragon_bytes_new(&flag, 1));
    return (DragonList*)result;
#else
    pump_set_nonblock(in_fd);
    pump_set_nonblock(out_fd);
    pump_set_nonblock(err_fd);

    PumpBuf outb = {nullptr, 0, 0};
    PumpBuf errb = {nullptr, 0, 0};

    const uint8_t* in_ptr = (stdin_data ? stdin_data->data : nullptr);
    size_t in_total = (stdin_data ? (size_t)stdin_data->len : 0);
    size_t in_done  = 0;
    bool stdin_open = (in_fd >= 0);
    if (stdin_open && in_total == 0) {
        close(in_fd);
        in_fd = -1;
        stdin_open = false;
    }
    bool out_eof = (out_fd < 0);
    bool err_eof = (err_fd < 0);

    const int SLICE_MS = 25;
    const int64_t deadline = (timeout_ms >= 0) ? pump_now_ms() + timeout_ms : -1;
    bool timed_out = false;

    uint8_t rbuf[65536];

    while (stdin_open || !out_eof || !err_eof) {
        struct pollfd pfds[3];
        int idx_in = -1, idx_out = -1, idx_err = -1;
        nfds_t nf = 0;
        if (stdin_open) {
            pfds[nf].fd = in_fd;  pfds[nf].events = POLLOUT; pfds[nf].revents = 0;
            idx_in = (int)nf; nf++;
        }
        if (!out_eof) {
            pfds[nf].fd = out_fd; pfds[nf].events = POLLIN;  pfds[nf].revents = 0;
            idx_out = (int)nf; nf++;
        }
        if (!err_eof) {
            pfds[nf].fd = err_fd; pfds[nf].events = POLLIN;  pfds[nf].revents = 0;
            idx_err = (int)nf; nf++;
        }
        if (nf == 0) break;

        int poll_to;
        if (deadline >= 0) {
            int64_t remain = deadline - pump_now_ms();
            if (remain <= 0) { timed_out = true; break; }
            poll_to = (remain < SLICE_MS) ? (int)remain : SLICE_MS;
        } else {
            poll_to = SLICE_MS;
        }

        int pr = poll(pfds, nf, poll_to);
        if (pr < 0) {
            if (errno == EINTR) { dragon_vthread_yield(); continue; }
            break;
        }
        if (pr == 0) {
            dragon_vthread_yield();
            continue;
        }

        if (idx_in >= 0 && (pfds[idx_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (pfds[idx_in].revents & (POLLERR | POLLHUP)) {
                close(in_fd); in_fd = -1; stdin_open = false;
            } else {
                SigpipeGuard sg = pump_block_sigpipe();
                for (;;) {
                    size_t want = in_total - in_done;
                    if (want == 0) break;
                    ssize_t w = write(in_fd, in_ptr + in_done, want);
                    if (w > 0) { in_done += (size_t)w; continue; }
                    if (w < 0 && errno == EINTR) continue;
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if (w < 0 && errno == EPIPE) pump_drain_sigpipe();
                    close(in_fd); in_fd = -1; stdin_open = false;
                    break;
                }
                pump_restore_sigpipe(sg);
                if (stdin_open && in_done == in_total) {
                    close(in_fd); in_fd = -1; stdin_open = false;
                }
            }
        }

        if (idx_out >= 0 && (pfds[idx_out].revents & (POLLIN | POLLERR | POLLHUP))) {
            for (;;) {
                ssize_t r = read(out_fd, rbuf, sizeof(rbuf));
                if (r > 0) { pumpbuf_append(&outb, rbuf, (size_t)r); continue; }
                if (r == 0) { out_eof = true; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                out_eof = true; break;
            }
        }

        if (idx_err >= 0 && (pfds[idx_err].revents & (POLLIN | POLLERR | POLLHUP))) {
            for (;;) {
                ssize_t r = read(err_fd, rbuf, sizeof(rbuf));
                if (r > 0) { pumpbuf_append(&errb, rbuf, (size_t)r); continue; }
                if (r == 0) { err_eof = true; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                err_eof = true; break;
            }
        }

        dragon_vthread_yield();
    }

    if (in_fd  >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);

    DragonBytes* out_bytes = dragon_bytes_new(outb.buf ? outb.buf : (const uint8_t*)"",
                                              (int64_t)outb.len);
    DragonBytes* err_bytes = dragon_bytes_new(errb.buf ? errb.buf : (const uint8_t*)"",
                                              (int64_t)errb.len);
    free(outb.buf);
    free(errb.buf);

    uint8_t flag = timed_out ? 1 : 0;
    DragonBytes* flag_bytes = dragon_bytes_new(&flag, 1);

    dragon_list_append_ptr(result, (void*)out_bytes);
    dragon_list_append_ptr(result, (void*)err_bytes);
    dragon_list_append_ptr(result, (void*)flag_bytes);
    return (DragonList*)result;
#endif
}


DragonBytes* dragon_subprocess_read_n(int fd, int64_t n) {
#ifdef _WIN32
    (void)fd; (void)n;
    return dragon_bytes_new((const uint8_t*)"", 0);
#else
    if (fd < 0 || n <= 0) return dragon_bytes_new((const uint8_t*)"", 0);
    uint8_t* buf = (uint8_t*)dragon_malloc_nullable((size_t)n);
    if (!buf) return dragon_bytes_new((const uint8_t*)"", 0);
    size_t got = 0;
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLIN;
    while (got < (size_t)n) {
        pf.revents = 0;
        int pr = poll(&pf, 1, 25);
        if (pr < 0) {
            if (errno == EINTR) { dragon_vthread_yield(); continue; }
            break;
        }
        if (pr == 0) { dragon_vthread_yield(); continue; }
        ssize_t r = read(fd, buf + got, (size_t)n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)got);
    free(buf);
    return out;
#endif
}

int64_t dragon_subprocess_write_all(int fd, DragonBytes* data) {
#ifdef _WIN32
    (void)fd; (void)data;
    return -1;
#else
    if (fd < 0 || !data) return -1;
    const uint8_t* p = data->data;
    size_t left = (size_t)data->len;
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLOUT;
    while (left > 0) {
        pf.revents = 0;
        int pr = poll(&pf, 1, 25);
        if (pr < 0) {
            if (errno == EINTR) { dragon_vthread_yield(); continue; }
            return -1;
        }
        if (pr == 0) { dragon_vthread_yield(); continue; }
        if (pf.revents & (POLLERR | POLLHUP)) return -1;
        SigpipeGuard sg = pump_block_sigpipe();
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EPIPE) pump_drain_sigpipe();
        pump_restore_sigpipe(sg);
        if (w > 0) { p += (size_t)w; left -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return 0;
#endif
}

}
