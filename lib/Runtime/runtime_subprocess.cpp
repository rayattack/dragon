/// Dragon Runtime - subprocess spawn with pipe capture (POSIX).
///
/// Its OWN translation unit, mirroring runtime_ed25519.cpp / runtime_crypto.cpp:
/// the spawn+pipe+dup2+exec dance is the one part of subprocess that CANNOT be
/// written in Dragon. Between fork() and execvp() a child may run only
/// async-signal-safe code - no Dragon allocator, no green-thread scheduler - so
/// the wiring of stdin/stdout/stderr pipes (pipe + dup2 + close-on-exec) and the
/// post-fork chdir must happen here in plain C. Everything user-visible
/// (CompletedProcess, Popen, run/check_output/call, CalledProcessError, status
/// decode) lives in stdlib/subprocess.dr.
///
/// argv is marshalled from a Dragon `list[str]` exactly like dragon_execvp in
/// runtime_platform.cpp: each element is a `const char*` into a heap
/// DragonString, NULL-terminated into a `char**` vector.
///
/// Pipe fds are ordinary file descriptors (NOT sockets), so the os/socket
/// nb_recv helpers (which call recv(2)) do not apply - Tier-0 drains them with
/// plain blocking read(2)/write(2)/close(2) defined here.

#include "runtime_internal.h"   // DragonList, DragonBytes, dragon_list_* , dragon_bytes_new
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
#endif

#ifndef _WIN32
// Create a pipe with both ends CLOEXEC. The runtime is multithreaded (scheduler
// workers + reactor), so a CONCURRENT dragon_subprocess_spawn on another carrier
// thread can fork between our pipe() and exec and inherit our pipe ends into an
// unrelated child - that child then holds our stdout write end open, so our
// drain never sees EOF and communicate() hangs past the intended child's
// death. O_CLOEXEC closes that race: the child of THIS spawn
// re-establishes the ends it needs via dup2 (which clears CLOEXEC on the copy),
// so the existing child-side wiring is unaffected. pipe2 in one syscall where
// available; fcntl fallback otherwise
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

// Forward decls for the green-thread cooperation seam (runtime_concurrency.cpp).
// dragon_vthread_yield() is a no-op off a green thread; on one it cooperatively
// reschedules so a poll-driven pump does not monopolize the carrier OS thread.
void dragon_vthread_yield(void);

//===-------------------------------------------------------------------===//
// dragon_subprocess_spawn - fork + pipe + dup2 + (chdir) + execvp.
//
// Returns a list[int] of length 4: [pid, stdin_w_fd, stdout_r_fd, stderr_r_fd].
//   * stdin_w_fd  is the PARENT's write end of the child's stdin  (-1 if not captured)
//   * stdout_r_fd is the PARENT's read  end of the child's stdout (-1 if not captured)
//   * stderr_r_fd is the PARENT's read  end of the child's stderr (-1 if not captured)
// On failure to fork the list is [-1, errno, -1, -1] and stdlib raises OSError.
//
// cwd: chdir to this directory in the child before exec, unless it is empty.
//===-------------------------------------------------------------------===//
DragonList* dragon_subprocess_spawn(DragonList* argv, int cap_in, int cap_out,
                                    int cap_err, const char* cwd) {
    DragonList* result = dragon_list_new_tagged(4, TAG_INT);
#ifdef _WIN32
    // Windows process spawn is deferred per D019 (CreateProcess + handle
    // inheritance is a different model). Report an error the .dr side raises.
    (void)argv; (void)cap_in; (void)cap_out; (void)cap_err; (void)cwd;
    dragon_list_append(result, -1);
    dragon_list_append(result, -1);
    dragon_list_append(result, -1);
    dragon_list_append(result, -1);
    return result;
#else
    // --- create the requested pipes in the PARENT first ---------------------
    // pipe[0] = read end, pipe[1] = write end.
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

    // --- marshal argv (same pattern as dragon_execvp) -----------------------
    // Done in the PARENT: the DragonString element pointers are valid here, and
    // execvp copies the strings, so the child only reads from `args`. We must
    // NOT call malloc between fork and exec (not async-signal-safe), so this
    // vector is built before forking and survives into the child's address
    // space via copy-on-write.
    int n = (int)dragon_list_len(argv);
    char** args = (char**)malloc((size_t)(n + 1) * sizeof(char*));
    // Parallel array of UTF-8 temporaries we own and must free. owned[i] is
    // non-NULL only when arg i needed encoding (a wide/UCS-4 string); a raw
    // (already-UTF-8) arg leaves owned[i] == NULL and args[i] borrows the
    // DragonString bytes directly. calloc'd so an early-exit free is safe.
    char** owned = (char**)calloc((size_t)(n > 0 ? n : 1), sizeof(char*));
    if (!args || !owned) {
        int e = ENOMEM;
        free(args); free(owned);
        if (cap_in)  { close(in_pipe[0]);  close(in_pipe[1]); }
        if (cap_out) { close(out_pipe[0]); close(out_pipe[1]); }
        if (cap_err) { close(err_pipe[0]); close(err_pipe[1]); }
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }
    // Encode each arg to UTF-8 in the PARENT (malloc is safe here; the child
    // reads the finished vector via copy-on-write). A DragonString element may
    // be UCS-4 storage - handing that raw pointer to execvp feeds the child
    // wide chars, so a non-ASCII arg like "café" arrives truncated at the
    // first embedded NUL. dragon_str_to_utf8_alloc returns NULL for a string
    // that is already UTF-8 bytes ("use the raw pointer") and a fresh buffer
    // otherwise.
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
        dragon_list_append(result, -1); dragon_list_append(result, e);
        dragon_list_append(result, -1); dragon_list_append(result, -1);
        return result;
    }

    if (pid == 0) {
        // ---- CHILD: async-signal-safe only past this point ----------------
        // Wire the child's std fds to the pipe ends, then exec. dup2 the
        // child-side ends onto 0/1/2 and close every pipe fd we still hold.
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
                _exit(127);
            }
        }
        execvp(args[0], args);
        // exec only returns on failure; 127 mirrors the shell "command not
        // found" convention CPython's subprocess also surfaces.
        _exit(127);
    }

    // ---- PARENT: close the child-side ends, keep our own ends --------------
    // Free the UTF-8 temporaries. Safe post-fork: the child has its own COW
    // copy, so releasing the parent's does not disturb the exec'd argv.
    for (int i = 0; i < n; i++) free(owned[i]);
    free(owned);
    free(args);
    int64_t stdin_w  = -1;
    int64_t stdout_r = -1;
    int64_t stderr_r = -1;
    if (cap_in)  { close(in_pipe[0]);  stdin_w  = in_pipe[1]; }
    if (cap_out) { close(out_pipe[1]); stdout_r = out_pipe[0]; }
    if (cap_err) { close(err_pipe[1]); stderr_r = err_pipe[0]; }

    dragon_list_append(result, (int64_t)pid);
    dragon_list_append(result, stdin_w);
    dragon_list_append(result, stdout_r);
    dragon_list_append(result, stderr_r);
    return result;
#endif
}

//===-------------------------------------------------------------------===//
// Blocking pipe I/O for the Tier-0 drain. Pipes are plain fds, not sockets, so
// these use read(2)/write(2)/close(2) directly. Distinct names from the
// socket-oriented dragon_nb_* helpers and from dragon_close_fd so there is no
// symbol collision.
//===-------------------------------------------------------------------===//

/// Blocking read of the WHOLE pipe until EOF (the child closed its write end).
/// Returns the accumulated bytes; empty bytes on error or immediate EOF. This
/// is the Tier-0 drain: it blocks the carrier thread (see knownRisks - the
/// green-thread non-blocking integration is a follow-up).
DragonBytes* dragon_subprocess_drain(int fd) {
#ifdef _WIN32
    (void)fd;
    return dragon_bytes_new((const uint8_t*)"", 0);
#else
    if (fd < 0) return dragon_bytes_new((const uint8_t*)"", 0);
    size_t cap = 4096;
    size_t len = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return dragon_bytes_new((const uint8_t*)"", 0);
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t* nbuf = (uint8_t*)realloc(buf, ncap);
            if (!nbuf) { free(buf); return dragon_bytes_new((const uint8_t*)"", 0); }
            buf = nbuf;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r > 0) {
            len += (size_t)r;
            continue;
        }
        if (r == 0) break;                       // EOF
        if (errno == EINTR) continue;            // interrupted - retry
        break;                                   // real error: return what we have
    }
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)len);
    free(buf);
    return out;
#endif
}

/// Blocking write of an entire DragonBytes buffer to a pipe fd. Loops over
/// short writes. Returns bytes written, or -1 on error.
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

/// Close a pipe fd held by the parent. Plain close(2); -1 is a no-op.
void dragon_subprocess_close(int fd) {
#ifndef _WIN32
    if (fd >= 0) close(fd);
#else
    (void)fd;
#endif
}

//===-------------------------------------------------------------------===//
// dragon_subprocess_pump - the Tier-1 concurrent drain.
//
// Replaces communicate()'s serial "write-all-stdin -> drain-stdout-fully ->
// drain-stderr-fully" loop, which DEADLOCKS the moment the child fills a pipe
// the parent is not currently servicing (e.g. the child blocks writing stderr
// while we block reading stdout, or needs more stdin than one pipe buffer
// holds). This pump multiplexes all three captured fds with poll(2): it feeds
// stdin while simultaneously draining stdout AND stderr, so no party can wedge
// on a full/empty pipe. It does NOT waitpid - once both read ends hit EOF the
// child has finished writing and the .dr side's existing wait() reaps it.
//
//   in_fd       parent's write end of child stdin  (-1 if not captured)
//   stdin_data  bytes to feed to stdin             (may be empty/NULL)
//   out_fd      parent's read end of child stdout  (-1 if not captured)
//   err_fd      parent's read end of child stderr  (-1 if not captured)
//   timeout_ms  wall-clock budget; < 0 means no timeout
//
// Returns a list[bytes] of length 3: [stdout, stderr, flag]. `flag` is a
// single byte: 0x00 = the child closed both pipes within the budget, 0x01 =
// the budget elapsed first (the .dr side then kills + reaps and raises
// TimeoutExpired carrying the partial stdout/stderr captured so far).
//
// All captured fds are CLOSED here on the way out (success or timeout); the
// .dr side clears its fd fields so its destructor/close path is a no-op. The
// one exception is stdin: it is closed as soon as it is fully written so the
// child sees EOF and can exit.
//
// Scheduler cooperation: a single blocking poll() with the full timeout would
// stall every other green thread sharing this carrier OS thread. Instead the
// poll() timeout is clamped to a short slice (SLICE_MS) and we
// dragon_vthread_yield() after each slice; on a green thread that lets
// co-scheduled vthreads make progress, and off one (no vthread) the yield is a
// no-op so the slice loop is just a bounded wait. This is NOT a true reactor
// park (the pump is not suspended on the epoll/kqueue fd set while idle) - see
// the .dr knownRisks note on the dedicated multi-fd+timeout reactor seam.
//===-------------------------------------------------------------------===//
#ifndef _WIN32
static void pump_set_nonblock(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Monotonic milliseconds for deadline math (immune to wall-clock jumps).
static int64_t pump_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Block SIGPIPE on THIS thread for the lifetime of the pump. Writing to a pipe
// whose read end the child has closed (e.g. `head` exiting early) raises
// SIGPIPE, whose default disposition kills the whole process. The runtime
// deliberately does NOT set a process-wide SIG_IGN (it would be inherited
// across fork/exec and break child pipelines that rely on SIGPIPE), so we mask
// it thread-locally instead: after a write returns EPIPE we drain the pending
// signal so it cannot fire when the mask is later lifted. `had_blocked` records
// whether SIGPIPE was already masked so we restore the exact prior state.
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

// Consume a SIGPIPE that is pending on this thread (after an EPIPE write), so
// unblocking it later does not deliver a stale signal. Zero-timeout wait.
static void pump_drain_sigpipe(void) {
    sigset_t pend;
    sigpending(&pend);
    if (sigismember(&pend, SIGPIPE) == 1) {
        sigset_t only;
        sigemptyset(&only);
        sigaddset(&only, SIGPIPE);
#ifdef __APPLE__
        // macOS has no sigtimedwait. sigwait would block on an empty pending
        // set, but the sigpending check above guarantees SIGPIPE is pending,
        // so this consumes it and returns immediately
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

// Grow-on-demand byte accumulator for one read pipe.
typedef struct {
    uint8_t* buf;
    size_t   len;
    size_t   cap;
} PumpBuf;

// Append `n` bytes from `src`, doubling capacity as needed. Returns false on
// OOM (the caller then stops growing but keeps what it has).
static bool pumpbuf_append(PumpBuf* b, const uint8_t* src, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < b->len + n) ncap *= 2;
        uint8_t* nbuf = (uint8_t*)realloc(b->buf, ncap);
        if (!nbuf) return false;
        b->buf = nbuf;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
    return true;
}
#endif  // !_WIN32

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
    // Nothing to write: signal EOF to the child immediately so it can finish.
    if (stdin_open && in_total == 0) {
        close(in_fd);
        in_fd = -1;
        stdin_open = false;
    }
    bool out_eof = (out_fd < 0);
    bool err_eof = (err_fd < 0);

    // On a green thread, slice the wait so co-scheduled vthreads run; off one,
    // the SLICE_MS clamp still bounds each poll() but we simply re-arm.
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

        // Compute this slice's poll timeout from the remaining deadline.
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
            break;  // unrecoverable poll error: return what we have
        }
        if (pr == 0) {
            // Slice elapsed with nothing ready - let other green threads run,
            // then either re-arm (no deadline) or check the deadline above.
            dragon_vthread_yield();
            continue;
        }

        // --- stdin: write whatever the pipe will accept right now ----------
        if (idx_in >= 0 && (pfds[idx_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (pfds[idx_in].revents & (POLLERR | POLLHUP)) {
                // Child closed/aborted its stdin read end (e.g. `head`): stop
                // feeding and close our end so we don't take SIGPIPE later.
                close(in_fd); in_fd = -1; stdin_open = false;
            } else {
                // SIGPIPE is masked ONLY across this non-yielding write burst:
                // the M:N scheduler may migrate this vthread to another carrier
                // OS thread at a yield, so the pthread-local mask must never be
                // held across dragon_vthread_yield().
                SigpipeGuard sg = pump_block_sigpipe();
                for (;;) {
                    size_t want = in_total - in_done;
                    if (want == 0) break;
                    ssize_t w = write(in_fd, in_ptr + in_done, want);
                    if (w > 0) { in_done += (size_t)w; continue; }
                    if (w < 0 && errno == EINTR) continue;
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    // EPIPE or other: child stopped reading. Drain the pending
                    // (masked) SIGPIPE, stop writing, close our end.
                    if (w < 0 && errno == EPIPE) pump_drain_sigpipe();
                    close(in_fd); in_fd = -1; stdin_open = false;
                    break;
                }
                pump_restore_sigpipe(sg);
                if (stdin_open && in_done == in_total) {
                    // Fully fed: close to deliver EOF so the child can exit.
                    close(in_fd); in_fd = -1; stdin_open = false;
                }
            }
        }

        // --- stdout: drain until EAGAIN or EOF -----------------------------
        if (idx_out >= 0 && (pfds[idx_out].revents & (POLLIN | POLLERR | POLLHUP))) {
            for (;;) {
                ssize_t r = read(out_fd, rbuf, sizeof(rbuf));
                if (r > 0) { pumpbuf_append(&outb, rbuf, (size_t)r); continue; }
                if (r == 0) { out_eof = true; break; }           // EOF
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                out_eof = true; break;                            // real error
            }
        }

        // --- stderr: drain until EAGAIN or EOF -----------------------------
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

        // Cooperative reschedule point: a busy child keeps us in this loop, so
        // yield after servicing a ready slice so peer green threads progress.
        dragon_vthread_yield();
    }

    // Close every fd we still hold (read ends always; stdin if not yet closed).
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

}  // extern "C"
