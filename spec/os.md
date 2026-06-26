# Dragon `os` Module - Python Parity Spec

**Module**: `stdlib/os.dr`
**Python reference**: https://docs.python.org/3/library/os.html
**Overall progress**: **80 / 80** functions (100%)

> Phase 0 = Core (most-used, needed for self-hosting) - **COMPLETE**
> Phase 1 = Common (frequently used in real programs) - **COMPLETE**
> Phase 2 = Extended (full parity, niche/advanced) - **COMPLETE**

---

## File System Operations

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 1 | `os.getcwd()` | `cwd()` | 0 | Done |
| 2 | `os.chdir(path)` | `chdir_to(path: str)` | 0 | Done |
| 3 | `os.listdir(path)` | `listdir(path: str) -> list[str]` | 0 | Done |
| 4 | `os.mkdir(path, mode)` | `makedirs(path: str)` | 0 | Done |
| 5 | `os.makedirs(name, exist_ok)` | `makedirs(path: str)` - recursive via `dragon_makedirs` | 0 | Done |
| 6 | `os.rmdir(path)` | `removedirs(path: str)` | 0 | Done |
| 7 | `os.removedirs(name)` | `removedirs(path: str)` - recursive parent prune | 2 | Done |
| 8 | `os.remove(path)` / `os.unlink(path)` | `remove(path: str)` | 0 | Done |
| 9 | `os.rename(src, dst)` | `rename_path(src: str, dst: str)` | 0 | Done |
| 10 | `os.replace(src, dst)` | `replace(src: str, dst: str)` | 1 | Done |
| 11 | `os.link(src, dst)` | `link_to(src: str, dst: str)` | 1 | Done |
| 12 | `os.symlink(src, dst)` | `symlink_to(target: str, linkpath: str)` | 1 | Done |
| 13 | `os.readlink(path)` | `readlink_path(path: str) -> str` | 1 | Done |
| 14 | `os.truncate(path, length)` | `truncate_file(path: str, length: int)` | 1 | Done |
| 15 | `os.scandir(path)` | `scandir(path: str) -> list[DirEntry]` | 2 | Done |
| 16 | `os.walk(top)` | `walk_files(top: str) -> list[str]`, `walk_dirs(top: str) -> list[str]` | 1 | Done |
| 17 | `os.renames(old, new)` | `renames(src: str, dst: str)` | 2 | Done |
| 18 | `os.chroot(path)` | `chroot_to(path: str)` | 2 | Done |

## File Properties & Stat

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 19 | `os.stat(path)` | `getsize()`, `getmtime()`, `getatime()`, `getctime()`, `getmode()`, `isfile()`, `isdir()` | 0 | Done |
| 20 | `os.lstat(path)` | `lstat_size`, `lstat_mtime`, `lstat_atime`, `lstat_ctime`, `lstat_mode`, `lstat_isfile`, `lstat_isdir` | 2 | Done |
| 21 | `os.access(path, mode)` | `exists()`, `can_read()`, `can_write()`, `can_exec()` | 0 | Done |
| 22 | `os.chmod(path, mode)` | `chmod_path(path: str, mode: int)` | 1 | Done |
| 23 | `os.chown(path, uid, gid)` | `chown_path(path: str, uid: int, gid: int)` | 2 | Done |
| 24 | `os.F_OK` | `const F_OK: int = 0` | 1 | Done |
| 25 | `os.R_OK` | `const R_OK: int = 4` | 1 | Done |
| 26 | `os.W_OK` | `const W_OK: int = 2` | 1 | Done |
| 27 | `os.X_OK` | `const X_OK: int = 1` | 1 | Done |
| 28 | `os.path.getsize(path)` | `getsize(path: str) -> int` | 0 | Done |
| 29 | `os.path.getmtime(path)` | `getmtime(path: str) -> int` | 0 | Done |
| 30 | `os.path.isfile(path)` | `isfile(path: str) -> bool` | 0 | Done |
| 31 | `os.path.isdir(path)` | `isdir(path: str) -> bool` | 0 | Done |
| 32 | `os.path.exists(path)` | `exists(path: str) -> bool` | 0 | Done |
| 33 | `os.path.islink(path)` | `islink(path: str) -> bool` | 1 | Done |
| 34 | `os.path.ismount(path)` | `ismount(path: str) -> bool` | 2 | Done |
| 35 | `os.path.samefile(p1, p2)` | `samefile(p1: str, p2: str) -> bool` | 2 | Done |

## Path Utilities (os.path)

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 36 | `os.path.join(a, b, ...)` | `path_join(a: str, b: str) -> str` | 0 | Done |
| 37 | `os.path.basename(path)` | `path_basename(path: str) -> str` | 0 | Done |
| 38 | `os.path.dirname(path)` | `path_dirname(path: str) -> str` | 0 | Done |
| 39 | `os.path.split(path)` | `path_split(path: str) -> list[str]` | 1 | Done |
| 40 | `os.path.splitext(path)` | `path_splitext(path: str) -> list[str]` | 0 | Done |
| 41 | `os.path.abspath(path)` | `abspath(path: str) -> str` | 0 | Done |
| 42 | `os.path.realpath(path)` | `abspath()` wraps `realpath()` | 0 | Done |
| 43 | `os.path.isabs(path)` | `path_isabs(path: str) -> bool` | 0 | Done |
| 44 | `os.path.expanduser(path)` | `path_expanduser(path: str) -> str` | 1 | Done |
| 45 | `os.path.expandvars(path)` | `expandvars(path: str) -> str` | 2 | Done |
| 46 | `os.path.normpath(path)` | `path_normpath(path: str) -> str` | 1 | Done |
| 47 | `os.path.relpath(path, start)` | `path_relpath(path: str, start: str) -> str` | 1 | Done |
| 48 | `os.path.commonpath(paths)` | `commonpath(paths: list[str]) -> str` | 2 | Done |
| 49 | `os.path.commonprefix(list)` | `commonprefix(paths: list[str]) -> str` | 2 | Done |
| 50 | `os.sep` | `const sep: str = "/"` | 1 | Done |
| 51 | `os.path.splitdrive(path)` | `splitdrive(path: str) -> list[str]` | 2 | Done |
| 52 | `os.path.getatime(path)` | `getatime(path: str) -> int` | 1 | Done |
| 53 | `os.path.getctime(path)` | `getctime(path: str) -> int` | 1 | Done |

## Environment Variables

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 54 | `os.environ[key]` / `os.getenv(key)` | `environ(name: str) -> str` | 0 | Done |
| 55 | `os.getenv(key, default)` | `environ_get(name: str, default: str) -> str` | 0 | Done |
| 56 | `os.putenv(key, value)` | `setenv_val(name: str, value: str) -> int` | 0 | Done |
| 57 | `os.unsetenv(key)` | `unsetenv_val(name: str) -> int` | 0 | Done |
| 58 | `os.get_exec_path()` | `get_exec_path() -> list[str]` | 2 | Done |

## Process Management

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 59 | `os.getpid()` | `pid() -> int` | 0 | Done |
| 60 | `os.getppid()` | `ppid() -> int` | 0 | Done |
| 61 | `os.getuid()` | `uid() -> int` | 0 | Done |
| 62 | `os.getgid()` | `gid() -> int` | 0 | Done |
| 63 | `os.geteuid()` | `euid() -> int` | 1 | Done |
| 64 | `os.getegid()` | `egid() -> int` | 1 | Done |
| 65 | `os.system(command)` | `run(cmd: str) -> int` | 0 | Done |
| 66 | `os._exit(n)` | `exit_now(status: int)` | 0 | Done |
| 67 | `os.isatty(fd)` | `is_terminal(fd: int) -> bool` | 1 | Done |
| 68 | `os.getlogin()` | `login_name() -> str` | 1 | Done |
| 69 | `os.uname()` | `uname_sysname()`, `uname_nodename()`, `uname_release()`, `uname_version()`, `uname_machine()` | 1 | Done |
| 70 | `os.umask(mask)` | `umask_set(mask: int) -> int` | 2 | Done |
| 71 | `os.getpgrp()` | `getpgrp_id() -> int` | 2 | Done |
| 72 | `os.setpgrp()` | `setpgrp_self() -> int` (== `setpgid(0, 0)`) | 2 | Done |
| 73 | `os.setsid()` | `setsid_proc() -> int` | 2 | Done |
| 74 | `os.kill(pid, sig)` | `kill_proc(target_pid: int, sig: int)` | 1 | Done |
| 75 | `os.waitpid(pid, options)` | `waitpid_proc(pid: int, options: int) -> list[int]` ([pid, status]) | 2 | Done |
| 76 | `os.fork()` | `fork_proc() -> int` | 2 | Done |
| 77 | `os.exec*()` | `execv_path(path, argv)`, `execvp_path(file, argv)` | 2 | Done |

## Constants & Module Attributes

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 78 | `os.name` | `const name: str = "posix"` | 1 | Done |
| 79 | `os.sep` | `const sep: str = "/"` | 1 | Done |
| 80 | `os.linesep` | `const linesep: str = "\n"` | 1 | Done |

---

## Summary by Phase

| Phase | Total | Done | Remaining |
|-------|-------|------|-----------|
| **Phase 0** (core) | 30 | 30 | 0 |
| **Phase 1** (common) | 28 | 28 | 0 |
| **Phase 2** (extended) | 22 | 22 | 0 |
| **Total** | **80** | **80** | **0** |

### Phase 2 - completed (22 items, 2026-05-01)
- File system: `scandir` (with `DirEntry` class), `chroot_to`, `renames`, `removedirs` (recursive parent prune)
- Stat: `lstat_size`/`lstat_mtime`/`lstat_atime`/`lstat_ctime`/`lstat_mode`/`lstat_isfile`/`lstat_isdir`, `samefile`, `ismount`
- Process: `fork_proc`, `execv_path`/`execvp_path`, `waitpid_proc`, `umask_set`, `getpgrp_id`, `setpgrp_self`, `setsid_proc`, `chown_path`
- Path: `commonpath`, `commonprefix`, `splitdrive`, `expandvars`
- Environment: `get_exec_path`

---

## Runtime C Helpers (lib/Runtime/runtime.cpp)

| Helper | Purpose |
|--------|---------|
| `dragon_stat_size(path)` | `stat()` → `st_size` |
| `dragon_stat_mtime(path)` | `stat()` → `st_mtime` |
| `dragon_stat_atime(path)` | `stat()` → `st_atime` |
| `dragon_stat_ctime(path)` | `stat()` → `st_ctime` |
| `dragon_stat_mode(path)` | `stat()` → `st_mode` |
| `dragon_stat_isfile(path)` | `stat()` → `S_ISREG` |
| `dragon_stat_isdir(path)` | `stat()` → `S_ISDIR` |
| `dragon_stat_islink(path)` | `lstat()` → `S_ISLNK` |
| `dragon_readdir_name(dirp)` | `readdir()` → `d_name` (strdup) |
| `dragon_readlink(path)` | `readlink()` → malloc'd string |
| `dragon_makedirs(path, mode)` | Recursive `mkdir -p` |
| `dragon_uname_sysname()` | `uname()` → `sysname` field |
| `dragon_uname_nodename()` | `uname()` → `nodename` field |
| `dragon_uname_release()` | `uname()` → `release` field |
| `dragon_uname_version()` | `uname()` → `version` field |
| `dragon_uname_machine()` | `uname()` → `machine` field |
| `dragon_getlogin()` | `getlogin()` → strdup (empty on fail) |
| `dragon_normpath(path)` | Resolve `.`, `..`, collapse `//` |
| `dragon_relpath(path, start)` | Relative path computation |
| `dragon_stat_dev(path)` / `dragon_stat_ino(path)` | `stat()` → device id / inode (used by `samefile`) |
| `dragon_lstat_size/mtime/atime/ctime/mode/dev/ino` | `lstat()` accessors (don't follow symlinks) |
| `dragon_lstat_isfile(path)` / `dragon_lstat_isdir(path)` | `lstat()` predicates |
| `dragon_chown(path, uid, gid)` / `dragon_chroot(path)` | POSIX wrappers; Windows stubs return -1/EINVAL |
| `dragon_execv(path, argv)` / `dragon_execvp(file, argv)` | Build C `argv` from Dragon `list[str]`; call execv/execvp |
| `dragon_waitpid(pid, options)` | Returns `[pid, status]` (Dragon list[int]) - bridges the `int*` out-arg |

---

## Notes

- **os.path vs os**: Python separates `os.path` into a submodule. Dragon flattens everything into `os` with `path_` prefix for path utilities (e.g. `path_join`, `path_basename`). This is intentional - Dragon doesn't have submodule imports yet.
- **stat fields**: Python returns a `stat_result` named tuple from `os.stat()`. Dragon splits this into individual helpers (`getsize`, `getmtime`, `getatime`, `getctime`, `getmode`, `isfile`, `isdir`, `islink`). A proper `Stat` class is a Phase 2 goal.
- **os.walk()**: Python's `os.walk()` yields `(dirpath, dirnames, filenames)` tuples. Dragon provides `walk_files(top)` and `walk_dirs(top)` which return flat `list[str]` of all paths recursively. This is simpler and covers the most common use case.
- **uname**: Python returns a named tuple with 5 fields. Dragon provides 5 individual functions (`uname_sysname`, `uname_nodename`, `uname_release`, `uname_version`, `uname_machine`) since named tuples don't exist yet.
- **File descriptors**: Python's low-level FD operations (`os.open`, `os.read`, `os.write`, `os.close`, `os.dup`, etc.) are deferred. Dragon uses `io.dr` with FILE* for high-level I/O. FD-level ops are Phase 2+.
- **Process execution**: `os.fork()`, `os.exec*()`, `os.spawn*()` are Phase 2. Dragon programs should use `run()` (wraps `system()`) for simple cases. A proper `subprocess` module is planned separately.
