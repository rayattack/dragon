# Processes and the OS

Eventually a program needs to leave its own address space. It shells out
to `git`, pipes data through `gzip`, asks who is logged in, decides what
to do based on whether it is running on Linux or macOS, or simply reads
the arguments it was launched with and exits with a status the calling
shell can test. This chapter covers the modules that handle those jobs -
the boundary between your Dragon process and the operating system around
it:

- **`subprocess`** - spawn child processes and capture their output.
- **`signal`** - POSIX signal numbers and delivery helpers.
- **`getpass`** - read a password without echoing it; look up the
  current user.
- **`platform`** - identify the OS, machine, and kernel release.
- **`sys`** - the command line, process exit, and a few system constants.

These are POSIX-first wrappers. The dangerous primitives - the
`fork`/`dup2`/`execvp` dance, the terminal-attribute save-and-restore for
hidden input - live in C, where they have to, but everything you call
from Dragon is typed and thin: it costs what the underlying syscall
costs. Where the Python names and shapes survive that constraint, they
are kept; where Dragon's typed model changes them, this chapter says so.

> Environment variables (`os.environ`, `getenv`, `setenv`) live with the
> filesystem in [Files and the Filesystem](/docs/1402-stdlib-io) -
> they are not duplicated here.

## subprocess - running other programs

`subprocess` spawns a child process, optionally captures its standard
streams, and waits for it to finish. The whole module is **`shell=False`
semantics only**: `args` is always a `list[str]` argv vector, and there
is no shell-string parsing. To get shell features, you run the shell
explicitly and hand it the string:

```dragon
import subprocess

# Runs /bin/echo directly - no shell, no word-splitting, no globbing.
subprocess.run(["echo", "hello"])

# To get shell behaviour, invoke the shell yourself:
subprocess.run(["/bin/sh", "-c", "echo $HOME"])
```

### run - the high-level front end

`run(args, capture_output=False, check=False, input=b"", cwd="",
timeout=-1)` runs a command to completion and returns a
`CompletedProcess`. Note the positional order: `capture_output` comes
first, then `check`, then the `input` bytes.

```dragon
import subprocess

const cp: subprocess.CompletedProcess = subprocess.run(["echo", "hello"], true)
print(cp.returncode)            # 0
print(cp.stdout)                # b'hello\n'
print(cp.stdout.decode("utf-8")) # hello
```

`CompletedProcess` carries the `args` it ran, the `returncode`, and the
captured `stdout` / `stderr` as `bytes`. Captured output is only
populated when you pass `capture_output=True`; otherwise the child's
streams go straight to your terminal and `cp.stdout` is empty.

The `returncode` follows the Python convention: a normal exit yields its
code (0-255), and a process killed by signal *N* yields `-N`.

To feed a child its standard input, pass `input` as `bytes`. A
captured-stdin pipe is opened automatically when `input` is non-empty:

```dragon
import subprocess

# Pipe bytes into `cat`, which echoes them back on stdout.
const cp: subprocess.CompletedProcess = subprocess.run(["cat"], true, false, b"piped in\n")
print(cp.stdout.decode("utf-8").strip())  # piped in
```

The `cwd` argument runs the child in a different working directory
without changing your own:

```dragon
from subprocess import check_output

const out: bytes = check_output(["pwd"], "/tmp")
print(out.decode("utf-8").strip())  # /tmp
```

### check - failing loudly

By default `run` returns even when the child exits non-zero; you inspect
`returncode` yourself. Pass `check=True` (the second positional flag) to
turn a non-zero exit into a raised `CalledProcessError`:

```dragon
from subprocess import run, CalledProcessError

try {
    run(["false"], false, true)   # capture_output=false, check=true
} except CalledProcessError as e {
    print(f"caught: returncode={e.returncode}")  # caught: returncode=1
}
```

`CalledProcessError` carries `returncode`, `cmd` (the argv list), and
`output` (whatever stdout was captured). It descends from
`SubprocessError`, so a single `except SubprocessError` catches both it
and `TimeoutExpired`.

### check_output and call - the shorthands

When you only want one thing, two thin wrappers say so directly:

```dragon
from subprocess import check_output, call

# check_output: capture stdout, raise CalledProcessError on failure.
const out: bytes = check_output(["echo", "from check_output"])
print(out.decode("utf-8").strip())  # from check_output

# call: run it, return only the exit code.
print(call(["true"]))   # 0
print(call(["false"]))  # 1
```

`check_output(args, cwd="")` returns the child's stdout as `bytes` and
raises on a non-zero exit. `call(args, cwd="")` ignores output and
returns the integer return code.

### Popen - the low-level handle

`run` is built on `Popen`, which spawns the child at construction time
and hands you a handle you drive yourself. Its constructor is
`Popen(args, cwd="", capture_stdout=true, capture_stderr=true,
capture_stdin=false)`.

```dragon
from subprocess import Popen

const p: Popen = Popen(["echo", "popen line"])
const both: tuple[bytes, bytes] = p.communicate()
print(both[0].decode("utf-8").strip())  # popen line
print(p.returncode)                     # 0
```

`communicate(input=b"", timeout=-1)` is the safe way to talk to a child:
it feeds `input` to stdin while concurrently draining stdout and stderr
through a single `poll(2)` multiplex, so a child that fills one pipe
while you service another can never deadlock. It returns a
`tuple[bytes, bytes]` of `(stdout, stderr)`.

The other `Popen` methods mirror CPython:

- `wait() -> int` - block until the child exits, return the return code.
- `poll() -> int` - non-blocking check; returns the return code if the
  child has exited, or **`-1`** if it is still running (CPython returns
  `None`; Dragon uses `-1`).
- `send_signal(sig)` - deliver an arbitrary signal via `kill(2)`.
- `terminate()` - send `SIGTERM`.
- `kill()` - send `SIGKILL`.

### timeout

Both `run` and `communicate` take a `timeout` in **milliseconds**
(`-1`, the default, means wait forever). On expiry the child is killed
and reaped, and `TimeoutExpired` is raised carrying the partial `output`
and `stderr` captured before the deadline:

```dragon
from subprocess import run, TimeoutExpired

try {
    # Give `sleep 5` only 100 ms to finish.
    run(["sleep", "5"], false, false, b"", "", 100)
} except TimeoutExpired as e {
    print(f"timed out after {e.timeout} ms")  # timed out after 100 ms
}
```

> **Differs from Python.** `shell=` is not a parameter - there is no
> shell mode; run `/bin/sh -c` yourself. `timeout` is an integer count
> of **milliseconds**, not float seconds. `poll()` returns `-1` for a
> still-running child, not `None`. Streams are always `bytes`; there is
> no `text=`/`encoding=` mode, so call `.decode(...)` yourself. POSIX
> only - Windows is deferred.

## signal - signal numbers and delivery

`signal` is a pure FFI layer over libc. It gives you the POSIX signal
*numbers* as constants and a handful of helpers for *sending* signals -
but it does **not** let you install handlers.

The constants are the standard Linux/glibc values: `SIGHUP` (1),
`SIGINT` (2), `SIGQUIT` (3), `SIGKILL` (9), `SIGSEGV` (11),
`SIGPIPE` (13), `SIGALRM` (14), `SIGTERM` (15), `SIGCHLD` (17), and the
rest, plus `SIG_DFL` (0) and `SIG_IGN` (1).

```dragon
import signal

print(signal.SIGINT)                          # 2
print(signal.SIGTERM)                          # 15
print(signal.strsignal_of(signal.SIGTERM))     # Terminated
print(signal.strsignal_of(signal.SIGINT))      # Interrupt
```

The delivery helpers:

- `raise_signal(sig) -> int` - deliver `sig` to the current process
  (`raise(3)`); returns 0 on success.
- `kill_pid(pid, sig) -> int` - `kill(2)`, deliver `sig` to another
  process; returns 0 on success.
- `alarm_secs(seconds) -> int` - `alarm(2)`, schedule a `SIGALRM`;
  returns the previous alarm's remaining seconds.
- `wait_for_signal() -> int` - `pause(2)`, sleep until any signal
  arrives; always returns `-1`.
- `strsignal_of(sig) -> str` - the human-readable text for a signal.

> **Differs from Python.** There is no `signal.signal(sig, handler)`.
> Installing a handler means passing a C function pointer to `signal(3)`,
> and Dragon closures do not lower to a stable C ABI at this layer, so
> handler installation is intentionally not modelled. You can name,
> send, and describe signals; you cannot catch them in Dragon code. The
> sending helper is named `kill_pid`, not `os.kill`.

## getpass - passwords and the current user

`getpass` reads a line with the terminal echo turned off, so a typed
password never appears on screen, and looks up the name of the user
running the program.

```dragon
from getpass import getpass, getuser

# pw: str = getpass()           # prompts "Password: ", echo disabled
# pw2: str = getpass("PIN: ")   # custom prompt

const who: str = getuser()
print(len(who) > 0)             # True
```

`getpass(prompt="Password: ") -> str` writes the prompt to the
controlling terminal (so it stays visible even with stdout redirected),
disables echo, reads one line, and restores the terminal state on every
exit path - including EOF, interruption, or error - because that
save-and-restore dance lives in the native shim. The two calls to
`getpass` above are shown commented out: they block waiting for terminal
input, so they are not something a doc example should run, but the API is
exactly as written.

When standard input is *not* a terminal (a pipe, a redirect), echo cannot
be suppressed. Matching CPython, `getpass` then emits a `GetPassWarning`
and falls back to a plain, echoing read, still returning whatever was
typed.

`getuser() -> str` returns the current login name. It checks the
environment variables `LOGNAME`, `USER`, `LNAME`, and `USERNAME` in that
order, and falls back to the system password database (`getpwuid`) when
none are set - the same resolution order CPython uses.

> **Differs from Python.** `GetPassWarning` is a string category name
> (`"GetPassWarning"`), not an exception class, because Dragon's
> `warnings` module categorises by name. POSIX termios backs the
> no-echo read; Windows is deferred.

## platform - identifying the machine

`platform` is a small set of accessors over `uname(2)`. Each returns a
plain `str` (or, for `uname()`, a 5-tuple of them).

```dragon
import platform

print(platform.system())     # Linux
print(platform.machine())    # x86_64
print(platform.platform())   # Linux-6.17.0-29-generic-x86_64
```

The full surface:

- `system() -> str` - OS name, e.g. `"Linux"` or `"Darwin"`.
- `node() -> str` - the network node name (hostname).
- `release() -> str` - the OS/kernel release, e.g. `"6.17.0-29-generic"`.
- `version() -> str` - the OS version string.
- `machine() -> str` - the machine type, e.g. `"x86_64"`.
- `processor() -> str` - the processor name (best effort: the machine
  type).
- `platform() -> str` - a single composite string,
  `system()-release()-machine()`.
- `uname() -> tuple[str, str, str, str, str]` - all five as
  `(system, node, release, version, machine)`.

```dragon
from platform import system, machine, uname

print(system())   # Linux
print(machine())  # x86_64

const u: tuple[str, str, str, str, str] = uname()
print(u[0])       # Linux
```

> **Differs from Python.** The CPython-specific helpers
> (`python_version`, `python_implementation`, and friends) are absent -
> there is no CPython here. `uname()` is a plain 5-tuple, not a named
> tuple, so index it positionally. `processor()` returns the machine
> type rather than a distinct CPU string.

## sys - the command line and process exit

`sys` exposes the program's arguments, a way to exit with a status, and
a few constants describing the running system.

The command line comes from `argv()` - a **function**, not an attribute.
It returns `list[str]` where element 0 is the executable and the rest are
the user's arguments:

```dragon
import sys

const args: list[str] = sys.argv()
print(args[0])          # the running binary's path
print(len(args))        # 1 when no extra args were passed
```

To end the program with a specific status, call `exit_code(code)`. The
shell sees that integer as the process exit status:

```dragon
import sys

print("before exit")
sys.exit_code(3)        # process exits here with status 3
print("never printed")
```

`abort_now()` is the harder stop - it calls `abort(3)`, terminating via
`SIGABRT` without running cleanup.

The constants and small helpers:

- `maxsize: int` - the largest `int`, `9223372036854775807`.
- `platform: str` - `"linux"`.
- `byteorder: str` - `"little"`.
- `version: str` - the Dragon runtime version, e.g. `"0.1.0"`.
- `getdefaultencoding() -> str` - `"utf-8"`.
- `getrecursionlimit() -> int` - `1000`.

```dragon
import sys

print(sys.platform)     # linux
print(sys.maxsize)      # 9223372036854775807
print(sys.byteorder)    # little
print(sys.version)      # 0.1.0
```

> **Differs from Python.** `argv` and `exit` are **functions** here:
> `sys.argv()` (not the list attribute `sys.argv`) and `sys.exit_code(n)`
> (not `sys.exit(n)`). There are no `sys.stdin`/`sys.stdout`/`sys.stderr`
> stream objects - use `print` and `input`, or the `io` module, for I/O.
> `sys.platform` is the constant string `"linux"`; for the *actual*
> running OS, use `platform.system()`.

## At a glance

| You want to… | Reach for | Signature |
|---|---|---|
| Run a command, capture output | `subprocess.run` | `run(args, capture_output=False, check=False, input=b"", cwd="", timeout=-1) -> CompletedProcess` |
| Run a command, get stdout, fail on error | `subprocess.check_output` | `check_output(args, cwd="") -> bytes` |
| Run a command, get just the exit code | `subprocess.call` | `call(args, cwd="") -> int` |
| Drive a child process by hand | `subprocess.Popen` | `Popen(args, cwd="", capture_stdout=true, capture_stderr=true, capture_stdin=false)` |
| Feed stdin / drain stdout safely | `Popen.communicate` | `communicate(input=b"", timeout=-1) -> tuple[bytes, bytes]` |
| Name a signal | `signal.SIG*` | constants (`SIGTERM == 15`, …) |
| Send a signal to a process | `signal.kill_pid` | `kill_pid(pid, sig) -> int` |
| Describe a signal | `signal.strsignal_of` | `strsignal_of(sig) -> str` |
| Read a password (no echo) | `getpass.getpass` | `getpass(prompt="Password: ") -> str` |
| Current login name | `getpass.getuser` | `getuser() -> str` |
| OS / machine identity | `platform.system`, `platform.machine` | `system() -> str`, `machine() -> str` |
| The command-line arguments | `sys.argv` | `argv() -> list[str]` |
| Exit with a status code | `sys.exit_code` | `exit_code(code)` |

With the OS boundary covered from the Dragon side, the next part crosses
it in the other direction - calling into native libraries from your own
code: [FFI: Calling C and C++](/docs/1501-ffi).
