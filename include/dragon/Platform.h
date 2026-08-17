#pragma once

#include <string>

namespace dragon::platform {

std::string getTempDir();

std::string makeSecureTempDir(const std::string& prefix);

int getProcessId();
std::string getExecutablePath();
int getExitCode(int systemResult);
bool isDirectory(const std::string& path);

std::string getInstallPrefix();

char pathSeparator();

const char* exeExtension();

}
