#ifndef DRAGON_DRIVER_H
#define DRAGON_DRIVER_H

#include <string>
#include <vector>
#include <memory>

namespace dragon {

/// Compiler driver options
struct DriverOptions {
    enum class Action {
        Run,      // compile and execute
        Build,    // compile to executable
        Check,    // type check only
        Emit,     // emit IR/ASM
        FfiSync   // D052: regenerate process-extern stubs
    };

    Action action = Action::Build;
    std::vector<std::string> inputFiles;
    // Args after the program file in `dragon run file.dr a b c` forward to
    // argv (Python parity), not treated as source files.
    std::vector<std::string> programArgs;
    std::string outputFile;
    int optimizationLevel = 0;
    bool verbose = false;
    bool debugInfo = false;
    bool forcePython = false;  // -f flag for Python files
    bool dumpAst = false;      // --dump-ast flag
    bool dumpTokens = false;   // --dump-tokens flag
    std::vector<std::string> searchPaths;  // -I dirs for module search
    bool enableSitePackages = false;  // --site-packages flag
    std::vector<std::string> linkedLibraries;    // -l flags (e.g. "curl", "m")
    std::vector<std::string> librarySearchPaths; // -L flags (e.g. "/usr/local/lib")
    std::vector<std::string> ccSources;          // --cc-source: C/C++ shims to compile+link (ADR 041)
    std::string gcMode = "rc";  // --gc=rc (default) or --gc=none
    bool checkOverflow = false; // --check-overflow: raise OverflowError on int overflow
    bool ffiCheck = false;      // ffi sync --check: verify stubs, write nothing
};

/// Orchestrates the compile pipeline: Lexer -> Parser -> Sema -> TypeChecker -> CodeGen.
class Driver {
public:
    Driver();
    ~Driver();

    bool parseArgs(int argc, char* argv[]);
    int run();
    int run(const DriverOptions& options);

    static void printUsage();
    static void printVersion();

private:
    int runFile(const std::string& filename);
    int buildFile(const std::string& filename);
    int checkFile(const std::string& filename);

    std::string readFile(const std::string& filename);
    bool isDragonFile(const std::string& filename);
    bool isPythonFile(const std::string& filename);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_DRIVER_H
