#pragma once

#include <string>

namespace dragon::platform {

std::string getTempDir();

// Create a fresh private temporary directory (mode 0700 on POSIX) and return
// its absolute path, or "" on failure. Unlike a predictable
// getTempDir()/dragon_<pid> path, the name is randomized and the directory is
// owner-only, so a local attacker cannot pre-create a symlink at the build
// artifact path and redirect or hijack the link step (TOCTOU privesc).
std::string makeSecureTempDir(const std::string& prefix);

int getProcessId();
std::string getExecutablePath();
int getExitCode(int systemResult);
bool isDirectory(const std::string& path);

// Path discovery for installed binaries.
// Strips trailing /bin/<exe> from getExecutablePath() to find the install prefix.
// Returns empty string if discovery fails.
std::string getInstallPrefix();

// The native path separator: '/' on POSIX, '\\' on Windows.
char pathSeparator();

// Native executable extension: "" on POSIX, ".exe" on Windows.
const char* exeExtension();

} // namespace dragon::platform
