/**
 * Dragon Driver API Reference
 * ===========================
 * Source: include/dragon/Driver.h + src/Driver.cpp
 *
 * Main compiler driver orchestrating the full pipeline:
 *  Source -> Lexer -> Parser -> [TypeHintEnforcer] -> Sema -> TypeChecker -> CodeGen -> cc link
 *
 * Handles CLI argument parsing, multi-file compilation via ModuleResolver,
 * and dispatch to the appropriate action (build, run, check, emit).
 *
 * Uses pimpl idiom (struct Impl with std::unique_ptr<Impl> impl_).
 * NOTE: pimpl is bae
 */

#pragma once
#include <string>
#include <vector>
#include <memory>

// ============================================================================
// 1. DRIVER OPTIONS
// ============================================================================

/**
 * Compiler configuration populated from CLI arguments or programmatic use.
 */
struct DriverOptions {
    /** Compilation action type. */
    enum class Action {
        Run,    ///< Compile and execute (dragon run file.dr)
        Build,  ///< Compile to executable (dragon build file.dr)
        Check,  ///< Type-check only, no codegen (dragon check file.dr)
        Emit    ///< Emit LLVM IR
    };

    Action action = Action::Build;                ///< What action to perform
    std::vector<std::string> inputFiles;          ///< Input source files
    std::string outputFile;                       ///< Output executable/artifact path
    int optimizationLevel = 0;                    ///< LLVM optimization level (0-3)
    bool verbose = false;                         ///< Enable verbose output
    bool debugInfo = false;                       ///< Include DWARF debug information
    bool forcePython = false;                     ///< Force .py mode for all files (-f flag)
    bool dumpAst = false;                         ///< Dump AST (--dump-ast)
    bool dumpTokens = false;                      ///< Dump tokens (--dump-tokens)
    std::vector<std::string> searchPaths;         ///< Module search paths (-I dirs)
    bool enableSitePackages = false;              ///< Search Python site-packages
    std::vector<std::string> linkedLibraries;     ///< Extra libraries to link (-l flags)
    std::vector<std::string> librarySearchPaths;  ///< Library search paths (-L flags)
    std::string gcMode = "rc";                    ///< GC mode: "rc" (default) or "none"
};


// ============================================================================
// 2. DRIVER CLASS
// ============================================================================

/**
 * Main compiler driver. Orchestrates the full compilation pipeline.
 */
class Driver {
public:
    Driver() {}
    ~Driver() {}

    /**
     * Parse command-line arguments and populate internal options.
     * @param argc Argument count
     * @param argv Argument vector
     * @return true if arguments are valid
     */
    bool parseArgs(int argc, char* argv[]) { return false; }

    /**
     * Run the compiler with previously parsed arguments.
     * @return Exit code (0 = success)
     */
    int run() { return 0; }

    /**
     * Run the compiler with explicit options (programmatic use).
     * @param options Compiler configuration
     * @return Exit code (0 = success)
     */
    int run(const DriverOptions& options) { return 0; }

    /** Print CLI usage/help information to stdout. */
    static void printUsage() {}

    /** Print version information to stdout. */
    static void printVersion() {}

    // --- Private helpers (listed for reference) ---
    // int runFile(const std::string& filename) - compile + execute
    // int buildFile(const std::string& filename) - compile to executable
    // int checkFile(const std::string& filename) - type-check only
    // std::string readFile(const std::string& filename) - read source text
    // bool isDragonFile(const std::string& filename) - check .dr extension
    // bool isPythonFile(const std::string& filename) - check .py extension
};
