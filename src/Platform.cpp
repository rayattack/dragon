#include "dragon/Platform.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
  #include <process.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <unistd.h>
  #include <sys/wait.h>
  #include <pwd.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <limits.h>
  #include <pwd.h>
#endif

namespace dragon::platform {

std::string getTempDir() {
#if defined(_WIN32)
    char buf[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof(buf), buf);
    if (n == 0 || n > sizeof(buf)) return std::string("C:\\Temp");
    // GetTempPathA includes a trailing backslash; strip it.
    while (n > 0 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) {
        buf[n - 1] = '\0';
        --n;
    }
    return std::string(buf);
#else
    if (const char* env = std::getenv("TMPDIR")) {
        if (env[0] != '\0') return std::string(env);
    }
    return std::string("/tmp");
#endif
}

int getProcessId() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

std::string makeSecureTempDir(const std::string& prefix) {
    std::string base = getTempDir();
#if defined(_WIN32)
    // No mkdtemp on Windows. CreateDirectoryA fails if the name already exists,
    // giving us exclusive creation; retry with fresh names until one is unused.
    // The per-user temp dir's default ACL already restricts other unprivileged
    // users, so the symlink-redirect attack the POSIX path defends against does
    // not apply the same way here. Names mix pid + tick + a counter so two
    // concurrent builds don't collide.
    static const char hex[] = "0123456789abcdef";
    for (int attempt = 0; attempt < 64; ++attempt) {
        unsigned long long r =
            (static_cast<unsigned long long>(GetCurrentProcessId()) << 32) ^
            (GetTickCount64() + static_cast<unsigned long long>(attempt) * 0x9E3779B1ULL);
        char rnd[17];
        for (int i = 0; i < 16; ++i) rnd[i] = hex[(r >> (i * 4)) & 0xF];
        rnd[16] = '\0';
        std::string dir = base + "\\" + prefix + rnd;
        if (CreateDirectoryA(dir.c_str(), nullptr)) return dir;
    }
    return {};
#else
    // mkdtemp() atomically creates a uniquely-named dir with mode 0700.
    std::string tmpl = base + "/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) return {};
    return std::string(buf.data());
#endif
}

std::string getDragonHomeDir() {
#if defined(_WIN32)
    const char* userProfile = std::getenv("USERPROFILE");
    if (!userProfile || userProfile[0] == '\0') return {};
    return std::string(userProfile) + "\\dragon";
#else
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
        // Fallback to getpwuid on POSIX if HOME is not set
        struct passwd* pw = getpwuid(getuid());
        if (!pw || !pw->pw_dir) return {};
        home = pw->pw_dir;
    }
    return std::string(home) + "/.dragon";
#endif
}

std::string makeFallbackTempDir(const std::string& prefix) {
    std::string homeDir = getDragonHomeDir();
    if (homeDir.empty()) return {};

    // Create ~/.dragon directory if it doesn't exist
    std::error_code ec;
    std::filesystem::create_directories(homeDir, ec);
    if (ec) return {};

#if defined(_WIN32)
    // Windows: similar to makeSecureTempDir but under ~/.dragon
    static const char hex[] = "0123456789abcdef";
    for (int attempt = 0; attempt < 64; ++attempt) {
        unsigned long long r =
            (static_cast<unsigned long long>(GetCurrentProcessId()) << 32) ^
            (GetTickCount64() + static_cast<unsigned long long>(attempt) * 0x9E3779B1ULL);
        char rnd[17];
        for (int i = 0; i < 16; ++i) rnd[i] = hex[(r >> (i * 4)) & 0xF];
        rnd[16] = '\0';
        std::string dir = homeDir + "\\" + prefix + rnd;
        if (CreateDirectoryA(dir.c_str(), nullptr)) return dir;
    }
    return {};
#else
    // POSIX: use mkdtemp under ~/.dragon
    std::string tmpl = homeDir + "/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) return {};
    return std::string(buf.data());
#endif
}

std::string getExecutablePath() {
#if defined(_WIN32)
    std::vector<char> buf(MAX_PATH);
    while (true) {
        DWORD n = GetModuleFileNameA(nullptr, buf.data(),
                                     static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return std::string(buf.data(), n);
        // Buffer too small (n == buf.size() and ERROR_INSUFFICIENT_BUFFER).
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    // Resolve symlinks / . / ..
    std::error_code ec;
    auto resolved = std::filesystem::canonical(buf.data(), ec);
    return ec ? std::string(buf.data()) : resolved.string();
#else
    // Linux / generic POSIX with /proc.
    std::error_code ec;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return resolved.string();
    return {};
#endif
}

int getExitCode(int systemResult) {
#if defined(_WIN32)
    return systemResult;
#else
    if (WIFEXITED(systemResult)) return WEXITSTATUS(systemResult);
    // The child died from a signal (SIGSEGV, SIGABRT, ...). Surface the
    // shell-conventional 128+signum so a crashing `dragon run` reports a
    // meaningful nonzero status (e.g. 139 for SIGSEGV) instead of masking it
    // as a generic failure. Without this, the old `return systemResult` leaked
    // the raw status word (signal in the low byte), which callers misread.
    if (WIFSIGNALED(systemResult)) return 128 + WTERMSIG(systemResult);
    return systemResult;
#endif
}

bool isDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::string getInstallPrefix() {
    auto exe = getExecutablePath();
    if (exe.empty()) return {};
    std::filesystem::path p(exe);
    // Strip the executable name and the bin/ directory.
    auto parent = p.parent_path();          // <prefix>/bin
    if (parent.empty()) return {};
    return parent.parent_path().string();   // <prefix>
}

char pathSeparator() {
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

const char* exeExtension() {
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
}

} // namespace dragon::platform
