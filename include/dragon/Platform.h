#pragma once

#include <string>

namespace dragon::platform {

std::string getTempDir();

// Fresh private temp dir (mode 0700, randomized name), "" on failure.
// Blocks a symlink-preplant TOCTOU attack on the predictable dragon_<pid> path.
std::string makeSecureTempDir(const std::string& prefix);

int getProcessId();
std::string getExecutablePath();
int getExitCode(int systemResult);
bool isDirectory(const std::string& path);

// Install prefix: strips trailing /bin/<exe> from getExecutablePath(); "" on failure.
std::string getInstallPrefix();

// The native path separator: '/' on POSIX, '\\' on Windows.
char pathSeparator();

// Native executable extension: "" on POSIX, ".exe" on Windows.
const char* exeExtension();

} // namespace dragon::platform
