#pragma once

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dragon::test {

inline std::string codeBlockPath(const std::string& file) {
    if (file.find('/') != std::string::npos) {
        return std::string(DRAGON_ROOT_DIR) + "/" + file;
    }
    return std::string(DRAGON_TEST_CASES_DIR) + "/" + file;
}

inline const std::string& codeBlockSource(const std::string& file) {
    static std::map<std::string, std::string> cache;
    auto cached = cache.find(file);
    if (cached != cache.end()) return cached->second;

    const std::string path = codeBlockPath(file);
    std::ifstream in(path);
    if (!in) throw std::runtime_error("code block file not found: " + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return cache.emplace(file, buffer.str()).first->second;
}

inline bool isBlockHeading(const std::string& line, const std::string& block) {
    size_t i = 0;
    while (i < line.size() && line[i] == '#') ++i;
    if (i == 0) return false;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size() || line[i] != ':') return false;
    size_t end = line.size();
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\r')) --end;
    return line.compare(i + 1, end - i - 1, block) == 0;
}

inline bool isFence(const std::string& line) {
    return line.compare(0, 3, "```") == 0;
}

inline std::string extractCode(const std::string& file, const std::string& block) {
    std::istringstream lines(codeBlockSource(file));
    std::string line;

    bool found = false;
    while (!found && std::getline(lines, line)) {
        found = isBlockHeading(line, block);
    }
    if (!found) {
        throw std::runtime_error("no block ':" + block + "' in " + codeBlockPath(file));
    }

    bool opened = false;
    while (!opened && std::getline(lines, line)) {
        if (!line.empty() && line[0] == '#') break;
        opened = isFence(line);
    }
    if (!opened) {
        throw std::runtime_error(
            "block ':" + block + "' has no code fence in " + codeBlockPath(file));
    }

    std::string code;
    bool closed = false;
    while (!closed && std::getline(lines, line)) {
        closed = isFence(line);
        if (!closed) {
            code += line;
            code += '\n';
        }
    }
    if (!closed) {
        throw std::runtime_error(
            "unterminated code fence for ':" + block + "' in " + codeBlockPath(file));
    }
    if (code.empty()) {
        throw std::runtime_error("block ':" + block + "' is empty in " + codeBlockPath(file));
    }
    return code;
}

}
