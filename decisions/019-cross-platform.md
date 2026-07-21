# Decision 019: Cross-Platform Distribution

> **Status:** Largely implemented. Dragon is **not** Linux-only anymore. `Driver.cpp`/`CodeGen.cpp` have `__linux__`/`__APPLE__`/`_WIN32` branches, CPack per OS, release CI matrix (linux-x86_64, macos-arm64, macos-x86_64 solid; windows-x86_64 experimental). What follows is the original audit I wrote when everything was hardcoded to my laptop.

I used to ship builds that only worked on my x86_64 Linux box - absolute paths baked at configure time, `cc` hardcoded, `/tmp` everywhere. Porting to macOS meant discovering `-ldl` doesn't exist there; Windows meant seven POSIX headers in `runtime.cpp` laughing at me. LLVM and bundled libs (minicoro, sqlite3, pcre2, mbedTLS, llhttp) are cross-platform; our I/O uses **raw epoll/kqueue/WSAPoll, not libuv** (original draft said libuv, never adopted).

## Audit Summary

### Already Cross-Platform (no changes needed)
- LLVM target triple detection, `compileToObject` - pure LLVM, portable
- libuv (has `src/win/` and `src/unix/` internally), minicoro (x86_64/ARM + Windows Fiber fallback)
- sqlite3 amalgamation (C89), pcre2 (CMake platform detection built in)

### What Breaks on macOS
- `linkExecutable` (`src/CodeGen.cpp:1757-1790`) uses `-ldl` - doesn't exist on macOS (dlopen in libSystem)
- Everything else compiles - macOS is POSIX

### What Breaks on Windows
- **7 POSIX-only headers** in `lib/Runtime/runtime.cpp:10-17`: `pthread.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `unistd.h`, `sys/utsname.h`, `dirent.h`
- **Hardcoded `cc` linker** (`src/CodeGen.cpp:1757`) - doesn't exist on MSVC
- **Unix link flags** `-ldl`, `-lpthread`, `-lm` (`src/CodeGen.cpp:1763,1772,1786,1788`)
- **`/tmp` hardcoded** in `src/Driver.cpp:174,412` and all test files
- **`WEXITSTATUS`** macro (`src/Driver.cpp:185`), **`<unistd.h>`** (`src/Driver.cpp:15`), **`<sys/wait.h>`** (`src/Driver.cpp:16`)
- **`popen("python3 ... 2>/dev/null")`** in `src/ModuleResolver.cpp:19`
- **`sysconf`**, **`usleep`**, **`readdir`/`opendir`**, **`uname`** in runtime (lines 2854, 2957, 3500, 3574-3604)

### What Prevents Relocation (all platforms)
- `DRAGON_RUNTIME_LIB`, `DRAGON_STDLIB_DIR` etc. are **absolute paths baked at configure time** (`CMakeLists.txt:94-99,104-109`)
- No `install` for stdlib or bundled `.a` libs (`CMakeLists.txt:111-114` - only installs binary, dragon_lib, dragon_runtime, and headers)
- No CPack, no runtime path discovery

---

## Implementation Plan

### Phase 1: Relocatable Path Discovery

Make the installed binary find its stdlib and libraries without hardcoded paths.

**`src/Driver.cpp`** - add `resolveInstallPrefix`:
- Linux: `readlink("/proc/self/exe")` → strip `/bin/dragon` → get prefix
- macOS: `_NSGetExecutablePath` → strip `/bin/dragon`
- Windows: `GetModuleFileNameW` → strip `\bin\dragon.exe`
- From prefix, search `<prefix>/share/dragon/stdlib/` and `<prefix>/lib/dragon/`
- Fallback chain: `$DRAGON_STDLIB_PATH` env → relative to binary → compile-time `DRAGON_STDLIB_DIR`
- Same for runtime lib: `$DRAGON_LIB_PATH` env → `<prefix>/lib/dragon/libdragon_runtime.a` → compile-time path

**`CMakeLists.txt`** - fix install targets:
```cmake
install(TARGETS dragon DESTINATION bin)
install(TARGETS dragon_runtime DESTINATION lib/dragon)
install(TARGETS dragon_sqlite3 DESTINATION lib/dragon)
install(FILES $<TARGET_FILE:pcre2-8-static> DESTINATION lib/dragon)
install(FILES $<TARGET_FILE:uv_a> DESTINATION lib/dragon)
install(DIRECTORY stdlib/ DESTINATION share/dragon/stdlib)
install(DIRECTORY include/ DESTINATION include)
```

### Phase 2: Platform Abstraction Layer

**New file `include/dragon/Platform.h`** + **`src/Platform.cpp`**:
```cpp
namespace dragon::platform {
 std::string getTempDir; // /tmp or %TEMP%
 int getProcessId; // getpid or _getpid
 std::string getExecutablePath; // /proc/self/exe or equivalent
 int getExitCode(int systemResult); // WEXITSTATUS or direct
 bool isDirectory(const std::string& path); // replaces stat+S_ISDIR
}
```

**`src/Driver.cpp`** - replace:
- `#include <unistd.h>` / `#include <sys/wait.h>` → `#include "dragon/Platform.h"`
- `/tmp/dragon_run_` + `getpid` → `platform::getTempDir + "/dragon_run_" + std::to_string(platform::getProcessId)`
- `WEXITSTATUS(result)` → `platform::getExitCode(result)`

**`src/ModuleResolver.cpp`** - replace:
- `#include <sys/stat.h>` → `#include "dragon/Platform.h"` or `#include <filesystem>` (C++17)
- `stat` + `S_ISDIR` → `std::filesystem::is_directory`
- `popen("python3 ... 2>/dev/null")` → platform-aware: Windows uses `python` not `python3`, and `2>nul` not `2>/dev/null`

### Phase 3: Cross-Platform Linker

**`src/CodeGen.cpp` `linkExecutable`** - platform-aware link command:

```
#if defined(__APPLE__)
 // macOS: no -ldl (in libSystem), use -lpthread
 cc -o out obj.o runtime.a libuv.a -lm -lpthread
#elif defined(_WIN32)
 // Windows MinGW: no -ldl/-lpthread, add winsock
 cc -o out.exe obj.o runtime.a libuv.a -lws2_32 -liphlpapi -lpsapi -luserenv
#else
 // Linux: current behavior
 cc -o out obj.o runtime.a libuv.a -lm -lpthread -ldl
#endif
```

Also: `.exe` extension on Windows, `2>nul` instead of `2>&1` on Windows.

**`test/CodeGenTest.cpp`** - same changes in `compileAndRun` / `compileAndRunPy` helpers.

### Phase 4: Runtime Platform Ifdefs

**`lib/Runtime/runtime.cpp`** - largest phase, ~150 lines of ifdefs.

Header block:
```cpp
#ifdef _WIN32
 #include <windows.h>
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <io.h>
 #include <process.h>
 #include <direct.h>
#else
 #include <pthread.h>
 #include <sys/socket.h>
 // ... existing POSIX headers
#endif
```

Key functions to wrap:
- Threading: `pthread_*` → Windows `CreateThread` / `InitializeCriticalSection` (or libuv threading)
- `sysconf(_SC_NPROCESSORS_ONLN)` → Windows `GetSystemInfo.dwNumberOfProcessors`
- `usleep(us)` → Windows `Sleep(us/1000)` (or `uv_sleep`)
- `readdir`/`opendir` → Windows `FindFirstFileA`/`FindNextFileA`
- `uname` → Windows `GetVersionExA` + `GetComputerNameA`
- `getcwd` → Windows `_getcwd`

### Phase 5: CPack Packaging

**`CMakeLists.txt`** - add at bottom:
```cmake
# --- Packaging ---
set(CPACK_PACKAGE_NAME "dragon")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dragon: typed, compiled Python variant targeting LLVM")
set(CPACK_PACKAGE_VENDOR "Dragon Language")

if(UNIX AND NOT APPLE)
 set(CPACK_GENERATOR "DEB;TGZ")
 set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Dragon Team")
 set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.17)")
 set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
elseif(APPLE)
 set(CPACK_GENERATOR "DragNDrop;TGZ")
 set(CPACK_DMG_VOLUME_NAME "Dragon")
elseif(WIN32)
 set(CPACK_GENERATOR "NSIS;ZIP")
 set(CPACK_NSIS_MODIFY_PATH ON)
endif

include(CPack)
```

Build packages: `cmake --build . --target package`

### Phase 6: GitHub Actions CI/CD

**`.github/workflows/ci.yml`** - build + test on every push:
- Linux (ubuntu-latest): `apt install llvm-18-dev`, cmake, ctest
- macOS (macos-latest): `brew install llvm`, cmake, ctest
- Windows (windows-latest): choco install llvm, cmake with MinGW, ctest

**`.github/workflows/release.yml`** - on tag `v*`:
- Build on all 3 platforms
- ctest on all 3
- CPack to generate packages
- Upload .deb, .dmg, .exe, .tar.gz, .zip to GitHub Releases

---

## Recommended Execution Order

| Order | Phase | Effort | What It Unlocks |
|-------|-------|--------|-----------------|
| 1st | Phase 1: Relocatable paths | 2-3 hrs | `make install` works, tar.gz usable |
| 2nd | Phase 5: CPack | 1-2 hrs | .deb/.tar.gz packages for Linux |
| 3rd | Phase 6: CI/CD | 2-3 hrs | Automated builds, catches regressions |
| 4th | Phase 2: Platform abstraction | 2-3 hrs | Clean separation, prep for macOS/Windows |
| 5th | Phase 3: Cross-platform linker | 2-3 hrs | macOS fully works, Windows MinGW works |
| 6th | Phase 4: Runtime ifdefs | 4-6 hrs | Windows compilation (biggest phase) |

**Total: ~15-20 hours**

Phases 1+5 give a distributable Linux package immediately. Phase 6 keeps future work CI-tested. Phases 2-4 extend to macOS and Windows. Teh Windows phase is still the hairy one if you're doing it by hand.

## Verification

1. `cmake --install . --prefix /tmp/dragon-install && /tmp/dragon-install/bin/dragon run hello.dr`
2. `cmake --build . --target package` → install .deb → `dragon run hello.dr`
3. macOS: `brew install llvm && cmake .. && make && make install && dragon run hello.dr`
4. GitHub Actions green on all platforms
5. Fresh Ubuntu VM: `sudo dpkg -i dragon-*.deb && dragon run hello.dr` → "Hello World"
