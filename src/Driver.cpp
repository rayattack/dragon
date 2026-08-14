#include "dragon.h"
#include "dragon/Driver.h"
#include "FfiSync.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"
#include "dragon/Sema.h"
#include "dragon/DefiniteAssignment.h"
#include "dragon/OwnershipCheck.h"
#include "dragon/TypeChecker.h"
#include "dragon/CodeGen.h"
#include "dragon/TypeHintEnforcer.h"
#include "dragon/ModuleResolver.h"
#include "dragon/DiagnosticFormatter.h"
#include "dragon/Platform.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
  #include <process.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
#endif

namespace dragon {

namespace {

/// Search the install prefix for the Dragon stdlib dir (share/dragon/stdlib
/// or lib/dragon/stdlib); empty string if not found.
std::string findStdlibUnderPrefix(const std::string& prefix) {
    if (prefix.empty()) return {};
    namespace fs = std::filesystem;
    for (const char* sub : {"share/dragon/stdlib", "lib/dragon/stdlib"}) {
        fs::path candidate = fs::path(prefix) / sub;
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) return candidate.string();
    }
    return {};
}

/// Locate the platform/ tree (native per-program shim sources, e.g. the
/// webview shell) under an install prefix.
std::string findPlatformUnderPrefix(const std::string& prefix) {
    if (prefix.empty()) return {};
    namespace fs = std::filesystem;
    for (const char* sub : {"share/dragon/platform", "lib/dragon/platform"}) {
        fs::path candidate = fs::path(prefix) / sub;
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) return candidate.string();
    }
    return {};
}

/// Resolve the runtime libdragon_runtime.a alongside the install prefix.
std::string findRuntimeUnderPrefix(const std::string& prefix) {
    if (prefix.empty()) return {};
    namespace fs = std::filesystem;
    for (const char* name : {
            "lib/dragon/libdragon_runtime.a",
            "lib/libdragon_runtime.a"}) {
        fs::path candidate = fs::path(prefix) / name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate.string();
    }
    return {};
}

/// Resolve a bundled library (e.g. sqlite3, pcre2-8, llhttp) under the install
/// prefix, falling back to the compile-time path.
std::string findBundledLib(const std::string& prefix,
                            const std::string& filename,
                            const std::string& compileTimePath) {
    namespace fs = std::filesystem;
    if (!prefix.empty()) {
        for (const char* sub : {"lib/dragon", "lib"}) {
            fs::path candidate = fs::path(prefix) / sub / filename;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) return candidate.string();
        }
    }
    return compileTimePath;
}

/// Resolve the stdlib dir: $DRAGON_STDLIB_PATH env var, then
/// <prefix>/share/dragon/stdlib, then compile-time DRAGON_STDLIB_DIR.
std::string resolveStdlibDir() {
    if (const char* env = std::getenv("DRAGON_STDLIB_PATH")) {
        if (env[0] != '\0') return std::string(env);
    }
    auto prefix = platform::getInstallPrefix();
    auto installed = findStdlibUnderPrefix(prefix);
    if (!installed.empty()) return installed;
#ifdef DRAGON_STDLIB_DIR
    return std::string(DRAGON_STDLIB_DIR);
#else
    return {};
#endif
}

/// Resolve the platform/ tree (native per-program shim sources) via the same
/// fallback chain: $DRAGON_PLATFORM_PATH env var, install prefix, then DRAGON_PLATFORM_DIR.
std::string resolvePlatformDir() {
    if (const char* env = std::getenv("DRAGON_PLATFORM_PATH")) {
        if (env[0] != '\0') return std::string(env);
    }
    auto prefix = platform::getInstallPrefix();
    auto installed = findPlatformUnderPrefix(prefix);
    if (!installed.empty()) return installed;
#ifdef DRAGON_PLATFORM_DIR
    return std::string(DRAGON_PLATFORM_DIR);
#else
    return {};
#endif
}

/// Resolve the runtime archive using the same fallback chain.
std::string resolveRuntimeLib() {
    if (const char* env = std::getenv("DRAGON_LIB_PATH")) {
        if (env[0] != '\0') return std::string(env);
    }
    auto prefix = platform::getInstallPrefix();
    auto installed = findRuntimeUnderPrefix(prefix);
    if (!installed.empty()) return installed;
#ifdef DRAGON_RUNTIME_LIB
    return std::string(DRAGON_RUNTIME_LIB);
#else
    return {};
#endif
}

// dragon-egg sidecar (D022): the Driver can't call the pure-Dragon .drs parser
// directly, so it execs the compiled dragon-egg binary (or runs egg.dr via this `dragon` in a dev tree).

/// Resolve the dragon-egg sidecar binary, or "" if not found.
std::string resolveEggBin() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("DRAGON_EGG_PATH")) {
        if (env[0] != '\0') return std::string(env);
    }
    auto prefix = platform::getInstallPrefix();
    if (!prefix.empty()) {
        fs::path cand = fs::path(prefix) / "bin" /
            (std::string("dragon-egg") + platform::exeExtension());
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) return cand.string();
    }
#ifdef DRAGON_EGG_BIN
    {
        std::error_code ec;
        if (fs::is_regular_file(std::string(DRAGON_EGG_BIN), ec))
            return std::string(DRAGON_EGG_BIN);
    }
#endif
    return {};
}

/// argv for invoking the sidecar with subArgs; prefers the compiled binary,
/// falls back to `dragon run <egg.dr> -- <subArgs>` in a dev tree. Empty = unavailable.
std::vector<std::string> buildEggArgv(const std::vector<std::string>& subArgs) {
    std::vector<std::string> argv;
    std::string bin = resolveEggBin();
    if (!bin.empty()) {
        argv.push_back(bin);
        for (const auto& a : subArgs) argv.push_back(a);
        return argv;
    }
#ifdef DRAGON_EGG_SRC
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::is_regular_file(std::string(DRAGON_EGG_SRC), ec)) {
            argv.push_back(platform::getExecutablePath());
            argv.push_back("run");
            argv.push_back(std::string(DRAGON_EGG_SRC));
            argv.push_back("--");
            for (const auto& a : subArgs) argv.push_back(a);
        }
    }
#endif
    return argv;
}

/// Run the sidecar inheriting stdio (init/grab/sync/...). Returns its exit
/// code, or 1 if it could not be located/spawned.
int execEggPassthrough(const std::vector<std::string>& subArgs) {
    auto argv = buildEggArgv(subArgs);
    if (argv.empty()) {
        std::cerr << "dragon: package CLI (dragon-egg) not found\n";
        return 1;
    }
#if defined(_WIN32)
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += ' ';
        cmd += '"' + argv[i] + '"';
    }
    return platform::getExitCode(std::system(cmd.c_str()));
#else
    pid_t pid = fork();
    if (pid == -1) return 1;
    if (pid == 0) {
        std::vector<const char*> ev;
        for (const auto& a : argv) ev.push_back(a.c_str());
        ev.push_back(nullptr);
        execvp(argv[0].c_str(), const_cast<char* const*>(ev.data()));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return platform::getExitCode(status);
#endif
}

/// Run the sidecar capturing its stdout into `out` (for `entry` queries).
/// Returns its exit code, or 1 if it could not be located/spawned.
int execEggCapture(const std::vector<std::string>& subArgs, std::string& out) {
    auto argv = buildEggArgv(subArgs);
    if (argv.empty()) return 1;
#if defined(_WIN32)
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += ' ';
        cmd += '"' + argv[i] + '"';
    }
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return 1;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    return platform::getExitCode(_pclose(p));
#else
    int fds[2];
    if (pipe(fds) != 0) return 1;
    pid_t pid = fork();
    if (pid == -1) { close(fds[0]); close(fds[1]); return 1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        std::vector<const char*> ev;
        for (const auto& a : argv) ev.push_back(a.c_str());
        ev.push_back(nullptr);
        execvp(argv[0].c_str(), const_cast<char* const*>(ev.data()));
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return platform::getExitCode(status);
#endif
}

/// True if `cmd` is a D022 package-manager verb handled by the sidecar.
bool isEggVerb(const std::string& cmd) {
    static const char* verbs[] = {
        "init", "grab", "drop", "sync", "bump", "list", "info",
        "hash", "find", "push", "yank", "wipe", "scan"
    };
    for (const char* v : verbs) if (cmd == v) return true;
    return false;
}

/// Type-checks the entry module plus its dependency graph; the single registration path
/// shared by build/run/check. Fills depModules with the dependency ASTs; returns 0 or 1.
int typeCheckModuleGraph(Module& entryModule,
                         const std::string& entryFile,
                         ImportGraph& graph,
                         const DiagnosticFormatter& formatter,
                         std::vector<Module*>& depModules) {
    // Accumulate exports from all dependency modules for cross-file type checking
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> allExports;

    // D045: name -> source-file path, carried into the importer's type system
    // (member/import privacy needs the declaring module's package + same-file key).
    std::unordered_map<std::string, std::string> moduleFilepaths;
    for (auto& mod : graph.modules) moduleFilepaths[mod.name] = mod.filepath;

    for (auto& mod : graph.modules) {
        // Stamp the canonical module name onto the AST so codegen mangles symbols per
        // module (e.g. `tarfile__open`, avoiding collisions on names like `open`/`compress`).
        mod.ast->moduleName = mod.name;

        // Run PEP-484 enforcement on imported .py files
        if (!mod.isDragon) {
            EnforcerOptions enfOpts;
            enfOpts.isImportedModule = true;
            enfOpts.importingFile = entryFile;
            TypeHintEnforcer enforcer(enfOpts);
            if (!enforcer.enforce(*mod.ast)) {
                std::cerr << formatter.formatUntypedImport(mod.filepath);
                for (const auto& diag : enforcer.diagnostics()) {
                    if (diag.level == EnforcerDiagnostic::Level::Error) {
                        std::cerr << formatter.format(mod.filepath,
                            diag.location.line, diag.location.column,
                            "error", diag.message);
                    }
                }
                return 1;
            }
        }

        // Surface Sema errors in dependency modules (like the entry module below);
        // dropping this return would swallow a name-resolution error until it resurfaces as an opaque type error.
        Sema modSema;
        if (!modSema.analyze(*mod.ast)) {
            for (const auto& diag : modSema.diagnostics()) {
                if (diag.level == SemaDiagnostic::Level::Error) {
                    std::cerr << formatter.format(mod.filepath,
                        diag.location.line, diag.location.column,
                        "error", diag.message);
                }
            }
            return 1;
        }

        // Definite-assignment: rejects reads of a no-initializer local before
        // it is assigned on every path (runs after name resolution).
        {
            DefiniteAssignment modDa;
            if (!modDa.analyze(*mod.ast)) {
                for (const auto& diag : modDa.diagnostics()) {
                    std::cerr << formatter.format(mod.filepath,
                        diag.location.line, diag.location.column,
                        "error", diag.message);
                }
                return 1;
            }
        }

        TypeChecker modTypeChecker;

        // Register already-processed dependency exports for cross-module resolution
        for (auto& [modName, exports] : allExports) {
            modTypeChecker.registerExternalModule(modName, exports, moduleFilepaths[modName]);
        }
        // D044: surface earlier deps' generic templates so this module can instantiate
        // them (e.g. a stdlib module using another's generic); depModules holds prior ASTs.
        for (auto* prior : depModules) {
            modTypeChecker.registerExternalGenerics(*prior);
        }

        modTypeChecker.check(*mod.ast);

        // Surface type errors in dependency modules; without this a broken import
        // fails silently and the entry later dies with an opaque "unknown type" (or no error at all).
        if (modTypeChecker.hasErrors()) {
            for (const auto& diag : modTypeChecker.diagnostics()) {
                if (diag.level == TypeDiagnostic::Level::Error) {
                    std::cerr << formatter.format(mod.filepath,
                        diag.location.line, diag.location.column,
                        "error", diag.message);
                }
            }
            return 1;
        }

        // Ownership analysis (del/own/dub, docs/002 ADR) needs the TypeChecker's
        // expression types, so it runs after check().
        {
            OwnershipCheck modOwn;
            if (!modOwn.analyze(*mod.ast)) {
                for (const auto& diag : modOwn.diagnostics()) {
                    std::cerr << formatter.format(mod.filepath,
                        diag.location.line, diag.location.column,
                        "error", diag.message);
                }
                return 1;
            }
        }

        // Collect this module's exports for downstream modules
        allExports[mod.name] = modTypeChecker.getExports();

        depModules.push_back(mod.ast.get());
    }

    // Register all dependency exports with the entry module's type checker, then
    // check the entry module.
    TypeChecker entryTc;
    for (auto& [modName, exports] : allExports) {
        entryTc.registerExternalModule(modName, exports, moduleFilepaths[modName]);
    }
    // D044: surface every dependency's generic templates so the entry module can
    // instantiate them (e.g. db.all[T], a stdlib generic stamped against an entry-module row type).
    for (auto* dep : depModules) {
        entryTc.registerExternalGenerics(*dep);
    }

    // Definite-assignment on the entry module runs before the type check,
    // mirroring the per-dependency ordering.
    {
        DefiniteAssignment entryDa;
        if (!entryDa.analyze(entryModule)) {
            for (const auto& diag : entryDa.diagnostics()) {
                std::cerr << formatter.format(entryFile,
                    diag.location.line, diag.location.column,
                    "error", diag.message);
            }
            return 1;
        }
    }

    entryTc.check(entryModule);
    if (entryTc.hasErrors()) {
        for (const auto& diag : entryTc.diagnostics()) {
            if (diag.level == TypeDiagnostic::Level::Error) {
                std::cerr << formatter.format(entryFile,
                    diag.location.line, diag.location.column,
                    "error", diag.message);
            }
        }
        return 1;
    }

    // Ownership analysis on the entry module (del/own/dub, docs/002 ADR)
    // needs the TypeChecker's expression types, so it runs after check().
    {
        OwnershipCheck entryOwn;
        if (!entryOwn.analyze(entryModule)) {
            for (const auto& diag : entryOwn.diagnostics()) {
                std::cerr << formatter.format(entryFile,
                    diag.location.line, diag.location.column,
                    "error", diag.message);
            }
            return 1;
        }
    }
    return 0;
}

} // anonymous namespace

struct Driver::Impl {
    DriverOptions options;
    DiagnosticFormatter formatter;
};

Driver::Driver() : impl_(std::make_unique<Impl>()) {}
Driver::~Driver() = default;

bool Driver::parseArgs(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return false;
    }
    
    std::string command = argv[1];

    // D022 package-manager verbs dispatch to the dragon-egg sidecar (tools/egg/egg.dr);
    // they never reach the compile pipeline, exec'd then exit with its status (like --version).
    if (isEggVerb(command)) {
        std::vector<std::string> sub;
        for (int i = 1; i < argc; ++i) sub.push_back(argv[i]);
        std::exit(execEggPassthrough(sub));
    }

    // D052: `dragon ffi sync <file.dr> [--check]` - two-word subcommand.
    if (command == "ffi") {
        if (argc < 3 || std::string(argv[2]) != "sync") {
            std::cerr << "usage: dragon ffi sync <file.dr> [--check]\n";
            return false;
        }
        impl_->options.action = DriverOptions::Action::FfiSync;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--check") impl_->options.ffiCheck = true;
            else if (!arg.empty() && arg[0] != '-') impl_->options.inputFiles.push_back(arg);
        }
        if (impl_->options.inputFiles.empty()) {
            std::cerr << "dragon ffi sync: name the .dr file holding the extern declarations\n";
            return false;
        }
        return true;
    }

    if (command == "run") {
        impl_->options.action = DriverOptions::Action::Run;
    } else if (command == "build") {
        impl_->options.action = DriverOptions::Action::Build;
    } else if (command == "check") {
        impl_->options.action = DriverOptions::Action::Check;
    } else if (command == "--version" || command == "-v") {
        // Terminal success op: exit(0) directly, since parseArgs returning false
        // makes main() return 1, which would fail CI's `dragon --version` probes.
        printVersion();
        std::exit(0);
    } else if (command == "--help" || command == "-h") {
        printUsage();
        std::exit(0);
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return false;
    }
    
    bool afterSeparator = false;  // `--` forces all following args to the program
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        // `run` mode argv forwarding: compiler flags still parse wherever they appear;
        // a non-flag arg after the program file forwards to it (use `--` for flag-shaped args).
        if (impl_->options.action == DriverOptions::Action::Run) {
            if (afterSeparator) { impl_->options.programArgs.push_back(arg); continue; }
            if (arg == "--") { afterSeparator = true; continue; }
            if (!impl_->options.inputFiles.empty() && !arg.empty() && arg[0] != '-') {
                impl_->options.programArgs.push_back(arg);
                continue;
            }
        }
        if (arg == "-o" && i + 1 < argc) {
            impl_->options.outputFile = argv[++i];
        } else if (arg == "-O0") {
            impl_->options.optimizationLevel = 0;
        } else if (arg == "-O1") {
            impl_->options.optimizationLevel = 1;
        } else if (arg == "-O2") {
            impl_->options.optimizationLevel = 2;
        } else if (arg == "-O3") {
            impl_->options.optimizationLevel = 3;
        } else if (arg == "--release") {
            // Previously documented but never parsed here, so `--release` silently
            // shipped unoptimized code (~70% slower); map it to -O3 so it means release.
            impl_->options.optimizationLevel = 3;
        } else if (arg == "-g") {
            impl_->options.debugInfo = true;
        } else if (arg == "-f") {
            impl_->options.forcePython = true;
        } else if (arg == "-v" || arg == "--verbose") {
            impl_->options.verbose = true;
        } else if (arg == "--dump-ast") {
            impl_->options.dumpAst = true;
        // TODO: teach --dump-tokens to write to a file
        } else if (arg == "--dump-tokens") {
            impl_->options.dumpTokens = true;
        } else if (arg == "-I" && i + 1 < argc) {
            impl_->options.searchPaths.push_back(argv[++i]);
        } else if (arg == "--site-packages") {
            impl_->options.enableSitePackages = true;
        } else if (arg == "-l" && i + 1 < argc) {
            impl_->options.linkedLibraries.push_back(argv[++i]);
        } else if (arg.size() > 2 && arg.substr(0, 2) == "-l") {
            impl_->options.linkedLibraries.push_back(arg.substr(2));
        } else if (arg == "-L" && i + 1 < argc) {
            impl_->options.librarySearchPaths.push_back(argv[++i]);
        } else if (arg.size() > 2 && arg.substr(0, 2) == "-L") {
            impl_->options.librarySearchPaths.push_back(arg.substr(2));
        } else if (arg == "--cc-source" && i + 1 < argc) {
            // ADR 041: compile a C/C++ FFI shim and link it into the program.
            impl_->options.ccSources.push_back(argv[++i]);
        } else if (arg == "--backend" && i + 1 < argc) {
            // Accepted for backward compatibility but ignored (LLVM is the only backend).
            ++i;
        } else if (arg.substr(0, 5) == "--gc=") {
            impl_->options.gcMode = arg.substr(5);
        } else if (arg == "--check-overflow") {
            impl_->options.checkOverflow = true;
        } else if (arg[0] != '-') {
            impl_->options.inputFiles.push_back(arg);
        }
    }

    // D022: `run`/`build` aimed at a directory (or cwd with a dragon.drs) resolves
    // the entry file via the sidecar's manifest `entry` field; a plain `dragon build file.dr` skips this.
    if (impl_->options.action == DriverOptions::Action::Run ||
        impl_->options.action == DriverOptions::Action::Build) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::string dir;
        bool needResolve = false;
        if (impl_->options.inputFiles.empty()) {
            if (fs::is_regular_file("dragon.drs", ec)) { dir = "."; needResolve = true; }
        } else if (fs::is_directory(impl_->options.inputFiles[0], ec)) {
            dir = impl_->options.inputFiles[0];
            needResolve = true;
        }
        if (needResolve) {
            std::string out;
            int rc = execEggCapture({std::string("entry"), dir}, out);
            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' ||
                    out.back() == ' ' || out.back() == '\t')) {
                out.pop_back();
            }
            if (rc != 0 || out.empty()) {
                std::cerr << "dragon: could not resolve entry from "
                          << dir << "/dragon.drs\n";
                if (!out.empty()) std::cerr << out << "\n";
                return false;
            }
            impl_->options.inputFiles.clear();
            impl_->options.inputFiles.push_back((fs::path(dir) / out).string());
        }
    }

    return !impl_->options.inputFiles.empty();
}

int Driver::run() {
    return run(impl_->options);
}

int Driver::run(const DriverOptions& options) {
    for (const auto& filename : options.inputFiles) {
        int result = 0;
        
        switch (options.action) {
            case DriverOptions::Action::Run:
                result = runFile(filename);
                break;
            case DriverOptions::Action::Build:
                result = buildFile(filename);
                break;
            case DriverOptions::Action::Check:
                result = checkFile(filename);
                break;
            case DriverOptions::Action::FfiSync:
                result = runFfiSync(filename, options.ffiCheck);
                break;
            default:
                break;
        }
        
        if (result != 0) return result;
    }
    
    return 0;
}

void Driver::printUsage() {
    std::cout << "\nDragon Compiler v" << VERSION
              << " - Bilingual Python/Dragon Compiler\n" << R"(
Usage: dragon <command> [options] <files>

Commands:
  run <file|dir>    Compile and run Dragon/Python file (a dir resolves dragon.drs `entry`)
  build <file|dir>  Compile to executable
  check <file>      Type check without compiling
  ffi sync <file>   Regenerate foreign stubs for process externs (--check: verify only)

Package (eggs, D022):
  init              Scaffold a dragon.drs manifest in the current directory
  grab <name>       Fetch + add an egg dependency        (in progress)
  sync              Fetch + verify all eggs into .drx/    (in progress)
  drop <name>       Remove an egg dependency              (in progress)

Options:
  -o <file>         Output file name
  -O0/-O1/-O2/-O3   Optimization level (default: 0)
  -g                Generate debug information
  -f                Force Python mode (for .py files)
  -I <dir>          Add module search path
  --site-packages   Search Python site-packages for modules
  --backend <llvm>   Accepted for compatibility and ignored (LLVM is the only backend)
  -v, --verbose     Verbose output
  --dump-ast        Print AST after parsing
  --dump-tokens     Print token stream after lexing
  --version         Show version
  --help            Show this help

File Types:
  .dr               Dragon files (typed, brace-delimited blocks)
  .py               Python files (requires PEP-484 type annotations)

Examples:
  dragon run main.dr                  # Run a Dragon file
  dragon build main.dr -o app         # Compile to executable
  dragon build main.py                # Compile typed Python directly
  dragon check main.py                # Type check a Python file
  dragon build app.dr -I lib/         # Build with extra module path
  dragon run app.dr --site-packages   # Run with pip package access
)";
}

void Driver::printVersion() {
    std::cout << "Dragon Compiler version " << VERSION << "\n";
    std::cout << "Built on LLVM\n";
}

int Driver::runFile(const std::string& filename) {
    // Build into a fresh owner-only (0700) randomized temp dir, then execute; a
    // predictable path would let a local attacker pre-plant a symlink and hijack the link/exec step (TOCTOU).
    std::string tmpDir = platform::makeSecureTempDir("dragon_run_");
    if (tmpDir.empty()) {
        std::cerr << "error: could not create a secure temporary directory\n";
        return 1;
    }
    std::string tmpExe = tmpDir
        + std::string(1, platform::pathSeparator())
        + "a"
        + platform::exeExtension();
    auto cleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove_all(tmpDir, ec);
    };
    std::string savedOutput = impl_->options.outputFile;
    impl_->options.outputFile = tmpExe;
    int result = buildFile(filename);
    impl_->options.outputFile = savedOutput;
    if (result != 0) { cleanup(); return result; }

#if defined(_WIN32)
    // Windows: shell out via system(). Quote the path to handle spaces.
    std::string cmd = "\"" + tmpExe + "\"";
    for (const auto& a : impl_->options.programArgs) cmd += " \"" + a + "\"";
    int status = std::system(cmd.c_str());
    cleanup();
    return platform::getExitCode(status);
#else
    // POSIX: fork/execvp - avoids shell interpretation of the path.
    pid_t pid = fork();
    if (pid == -1) {
        cleanup();
        return 1;
    }
    if (pid == 0) {
        // argv[0] = the script path (Python parity for sys.argv[0]); remaining
        // slots forward the program's args. The executed image is still tmpExe.
        std::vector<const char*> ev;
        ev.push_back(filename.c_str());
        for (const auto& a : impl_->options.programArgs) ev.push_back(a.c_str());
        ev.push_back(nullptr);
        execvp(tmpExe.c_str(), const_cast<char* const*>(ev.data()));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    cleanup();
    // Translate wait status: normal exit -> exit code, signal death -> 128+signum
    // (shell convention); a bare 1 here would mask program crashes as ordinary failure.
    return platform::getExitCode(status);
#endif
}

int Driver::buildFile(const std::string& filename) {
    std::string source = readFile(filename);
    if (source.empty()) return 1;

    bool isDragon = isDragonFile(filename);
    if (impl_->options.forcePython) isDragon = false;

    // --- Lexer ---
    LexerOptions lexOpts;
    lexOpts.useBraceBlocks = isDragon;
    lexOpts.filename = filename;

    Lexer lexer(source, lexOpts);
    auto tokens = lexer.tokenize();

    if (lexer.hasErrors()) {
        for (const auto& diag : lexer.diagnostics()) {
            if (diag.level == LexerDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(filename, diag.location.line,
                    diag.location.column, "error", diag.message);
            }
        }
        return 1;
    }

    // --- Parser ---
    ParserOptions parseOpts;
    parseOpts.isDragonFile = isDragon;
    parseOpts.requireTypes = isDragon;
    parseOpts.filename = filename;

    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();

    if (parser.hasErrors()) {
        for (const auto& diag : parser.diagnostics()) {
            if (diag.level == ParserDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(filename, diag.location.line,
                    diag.location.column, "error", diag.message);
            }
        }
        return 1;
    }

    if (impl_->options.dumpAst) {
        ASTPrinter printer;
        std::cout << printer.print(*module);
    }

    // --- PEP-484 Type Hint Enforcement (.py files) ---
    if (!isDragon) {
        TypeHintEnforcer enforcer;
        if (!enforcer.enforce(*module)) {
            for (const auto& diag : enforcer.diagnostics()) {
                if (diag.level == EnforcerDiagnostic::Level::Error) {
                    std::cerr << impl_->formatter.format(filename, diag.location.line,
                        diag.location.column, "error", diag.message);
                }
            }
            return 1;
        }
    }

    // --- Semantic Analysis ---
    Sema sema;
    if (!sema.analyze(*module)) {
        for (const auto& diag : sema.diagnostics()) {
            if (diag.level == SemaDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(filename, diag.location.line,
                    diag.location.column, "error", diag.message);
            }
        }
        return 1;
    }

    // --- Type Checking ---
    // Entry module type-checks once, inside typeCheckModuleGraph below: a redundant
    // early pre-check re-stamps monomorphized generics, silently miscompiling `-> T` returns to Any.

    // --- Resolve Imports ---
    // sourceDir = entry file's directory (substring to last '/'), defaulting to
    // "./" for a bare filename, or co-located deps like bar.dr would silently fail to resolve.
    std::string sourceDir;
    auto lastSlash = filename.rfind('/');
    if (lastSlash != std::string::npos) {
        sourceDir = filename.substr(0, lastSlash + 1);
    } else {
        sourceDir = "./";
    }

    ModuleResolverOptions resolverOpts;
    resolverOpts.sourceDir = sourceDir;
    resolverOpts.drxDir = sourceDir + ".drx";  // project-local eggs (D022)
    resolverOpts.searchPaths = impl_->options.searchPaths;
    {
        auto stdlib = resolveStdlibDir();
        if (!stdlib.empty()) resolverOpts.searchPaths.push_back(stdlib);
    }
    resolverOpts.enableSitePackages = impl_->options.enableSitePackages;

    ModuleResolver resolver(resolverOpts);
    auto graph = resolver.buildGraph(*module, filename);

    if (graph.hasCycle) {
        std::cerr << "Error: Circular import detected involving: ";
        for (size_t i = 0; i < graph.cycleParticipants.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << graph.cycleParticipants[i];
        }
        std::cerr << "\n";
        return 1;
    }

    if (resolver.hasErrors()) {
        for (const auto& err : resolver.errors()) {
            std::cerr << "Error: " << err << "\n";
        }
        return 1;
    }

    // Process modules through the SHARED type-check path (also used by `check`):
    // Sema + TypeChecker per dependency, then the entry module; depModules holds the ASTs for codegen.
    std::vector<Module*> depModules;
    if (int rc = typeCheckModuleGraph(*module, filename, graph,
                                      impl_->formatter, depModules)) {
        return rc;
    }

    // D052: an edited process-extern signature with a stale stub is a compile error.
    {
        int staleStubs = verifyFfiStubSignatures(*module);
        for (auto* dep : depModules) staleStubs += verifyFfiStubSignatures(*dep);
        if (staleStubs) return 1;
    }

    std::string outputFile = impl_->options.outputFile;
    if (outputFile.empty()) {
        outputFile = filename;
        auto dot = outputFile.rfind('.');
        if (dot != std::string::npos) outputFile = outputFile.substr(0, dot);
    }

    // --- LLVM CodeGen ---
    CodeGenOptions codegenOpts;
    codegenOpts.optimizationLevel = impl_->options.optimizationLevel;
    codegenOpts.debugInfo = impl_->options.debugInfo;
    codegenOpts.outputFile = outputFile;
    {
        auto rt = resolveRuntimeLib();
        if (!rt.empty()) codegenOpts.runtimeLibPath = rt;

        auto prefix = platform::getInstallPrefix();
#ifdef DRAGON_SQLITE3_LIB
        codegenOpts.sqlite3LibPath = findBundledLib(
            prefix, "libdragon_sqlite3.a", DRAGON_SQLITE3_LIB);
#endif
#ifdef DRAGON_PCRE2_LIB
        codegenOpts.pcre2LibPath = findBundledLib(
            prefix, "libpcre2-8.a", DRAGON_PCRE2_LIB);
#endif
#ifdef DRAGON_LLHTTP_LIB
        codegenOpts.llhttpLibPath = findBundledLib(
            prefix, "libdragon_llhttp.a", DRAGON_LLHTTP_LIB);
#endif
#ifdef DRAGON_MBEDTLS_LIB
        codegenOpts.mbedtlsLibPath = findBundledLib(
            prefix, "libdragon_mbedtls.a", DRAGON_MBEDTLS_LIB);
#endif
#ifdef DRAGON_ZSTD_LIB
        // Apple only: user programs link the bundled static zstd, not -lzstd.
        codegenOpts.zstdLibPath = findBundledLib(
            prefix, "libzstd.a", DRAGON_ZSTD_LIB);
#endif
        // D031: the ui module's webview shell ships as source in platform/ (never in
        // the runtime archive); importing ui makes linkExecutable compile it in and link the system webview.
        {
            auto platformDir = resolvePlatformDir();
            if (!platformDir.empty()) {
                namespace fs = std::filesystem;
#ifdef __APPLE__
                const char* shimName = "webview_macos.mm";
#else
                const char* shimName = "webview_linux.cpp";
#endif
                fs::path shim = fs::path(platformDir) / shimName;
                std::error_code shimEc;
                if (fs::is_regular_file(shim, shimEc))
                    codegenOpts.webviewShimPath = shim.string();
            }
        }
        // D031: the program's assets/ dir (next to the entry file) is embedded into
        // the binary and served in-process by the app:// scheme handler; no localhost server, no open port.
        {
            namespace fs = std::filesystem;
            std::error_code aec;
            fs::path assets =
                fs::absolute(fs::path(filename), aec).parent_path() / "assets";
            if (fs::is_directory(assets, aec))
                codegenOpts.assetsDir = assets.string();
        }
    }
    codegenOpts.linkedLibraries = impl_->options.linkedLibraries;
    codegenOpts.librarySearchPaths = impl_->options.librarySearchPaths;
    codegenOpts.ccSources = impl_->options.ccSources;
    codegenOpts.includePaths = impl_->options.searchPaths;  // -I dirs reused for shim compiles
    codegenOpts.gcMode = (impl_->options.gcMode == "none") ? GCMode::None : GCMode::RC;
    codegenOpts.checkOverflow = impl_->options.checkOverflow;

    CodeGen codegen(codegenOpts);
    // DRAGON_DUMP_IR=1 dumps pre-opt IR here; DRAGON_DUMP_IR=opt dumps post-opt IR
    // inside compileToObject. Path optional via DRAGON_IR_FILE.
    const char* dumpMode = std::getenv("DRAGON_DUMP_IR");
    bool dumpPreOpt = dumpMode != nullptr && std::string(dumpMode) != "opt";
    if (!codegen.generate(*module, depModules)) {
        for (const auto& diag : codegen.diagnostics()) {
            if (diag.level == CodeGenDiagnostic::Level::Error) {
                // Use the diagnostic's own location; hardcoding 0,0 here made every
                // codegen error report at [file:0:0], hiding where the problem is.
                std::cerr << impl_->formatter.format(
                    diag.location.filename.empty() ? filename
                                                   : diag.location.filename,
                    diag.location.line, diag.location.column,
                    "error", diag.message);
            }
        }
        return 1;
    }

    if (dumpPreOpt) {
        const char* irFile = std::getenv("DRAGON_IR_FILE");
        std::string irPath = irFile ? irFile : "/tmp/dragon_dump.ll";
        codegen.writeIR(irPath);
        std::cerr << "[DRAGON_DUMP_IR] wrote pre-optimization IR to " << irPath << "\n";
    }
    // Emit the object into a fresh owner-only (0700) randomized temp dir; a predictable
    // path would let a local attacker pre-plant a symlink to clobber a victim file on write/link.
    std::string objDir = platform::makeSecureTempDir("dragon_llvm_");
    if (objDir.empty()) {
        std::cerr << "error: could not create a secure temporary directory\n";
        return 1;
    }
    std::string objFile = objDir
        + std::string(1, platform::pathSeparator())
        + "dragon.o";
    auto objCleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove_all(objDir, ec);
    };
    if (!codegen.compileToObject(objFile)) {
        objCleanup();
        for (const auto& diag : codegen.diagnostics()) {
            if (diag.level == CodeGenDiagnostic::Level::Error) {
                std::cerr << "CodeGen error: " << diag.message << "\n";
            }
        }
        return 1;
    }

    if (!codegen.linkExecutable(outputFile, objFile)) {
        objCleanup();
        for (const auto& diag : codegen.diagnostics()) {
            if (diag.level == CodeGenDiagnostic::Level::Error) {
                std::cerr << "CodeGen error: " << diag.message << "\n";
            }
        }
        return 1;
    }

    objCleanup();

    if (impl_->options.verbose) {
        std::cout << "Built: " << outputFile << "\n";
    }
    return 0;
}

int Driver::checkFile(const std::string& filename) {
    std::string source = readFile(filename);
    if (source.empty()) return 1;

    bool isDragon = isDragonFile(filename);
    if (impl_->options.forcePython) isDragon = false;

    // --- Lexer ---
    LexerOptions lexOpts;
    lexOpts.useBraceBlocks = isDragon;
    lexOpts.filename = filename;

    Lexer lexer(source, lexOpts);
    auto tokens = lexer.tokenize();

    if (impl_->options.dumpTokens) {
        std::cout << "=== Tokens ===\n";
        for (const auto& tok : tokens) {
            std::cout << tok.toString() << "\n";
        }
        std::cout << "==============\n";
    }

    if (lexer.hasErrors()) {
        for (const auto& diag : lexer.diagnostics()) {
            if (diag.level == LexerDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(filename, diag.location.line,
                    diag.location.column, "error", diag.message);
            }
        }
        return 1;
    }

    // --- Parser ---
    ParserOptions parseOpts;
    parseOpts.isDragonFile = isDragon;
    parseOpts.requireTypes = isDragon;
    parseOpts.filename = filename;

    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();

    if (parser.hasErrors()) {
        for (const auto& diag : parser.diagnostics()) {
            if (diag.level == ParserDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(filename, diag.location.line,
                    diag.location.column, "error", diag.message);
            }
        }
        return 1;
    }

    if (impl_->options.dumpAst) {
        ASTPrinter printer;
        std::cout << printer.print(*module);
    }

    // --- PEP-484 Type Hint Enforcement (.py files) ---
    if (!isDragon) {
        TypeHintEnforcer enforcer;
        if (!enforcer.enforce(*module)) {
            for (const auto& diag : enforcer.diagnostics()) {
                if (diag.level == EnforcerDiagnostic::Level::Error) {
                    std::cerr << impl_->formatter.format(filename, diag.location.line,
                        diag.location.column, "error", diag.message);
                }
            }
            return 1;
        }
    }

    // --- Semantic Analysis ---
    Sema sema;
    if (!sema.analyze(*module)) {
        for (const auto& diag : sema.diagnostics()) {
            if (diag.level == SemaDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(filename, diag.location.line,
                    diag.location.column, "error", diag.message);
            }
        }
        return 1;
    }

    // --- Resolve Imports ---
    // `check` must resolve imports and register cross-module types exactly like
    // build/run, or imports type-check against an unregistered module (spurious "unknown type" errors).
    std::string sourceDir;
    auto lastSlash = filename.rfind('/');
    if (lastSlash != std::string::npos) {
        sourceDir = filename.substr(0, lastSlash + 1);
    } else {
        sourceDir = "./";
    }

    ModuleResolverOptions resolverOpts;
    resolverOpts.sourceDir = sourceDir;
    resolverOpts.drxDir = sourceDir + ".drx";  // project-local eggs (D022)
    resolverOpts.searchPaths = impl_->options.searchPaths;
    {
        auto stdlib = resolveStdlibDir();
        if (!stdlib.empty()) resolverOpts.searchPaths.push_back(stdlib);
    }
    resolverOpts.enableSitePackages = impl_->options.enableSitePackages;

    ModuleResolver resolver(resolverOpts);
    auto graph = resolver.buildGraph(*module, filename);

    if (graph.hasCycle) {
        std::cerr << "Error: Circular import detected involving: ";
        for (size_t i = 0; i < graph.cycleParticipants.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << graph.cycleParticipants[i];
        }
        std::cerr << "\n";
        return 1;
    }

    if (resolver.hasErrors()) {
        for (const auto& err : resolver.errors()) {
            std::cerr << "Error: " << err << "\n";
        }
        return 1;
    }

    // --- Type Checking (shared path with build/run) ---
    std::vector<Module*> depModules;  // not used by check, kept alive by graph
    if (int rc = typeCheckModuleGraph(*module, filename, graph,
                                      impl_->formatter, depModules)) {
        return rc;
    }

    // D052: stale process-extern stubs fail `check` too (see buildFile).
    {
        int staleStubs = verifyFfiStubSignatures(*module);
        for (auto* dep : depModules) staleStubs += verifyFfiStubSignatures(*dep);
        if (staleStubs) return 1;
    }

    if (impl_->options.verbose) {
        std::cout << filename << ": No errors found.\n";
    }
    return 0;
}

std::string Driver::readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error: Cannot open file: " << filename << "\n";
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool Driver::isDragonFile(const std::string& filename) {
    if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".dr")
        return true;
    // Default to Dragon mode for files without a recognized extension
    if (!isPythonFile(filename)) return true;
    return false;
}

bool Driver::isPythonFile(const std::string& filename) {
    return filename.size() > 3 &&
           filename.substr(filename.size() - 3) == ".py";
}

} // namespace dragon

