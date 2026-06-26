#include "dragon/StdlibRegistry.h"

namespace dragon {

StdlibRegistry::StdlibRegistry() {
    // Standard library mapping: (module, symbol) -> (c_include, c_name)
    // Extracted from CEmitter.cpp getStdlibRegistry()
    registry_ = {
        {"math", {
            {"sqrt",  {"math.h", "sqrt"}},
            {"sin",   {"math.h", "sin"}},
            {"cos",   {"math.h", "cos"}},
            {"tan",   {"math.h", "tan"}},
            {"log",   {"math.h", "log"}},
            {"log10", {"math.h", "log10"}},
            {"log2",  {"math.h", "log2"}},
            {"exp",   {"math.h", "exp"}},
            {"pow",   {"math.h", "pow"}},
            {"fabs",  {"math.h", "fabs"}},
            {"abs",   {"math.h", "fabs"}},
            {"ceil",  {"math.h", "ceil"}},
            {"floor", {"math.h", "floor"}},
            {"pi",    {"math.h", "M_PI"}},
            {"e",     {"math.h", "M_E"}},
        }},
        {"os", {
            {"getcwd",  {"unistd.h", "getcwd(NULL, 0)"}},
            {"getenv",  {"stdlib.h", "getenv"}},
            {"system",  {"stdlib.h", "system"}},
            {"getpid",  {"unistd.h", "getpid"}},
        }},
        {"sys", {
            {"exit",    {"stdlib.h", "exit"}},
        }},
        {"time", {
            {"time",    {"time.h", "time(NULL)"}},
            {"clock",   {"time.h", "clock"}},
            {"sleep",   {"unistd.h", "sleep"}},
        }},
        {"string", {
            {"ascii_lowercase", {"", "\"abcdefghijklmnopqrstuvwxyz\""}},
            {"ascii_uppercase", {"", "\"ABCDEFGHIJKLMNOPQRSTUVWXYZ\""}},
            {"digits",          {"", "\"0123456789\""}},
        }},
        {"random", {
            {"random",    {"stdlib.h", "((double)rand() / RAND_MAX)"}},
            {"randint",   {"stdlib.h", "dragon_random_randint"}},
            {"seed",      {"stdlib.h", "srand"}},
            {"choice",    {"stdlib.h", "dragon_random_choice"}},
        }},
        {"json", {
            {"dumps",     {"", "dragon_json_dumps"}},
            {"loads",     {"", "dragon_json_loads"}},
        }},
        {"re", {
            {"match",     {"regex.h", "dragon_re_match"}},
            {"search",    {"regex.h", "dragon_re_search"}},
            {"sub",       {"regex.h", "dragon_re_sub"}},
            {"findall",   {"regex.h", "dragon_re_findall"}},
        }},
        {"collections", {
            {"Counter",       {"", "dragon_counter_new"}},
            {"defaultdict",   {"", "dragon_defaultdict_new"}},
            {"OrderedDict",   {"", "dragon_dict_new"}},
        }},
        {"hashlib", {
            {"md5",       {"", "dragon_hashlib_md5"}},
            {"sha256",    {"", "dragon_hashlib_sha256"}},
        }},
        {"datetime", {
            {"now",           {"time.h", "dragon_datetime_now"}},
            {"timestamp",     {"time.h", "time(NULL)"}},
        }},
        {"functools", {
            {"reduce",    {"", "dragon_functools_reduce"}},
        }},
        {"itertools", {
            {"range",     {"", "dragon_range"}},
            {"enumerate", {"", "dragon_enumerate"}},
            {"zip",       {"", "dragon_zip"}},
        }},
    };
}

const StdlibRegistry& StdlibRegistry::instance() {
    static StdlibRegistry reg;
    return reg;
}

const std::map<std::string, StdlibEntry>* StdlibRegistry::findModule(const std::string& moduleName) const {
    auto it = registry_.find(moduleName);
    return it != registry_.end() ? &it->second : nullptr;
}

void StdlibRegistry::resolveImport(const std::string& moduleName,
                                    const std::string& asName,
                                    std::map<std::string, std::string>& outSymbolAliases,
                                    std::set<std::string>& outExtraIncludes) const {
    auto* mod = findModule(moduleName);
    if (!mod) return;
    for (auto& [sym, entry] : *mod) {
        if (entry.cInclude[0] != '\0') {
            outExtraIncludes.insert(entry.cInclude);
        }
        std::string qualName = (asName.empty() ? moduleName : asName) + "." + sym;
        outSymbolAliases[qualName] = entry.cName;
    }
}

bool StdlibRegistry::resolveFromImport(const std::string& moduleName,
                                        const std::string& symbolName,
                                        const std::string& asName,
                                        std::map<std::string, std::string>& outSymbolAliases,
                                        std::set<std::string>& outExtraIncludes) const {
    auto* mod = findModule(moduleName);
    if (!mod) return false;
    auto symIt = mod->find(symbolName);
    if (symIt == mod->end()) return false;
    if (symIt->second.cInclude[0] != '\0') {
        outExtraIncludes.insert(symIt->second.cInclude);
    }
    std::string pyName = asName.empty() ? symbolName : asName;
    outSymbolAliases[pyName] = symIt->second.cName;
    return true;
}

void StdlibRegistry::resolveFromImportStar(const std::string& moduleName,
                                            std::map<std::string, std::string>& outSymbolAliases,
                                            std::set<std::string>& outExtraIncludes) const {
    auto* mod = findModule(moduleName);
    if (!mod) return;
    for (auto& [sym, entry] : *mod) {
        if (entry.cInclude[0] != '\0') {
            outExtraIncludes.insert(entry.cInclude);
        }
        outSymbolAliases[sym] = entry.cName;
    }
}

} // namespace dragon
