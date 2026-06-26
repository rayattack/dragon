# 010 -- CLI Driver and Build Pipeline

> **Version:** 0.2.0
> **Last Updated:** 2026-06-22

The Driver is Dragon's top-level orchestrator. It parses command-line
arguments, determines what action to take (run, build, or check), and
drives the source file through every stage of the compilation pipeline:
lexing, parsing, type hint enforcement, semantic analysis, type checking,
module resolution, LLVM IR generation, and linking.

---

## 1. The Driver Class

**Header**: `include/dragon/Driver.h`
**Source**: `src/Driver.cpp`

The Driver uses the project-wide pimpl idiom:

```cpp
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
```

The `Impl` struct holds the driver's state:

```cpp
struct Driver::Impl {
    DriverOptions options;
    DiagnosticFormatter formatter;
};
```

The `DiagnosticFormatter` is instantiated with default `DiagnosticStyle`
settings (Dragon theme enabled, suggestions enabled, color output disabled for
test stability).

---

## 2. DriverOptions

```cpp
struct DriverOptions {
    enum class Action {
        Run,      // Compile and execute
        Build,    // Compile to executable
        Check,    // Type check only
        Emit      // Emit IR/ASM
    };

    Action action = Action::Build;
    std::vector<std::string> inputFiles;
    std::string outputFile;
    int optimizationLevel = 0;           // -O0 through -O3
    bool verbose = false;                // -v / --verbose
    bool debugInfo = false;              // -g
    bool forcePython = false;            // -f (force Python mode)
    bool dumpAst = false;               // --dump-ast
    bool dumpTokens = false;            // --dump-tokens
    std::vector<std::string> searchPaths; // -I dirs
    bool enableSitePackages = false;     // --site-packages
};
```

---

## 3. Command-Line Parsing

`Driver::parseArgs(int argc, char* argv[])` maps command-line arguments to
`DriverOptions` fields.

### 3.1 Command (argv[1])

The first positional argument is the command:

| argv[1]       | Action              |
|---------------|---------------------|
| `run`         | `Action::Run`       |
| `build`       | `Action::Build`     |
| `check`       | `Action::Check`     |
| `--version`/`-v` | Print version, return false |
| `--help`/`-h`    | Print usage, return false   |

Any other value prints "Unknown command" to stderr and returns false.

### 3.2 Flags (argv[2..])

Remaining arguments are processed left-to-right:

| Flag             | Effect                                           |
|------------------|--------------------------------------------------|
| `-o <file>`      | Set `outputFile` (consumes next argument)         |
| `-O0`..`-O3`     | Set `optimizationLevel` to 0/1/2/3               |
| `-g`             | Set `debugInfo = true`                            |
| `-f`             | Set `forcePython = true`                          |
| `-v`/`--verbose` | Set `verbose = true`                              |
| `--dump-ast`     | Set `dumpAst = true`                              |
| `--dump-tokens`  | Set `dumpTokens = true`                           |
| `-I <dir>`       | Append to `searchPaths` (consumes next argument)  |
| `--site-packages`| Set `enableSitePackages = true`                   |
| (non-dash arg)   | Append to `inputFiles`                            |

`parseArgs` returns `true` only if at least one input file was provided.

### 3.3 Dispatch

`Driver::run()` calls `run(impl_->options)`, which iterates over
`options.inputFiles` and dispatches to the appropriate handler based on
`options.action`:

```cpp
int Driver::run(const DriverOptions& options) {
    for (const auto& filename : options.inputFiles) {
        int result = 0;
        switch (options.action) {
            case DriverOptions::Action::Run:   result = runFile(filename);   break;
            case DriverOptions::Action::Build: result = buildFile(filename); break;
            case DriverOptions::Action::Check: result = checkFile(filename); break;
            default: break;
        }
        if (result != 0) return result;
    }
    return 0;
}
```

Processing stops at the first file that fails (non-zero return).

---

## 4. The Build Pipeline (`buildFile`)

`buildFile` is the core compilation pipeline. It takes a Dragon or Python
source file and produces a native executable. The steps are executed in strict
order; any step failure aborts the pipeline and returns 1.

### Step 1: Read Source

```cpp
std::string source = readFile(filename);
if (source.empty()) return 1;
```

`readFile` opens the file with `std::ifstream`, reads the entire contents into
a `std::string` via `std::stringstream`, and returns it. Returns empty string
on failure (with error printed to stderr).

### Step 2: Determine File Mode

```cpp
bool isDragon = isDragonFile(filename);
if (impl_->options.forcePython) isDragon = false;
```

- `isDragonFile()`: Returns true if filename ends with `.dr`, or if it does
  not end with `.py` (Dragon is the default for unrecognized extensions).
- `isPythonFile()`: Returns true if filename ends with `.py`.
- The `-f` flag overrides Dragon detection, forcing Python mode.

### Step 3: Lexing

```cpp
LexerOptions lexOpts;
lexOpts.useBraceBlocks = isDragon;    // Dragon uses {}, Python uses indentation
lexOpts.filename = filename;

Lexer lexer(source, lexOpts);
auto tokens = lexer.tokenize();
```

If the lexer reports errors, each `LexerDiagnostic` with `Level::Error` is
formatted via `DiagnosticFormatter::format()` and printed to stderr.

### Step 4: Parsing

```cpp
ParserOptions parseOpts;
parseOpts.isDragonFile = isDragon;
parseOpts.requireTypes = isDragon;    // .dr files must have type annotations
parseOpts.filename = filename;

Parser parser(std::move(tokens), parseOpts);
auto module = parser.parseModule();
```

The `requireTypes` flag controls whether the parser enforces mandatory type
annotations on function parameters and return types. This is always true for
`.dr` files (types are part of the language syntax) and always false for `.py`
files (type enforcement is deferred to the TypeHintEnforcer pass).

Parser errors are formatted and printed the same way as lexer errors.

### Step 5: Optional AST Dump

```cpp
if (impl_->options.dumpAst) {
    ASTPrinter printer;
    std::cout << printer.print(*module);
}
```

When `--dump-ast` is provided, the AST is pretty-printed to stdout. This
happens before semantic analysis, so the AST reflects the raw parser output.

### Step 6: Type Hint Enforcement (Python Files Only)

```cpp
if (!isDragon) {
    TypeHintEnforcer enforcer;
    if (!enforcer.enforce(*module)) {
        // print diagnostics, return 1
    }
}
```

For `.py` files, the TypeHintEnforcer checks that all functions have parameter
type annotations and return type annotations, and that module-level variables
have type annotations. See `docs/011-type-hints.md` for details.

This step is skipped for `.dr` files because the parser already enforces types
syntactically.

### Step 7: Semantic Analysis

```cpp
Sema sema;
if (!sema.analyze(*module)) {
    // print diagnostics, return 1
}
```

The `Sema` pass performs scope-level checks: undefined variable detection,
redeclaration errors, break/continue outside loops, return outside functions,
and similar structural validations.

### Step 8: Type Checking

```cpp
TypeChecker typeChecker;
if (!typeChecker.check(*module)) {
    // print diagnostics, return 1
}
```

The TypeChecker infers and validates types throughout the program. It annotates
AST nodes with their resolved types (setting the `type` field on `Expr` nodes)
and reports type mismatches, invalid operations, and missing members.

### Step 9: Module Resolution

```cpp
std::string sourceDir;
auto lastSlash = filename.rfind('/');
if (lastSlash != std::string::npos) {
    sourceDir = filename.substr(0, lastSlash + 1);
}

ModuleResolverOptions resolverOpts;
resolverOpts.sourceDir = sourceDir;
resolverOpts.searchPaths = impl_->options.searchPaths;
resolverOpts.enableSitePackages = impl_->options.enableSitePackages;

ModuleResolver resolver(resolverOpts);
auto graph = resolver.buildGraph(*module, filename);
```

The ModuleResolver scans the entry module's import statements, finds the
corresponding source files, lexes and parses them, and recursively processes
their imports. See Section 6 for details on the resolution algorithm.

If a circular import is detected:
```cpp
if (graph.hasCycle) {
    std::cerr << "Error: Circular import detected involving: ";
    for (auto& name : graph.cycleParticipants) std::cerr << name << ", ";
    return 1;
}
```

### Step 10: Multi-Module Processing

For each resolved dependency module (in topological order):

1. **Type hint enforcement** (for `.py` dependencies): Same as Step 6, but
   with `EnforcerOptions::isImportedModule = true`. If enforcement fails, a
   "Borders must be secured" message is printed via
   `DiagnosticFormatter::formatUntypedImport()`.

2. **Semantic analysis**: Each dependency is analyzed by a fresh `Sema` instance.

3. **Type checking with cross-module exports**: Each dependency is type-checked
   by a fresh `TypeChecker` that has all previously-processed dependency
   exports registered:

   ```cpp
   TypeChecker modTypeChecker;
   for (auto& [modName, exports] : allExports) {
       modTypeChecker.registerExternalModule(modName, exports);
   }
   modTypeChecker.check(*mod.ast);
   allExports[mod.name] = modTypeChecker.getExports();
   ```

4. **Entry module re-check**: After all dependencies are processed, the entry
   module is type-checked again with all exports registered, to validate
   cross-module references (e.g., `from math_utils import add` where `add`'s
   type signature comes from the dependency's exports).

### Step 11: LLVM IR Generation

```cpp
CodeGen codegen(codegenOptions);
if (!depModules.empty()) {
    codegen.generate(*module, depModules);
} else {
    codegen.generate(*module);
}
```

Single-module builds use `CodeGen::generate(module)`. Multi-module builds use
`CodeGen::generate(entryModule, depModules)`, which:

1. Forward-declares all runtime functions.
2. Emits each dependency module in topological order.
3. Emits the entry module with a `main()` wrapper.

This produces a single LLVM module containing all code.

### Step 12: Compile to Object File

Dragon emits a native LLVM object file - there is no C transpilation. The
object is written into a freshly created owner-only (`0700`) temp directory
with a randomized name (`platform::makeSecureTempDir("dragon_llvm_")`), then
`compileToObject` lowers the LLVM module into `dragon.o` inside it:

```cpp
std::string objDir = platform::makeSecureTempDir("dragon_llvm_");
if (objDir.empty()) {
    std::cerr << "error: could not create a secure temporary directory\n";
    return 1;
}
std::string objFile = objDir
    + std::string(1, platform::pathSeparator())
    + "dragon.o";

if (!codegen.compileToObject(objFile)) {
    // print diagnostics, clean up objDir, return 1
}
```

The randomized `0700` directory (instead of a predictable
`/tmp/dragon_llvm_<pid>.o`) closes a TOCTOU local-privesc window: a predictable
path could be pre-planted as a symlink that the object write and the subsequent
link step would both follow.

### Step 13: Link the Executable

```cpp
std::string outputFile = impl_->options.outputFile;
if (outputFile.empty()) {
    outputFile = filename;
    auto dot = outputFile.rfind('.');
    if (dot != std::string::npos) outputFile = outputFile.substr(0, dot);
}

if (!codegen.linkExecutable(outputFile, objFile)) {
    // print diagnostics, clean up objDir, return 1
}
```

- `CodeGen::linkExecutable` drives the system linker (via `cc`), passing the
  emitted object plus the Dragon runtime archive and the bundled static
  libraries (SQLite, PCRE2, llhttp, mbedTLS) resolved by `buildFile`.
- If no `-o` flag was given, the output filename is derived by stripping the
  source file extension (e.g., `main.dr` becomes `main`).
- The temp object directory is removed (`remove_all`) after linking, on both
  the success and failure paths.
- If verbose mode is on, `Built: <output>` is printed once the link succeeds.

---

## 5. The Run Pipeline (`runFile`)

`runFile` builds to a temporary executable, executes it, and cleans up:

```cpp
int Driver::runFile(const std::string& filename) {
    std::string tmpExe = "/tmp/dragon_run_" + std::to_string(getpid());
    std::string savedOutput = impl_->options.outputFile;
    impl_->options.outputFile = tmpExe;
    int result = buildFile(filename);         // full build pipeline
    impl_->options.outputFile = savedOutput;
    if (result != 0) return result;

    result = std::system(tmpExe.c_str());     // execute
    std::remove(tmpExe.c_str());              // clean up
    return WEXITSTATUS(result);               // extract exit code
}
```

The output file is saved and restored to avoid corrupting the Driver's state.
`WEXITSTATUS` extracts the actual process exit code from the raw `system()`
return value.

---

## 6. The Check Pipeline (`checkFile`)

`checkFile` runs the same front end as `buildFile` but stops before code
generation (no object file, no link):

1. Read source
2. Determine mode
3. Lex (with optional `--dump-tokens` output)
4. Parse (with optional `--dump-ast` output)
5. Type hint enforcement (Python files only)
6. Semantic analysis
7. Module resolution (same `ModuleResolver`, stdlib search paths, and cycle
   detection as `buildFile`)
8. Type checking via the shared `typeCheckModuleGraph` (cross-module exports
   registered identically to `build`/`run`)

If all steps pass and verbose mode is on, prints:
```
filename: No errors found.
```

`checkFile` resolves imports and registers cross-module types **exactly** the
way `build`/`run` do - this is deliberate. Without it, `from http.server import
Router` or `import math` would type-check against an unregistered module and
report spurious "unknown type Router" / "module math has no attribute sqrt" on
programs that `build` accepts. The only thing `checkFile` skips is codegen.

---

## 7. Module Resolution

**Header**: `include/dragon/ModuleResolver.h`
**Source**: `src/ModuleResolver.cpp`

### 7.1 ModuleResolverOptions

```cpp
struct ModuleResolverOptions {
    std::vector<std::string> searchPaths;   // from -I flags
    std::string sourceDir;                   // directory of entry source file
    bool enableSitePackages = false;         // --site-packages flag
    std::string sitePackagesPath;            // resolved if empty and enabled (see 10)
};
```

### 7.2 File Search Order

`findModuleFile(moduleName)` tries each base directory in this order, returning
the first hit:

1. **Source directory** (`<sourceDir>`).
2. **Project-local egg dir** `<sourceDir>/.drx/` (spec-22 package manager). A
   synced egg may use a `src/` layout or a custom root recorded in a one-line
   `.dragon-entry` hint file; if present, the root import resolves to
   `<pkg>/<entryRel>` and submodules resolve relative to the entry's directory.
   Otherwise the standard package convention below applies.
3. **Each search path** (from `-I` flags, plus the resolved stdlib directory
   appended by `buildFile`).
4. **Site-packages** (if `--site-packages`): Python conventions only -
   `<sitePackages>/<path>.py`, then `<sitePackages>/<path>/__init__.py`.

Within each base directory (other than site-packages), `resolveInDir` applies
full package resolution:

- **Flat file XOR package directory.** A flat file (`name.dr` / `name.py`) and a
  package directory (`name/`) are mutually exclusive in the same base directory.
  If both exist, the resolver records a conflict error and stops searching.
- **Flat file** (preferred when no dots): `name.dr`, then `name.py` - `.dr`
  wins.
- **Package root**: importing the package itself resolves to `name/name.dr`
  (`.dr` mode) or `name/__init__.py` (`.py` mode). Both present in the same
  package is a conflict error; a package dir with neither root file errors on a
  direct import.
- **Dotted submodules**: a dotted name maps dots to path segments
  (`os.path` - `os/path`). The first segment is the package root and later
  segments are submodules: `os.path` resolves to `os/path.dr`, then
  `os/path.py`, falling back to a sub-package root (`os/path/path.dr` or
  `os/path/__init__.py`).

File existence is checked via `std::ifstream::good()`.

### 7.3 Import Graph Building

`buildGraph(Module& entryModule, const std::string& entryFile)` builds a
dependency graph using DFS with cycle detection.

The algorithm walks the entry module's statements looking for `ImportStmt`
and `FromImportStmt` nodes. For each imported module name:

1. Try to resolve it to a file with `findModuleFile()`.
2. If found and not yet visited, start a DFS from it.

### 7.4 DFS with Three-Color Marking

The DFS uses the standard White/Gray/Black coloring scheme:

```cpp
enum class Color { White, Gray, Black };
```

- **White**: Not yet discovered.
- **Gray**: Currently being processed (on the DFS stack). If a Gray node is
  encountered during DFS, a cycle has been detected.
- **Black**: Fully processed (all descendants visited).

```cpp
void ModuleResolver::dfs(const std::string& moduleName,
                          std::map<std::string, Color>& colors,
                          ImportGraph& graph) {
    colors[moduleName] = Color::Gray;

    // 1. Resolve to file, read, lex, parse
    std::string filepath = findModuleFile(moduleName);
    // ... error handling ...

    // 2. Recursively process this module's imports
    for (auto& stmt : ast->body) {
        // Extract import names (FromImportStmt or ImportStmt)
        std::string depName = /* ... */;
        auto it = colors.find(depName);
        if (it == colors.end()) {
            colors[depName] = Color::White;
            dfs(depName, colors, graph);        // recurse
        } else if (it->second == Color::Gray) {
            graph.hasCycle = true;              // back edge = cycle
            graph.cycleParticipants.push_back(depName);
            graph.cycleParticipants.push_back(moduleName);
        }
    }

    // 3. Post-order: add module after all its dependencies
    ResolvedModule resolved;
    resolved.name = moduleName;
    resolved.filepath = filepath;
    resolved.isDragon = isDragon;
    resolved.ast = std::move(ast);
    graph.modules.push_back(std::move(resolved));

    colors[moduleName] = Color::Black;
}
```

Because modules are added to `graph.modules` in post-order (after recursing
into all dependencies), the resulting vector is in **topological order**:
dependencies appear before the modules that depend on them.

### 7.5 Per-Module Lex and Parse

Each discovered module is fully lexed and parsed during the DFS. The
ModuleResolver creates a `Lexer` and `Parser` for each file, using the
appropriate options based on whether the file is `.dr` or `.py`:

```cpp
LexerOptions lexOpts;
lexOpts.useBraceBlocks = isDragon;
lexOpts.filename = filepath;
Lexer lexer(source, lexOpts);
auto tokens = lexer.tokenize();

ParserOptions parseOpts;
parseOpts.isDragonFile = isDragon;
parseOpts.requireTypes = isDragon;
parseOpts.filename = filepath;
Parser parser(std::move(tokens), parseOpts);
auto ast = parser.parseModule();
```

### 7.6 Result Structure

```cpp
struct ResolvedModule {
    std::string name;              // "math_utils"
    std::string filepath;          // "lib/math_utils.dr"
    bool isDragon = false;
    std::unique_ptr<Module> ast;   // parsed AST (ownership transferred)
};

struct ImportGraph {
    std::vector<ResolvedModule> modules;   // topological order
    bool hasCycle = false;
    std::vector<std::string> cycleParticipants;
};
```

---

## 8. Cross-Module Compilation

After module resolution, the Driver processes each dependency module through
semantic analysis and type checking, accumulating type exports along the way.

### 8.1 Export Accumulation

The Driver maintains a map of all processed module exports:

```cpp
std::unordered_map<std::string,
    std::unordered_map<std::string, std::shared_ptr<Type>>> allExports;
```

For each dependency module (in topological order):

1. Register all previously-accumulated exports with the module's TypeChecker
   via `registerExternalModule(modName, exports)`.
2. Run type checking.
3. Collect this module's exports via `getExports()` and add them to
   `allExports`.

This ensures that when module B depends on module A, B's type checker has
access to A's exported type information.

### 8.2 Entry Module Re-Check

After all dependencies are processed, the entry module is type-checked a
second time with all dependency exports registered:

```cpp
TypeChecker entryTc;
for (auto& [modName, exports] : allExports) {
    entryTc.registerExternalModule(modName, exports);
}
entryTc.check(*module);
```

This validates that `from X import Y` references resolve to the correct types.

---

## 9. File Mode Detection

```cpp
bool Driver::isDragonFile(const std::string& filename) {
    if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".dr")
        return true;
    if (!isPythonFile(filename)) return true;   // default to Dragon
    return false;
}

bool Driver::isPythonFile(const std::string& filename) {
    return filename.size() > 3 &&
           filename.substr(filename.size() - 3) == ".py";
}
```

Rules:
- `.dr` extension: Dragon mode.
- `.py` extension: Python mode.
- Anything else: Dragon mode (default).
- The `-f` flag overrides any detection, forcing Python mode.

---

## 10. Site-Packages Detection

When `--site-packages` is enabled and no explicit path is given,
`detectSitePackagesPath()` resolves the directory in two steps:

1. **`DRAGON_SITE_PACKAGES` env var (exec-free, wins outright).** If the
   operator sets it, that directory is used verbatim - no interpreter is
   launched at all:

   ```cpp
   if (const char* env = std::getenv("DRAGON_SITE_PACKAGES")) {
       if (env[0] != '\0') return std::string(env);
   }
   ```

2. **Trusted-interpreter auto-detect.** Otherwise the resolver picks a trusted,
   **absolute** Python interpreter and asks it for the path:

   ```cpp
   std::string python = resolvePythonInterpreter();   // see below
   if (python.empty()) return "";                      // no trusted interp; skip
   std::string cmd = "\"" + python +
       "\" -c \"import site; print(site.getsitepackages()[0])\" 2>/dev/null";
   // popen, read output, trim trailing newline, return
   ```

`resolvePythonInterpreter()` honors an explicit `DRAGON_PYTHON` override (an
existing path), else picks the first existing absolute candidate
(`/usr/bin/python3`, `/usr/local/bin/python3`, `/bin/python3`). It deliberately
never falls back to a bare `python3`: `popen` spawns `/bin/sh -c`, which would
resolve `python3` via `$PATH`, letting a writable earlier-in-`$PATH` directory
hijack every `dragon build --site-packages`. Resolving to an absolute path
closes that.

The detected path becomes the final search location in `findModuleFile()`,
after the source directory, the `.drx/` egg dir, and the `-I`/stdlib search
paths. If no trusted interpreter exists or the command fails, the function
returns an empty string and site-packages search is silently disabled.

---

## 11. Diagnostic Formatting

**Header**: `include/dragon/DiagnosticFormatter.h`
**Source**: `src/DiagnosticFormatter.cpp`

The `DiagnosticFormatter` provides consistent error formatting across all
compiler stages. It supports two modes controlled by `DiagnosticStyle`:

### Dragon Theme (default)

```
DRAGON SCALE ERROR: <message> at [<filename>:<line>:<column>]
  Suggestion: <suggestion text>
```

### Plain Mode

```
<filename>:<line>:<column>: error: <message>
```

### Style Options

```cpp
struct DiagnosticStyle {
    bool useDragonTheme = true;    // Dragon-branded vs plain
    bool showSuggestions = true;   // Show "Suggestion: ..." lines
    bool colorOutput = false;     // ANSI color codes (off by default for tests)
};
```

### Special Formatters

- `formatMissingType()`: For missing type annotations, includes a Dragon-themed
  suggestion ("To breathe fire, the Dragon needs to know this type. Add ': int',
  ': str', etc.").
- `formatUntypedImport()`: For untyped `.py` imports, prints "Borders must be
  secured: <file> must be strictly typed to be imported into a Dragon context."
  This message is always Dragon-themed regardless of the style flag, because it
  represents a core Dragon philosophy about type safety at module boundaries.

---

## 12. Pipeline Summary

```
                    .dr / .py source file
                            |
                     [1] Read Source
                            |
                 [2] Determine File Mode
                            |
                       [3] Lexer
                   (LexerOptions based on mode)
                            |
                       [4] Parser
                   (ParserOptions: requireTypes for .dr)
                            |
              [5] --dump-ast (optional output)
                            |
           [6] TypeHintEnforcer (.py files only)
                            |
                [7] Sema (semantic analysis)
                            |
                    [8] TypeChecker
                            |
            [9] ModuleResolver (DFS import graph)
                            |
        [10] Process Dependencies (topological order)
              - enforce types on .py deps
              - sema + typecheck each dep
              - accumulate exports
              - re-typecheck entry module
                            |
       [11] CodeGen (LLVM IR generation)
              - single module: generate(module)
              - multi-module: generate(entry, deps)
                            |
    [12] compileToObject -> dragon.o in a secure randomized 0700 temp dir
                            |
    [13] linkExecutable -> cc links dragon.o + runtime + bundled libs -> <output>
                            |
    [14] Clean up the temp object directory
                            |
                    Native executable
```

---

## Previous Document

[009 - Memory Management](009-memory.md)

## Next Document

[011 - Type Hint Enforcement](011-type-hints.md)
