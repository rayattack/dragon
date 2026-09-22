#pragma once

#include <string>

namespace dragon::platform {

std::string getTempDir();

std::string makeSecureTempDir(const std::string& prefix);

// Get the dragon home directory (~/.dragon). Returns "" on failure.
std::string getDragonHomeDir();

// Create a fallback temporary directory under ~/.dragon/<prefix><unique>.
// Returns "" on failure. This is used when makeSecureTempDir fails.
std::string makeFallbackTempDir(const std::string& prefix);

int getProcessId();
std::string getExecutablePath();
int getExitCode(int systemResult);
bool isDirectory(const std::string& path);

std::string getInstallPrefix();

char pathSeparator();

const char* exeExtension();

}
