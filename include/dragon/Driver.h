#ifndef DRAGON_DRIVER_H
#define DRAGON_DRIVER_H

#include <string>
#include <vector>
#include <memory>

namespace dragon {

/// Compiler driver options
struct DriverOptions {
    enum class Action {
        Run,      // Compile and execute
        Build,    // Compile to executable
        Check,    // Type check only
        Emit      // Emit IR/ASM
    };

    Action action = Action::Build;
    std::vector<std::string> inputFiles;
    // `dragon run file.dr a b c` - args after the program file are forwarded to
    // the program's argv (Python parity), not treated as more source files.
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
};

/// Main compiler driver
/// 
/// Orchestrates the compilation pipeline:
/// Lexer -> Parser -> Sema -> TypeChecker -> CodeGen
class Driver {
public:
    Driver();
    ~Driver();

    /// Parse command line arguments
    bool parseArgs(int argc, char* argv[]);

    /// Run the compiler with parsed options
    int run();

    /// Run with explicit options
    int run(const DriverOptions& options);

    /// Print usage information
    static void printUsage();

    /// Print version
    static void printVersion();

private:
    // Pipeline stages
    int runFile(const std::string& filename);
    int buildFile(const std::string& filename);
    int checkFile(const std::string& filename);

    // File handling
    std::string readFile(const std::string& filename);
    bool isDragonFile(const std::string& filename);
    bool isPythonFile(const std::string& filename);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_DRIVER_H
