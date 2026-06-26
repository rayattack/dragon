/**
 * Dragon CodeGen API Reference
 * ============================
 * Source: include/dragon/CodeGen.h (header) + src/CodeGen.cpp (~9200 lines)
 *
 * LLVM IR code generator. Translates the Dragon AST into LLVM IR, compiles
 * to object code, and links executables via cc.
 *
 * All Dragon values are i64 at the LLVM level. Pointers cast via PtrToInt/IntToPtr.
 * VarKind enum tracks semantic types for RC decisions.
 * Runtime functions are declared lazily via getOrDeclareRuntime() cached in runtimeFuncs map.
 *
 * Uses the ASTVisitor pattern (56 visit methods).
 * Uses pimpl idiom (struct Impl with std::unique_ptr<Impl> impl_).
 * NOTE: pimpl is bae
 */

#pragma once
#include <string>
#include <vector>
#include <memory>

// Forward declarations
namespace dragon { class Module; }
namespace llvm { class Module; }

// ============================================================================
// 1. GC MODE
// ============================================================================

/**
 * Garbage collection strategy. Selectable via --gc=rc or --gc=none.
 */
enum class GCMode {
    None,  ///< No garbage collection (leak everything - for debugging/benchmarking)
    RC     ///< Reference counting (default, CPython-like) with cycle collector
};


// ============================================================================
// 2. CODEGEN DIAGNOSTIC
// ============================================================================

struct CodeGenDiagnostic {
    enum class Level { Warning, Error };

    Level level;
    SourceLocation location;
    std::string message;
};


// ============================================================================
// 3. CODEGEN OPTIONS
// ============================================================================

/**
 * Configuration for code generation and linking.
 */
struct CodeGenOptions {
    int optimizationLevel = 0;             ///< LLVM optimization level (0-3)
    GCMode gcMode = GCMode::RC;           ///< Garbage collection mode
    std::string targetTriple;              ///< LLVM target triple (default: host)
    bool debugInfo = false;                ///< Generate DWARF debug info
    std::string outputFile = "a.out";      ///< Output executable path
    std::string runtimeLibPath;            ///< Path to libdragon_runtime.a (DRAGON_RUNTIME_LIB)
    std::string sqlite3LibPath;            ///< Path to libsqlite3.a (DRAGON_SQLITE3_LIB)
    std::string pcre2LibPath;              ///< Path to libpcre2-8.a (DRAGON_PCRE2_LIB)
    std::vector<std::string> linkedLibraries;     ///< Extra -l flags (e.g., "m", "pthread", "curl")
    std::vector<std::string> librarySearchPaths;  ///< Extra -L paths
};


// ============================================================================
// 4. VARKIND ENUM (internal to CodeGen::Impl, documented for reference)
// ============================================================================

/**
 * Semantic variable type classification. Used to determine:
 *  - Whether a variable needs RC cleanup at scope exit
 *  - Which decref function to call (dragon_decref vs dragon_decref_str)
 *  - Whether to incref on assignment/return
 *
 * NOTE: This is defined inside CodeGen::Impl in src/CodeGen.cpp, not in the header.
 *  Documented here for reference.
 */
enum class VarKind {
    Int,            ///< Integer (i64, no refcount)
    Float,          ///< Float (i64 bit-cast, no refcount)
    Bool,           ///< Boolean (i64, no refcount)
    Str,            ///< Dynamic heap string (DragonString with header, decref_str)
    StrLiteral,     ///< Compile-time string literal (no header, never decref)
    Bytes,          ///< Bytes object (header, decref)
    List,           ///< List container (header, decref)
    Dict,           ///< Dictionary container (header, decref)
    Tuple,          ///< Tuple container (header, decref)
    Set,            ///< Set container (header, decref)
    File,           ///< File object (header, decref)
    ClassInstance,  ///< User-defined class instance (GC header, decref)
    Generator,      ///< Generator coroutine (header, decref)
    Other           ///< Unknown/generic type
};


// ============================================================================
// 5. CODEGEN CLASS
// ============================================================================

/**
 * LLVM IR code generator for Dragon.
 * Implements ASTVisitor with 56 visit() methods for all AST node types.
 */
class CodeGen {
public:
    /**
     * Construct code generator with options.
     * Initializes LLVM context, module, IRBuilder, and target machine.
     * @param options Configuration for optimization, GC, linking, etc.
     */
    explicit CodeGen(CodeGenOptions options = {}) {}

    ~CodeGen() {}

    // --- IR Generation ---

    /**
     * Generate LLVM IR for a single-file module.
     * @param module Parsed, analyzed Module AST
     * @return true if IR generation succeeded
     */
    bool generate(dragon::Module& module) { return false; }

    /**
     * Generate LLVM IR for a multi-file project.
     * Dependencies are forward-declared and emitted before the entry module.
     * All modules compile into a SINGLE LLVM module (no separate compilation units).
     * @param entryModule The main entry module
     * @param depModules Dependency modules in topological order (deps first)
     * @return true if IR generation succeeded
     */
    bool generate(dragon::Module& entryModule, const std::vector<dragon::Module*>& depModules) { return false; }

    // --- Output ---

    /**
     * Get the generated LLVM module for inspection.
     * @return Pointer to llvm::Module (owned by CodeGen)
     */
    llvm::Module* getLLVMModule() { return nullptr; }

    /**
     * Write LLVM IR to a text file (.ll format).
     * @param filename Output file path
     * @return true on success
     */
    bool writeIR(const std::string& filename) { return false; }

    /**
     * Write LLVM bitcode to a binary file (.bc format).
     * @param filename Output file path
     * @return true on success
     */
    bool writeBitcode(const std::string& filename) { return false; }

    /**
     * Compile LLVM module to native object file (.o format).
     * Uses LLVM's target machine and pass pipeline.
     * @param filename Output file path
     * @return true on success
     */
    bool compileToObject(const std::string& filename) { return false; }

    /**
     * Link object file into executable using cc.
     * Links against: dragon_runtime, sqlite3, pcre2, libuv, plus user-specified libraries.
     * @param outputFile Output executable path
     * @param objectFile Input object file path
     * @return true on success
     */
    bool linkExecutable(const std::string& outputFile, const std::string& objectFile) { return false; }

    // --- Diagnostics ---

    /**
     * Get all code generation diagnostics.
     * @return Vector of CodeGenDiagnostic
     */
    const std::vector<CodeGenDiagnostic>& diagnostics() const { return {}; }

    /**
     * Check if any errors occurred during code generation.
     * @return true if at least one Error-level diagnostic
     */
    bool hasErrors() const { return false; }

    // --- ASTVisitor overrides (56 methods) ---
    // All node types listed in ast-api.h are visited.
    // See ast-api.h section 16 for the full visitor interface.
};


// ============================================================================
// 6. KEY INTERNAL MECHANISMS (documented for reference)
// ============================================================================

/*
 * Tagged Value System:
 *  All values are i64. Pointers cast via PtrToInt/IntToPtr.
 *  DragonValueTag: TAG_INT=0, TAG_STR=1, TAG_FLOAT=2, TAG_BOOL=3,
 *  TAG_NONE=4, TAG_LIST=5, TAG_DICT=6, TAG_BYTES=7, TAG_GENERATOR=8
 *
 * Runtime Function Declaration:
 *  getOrDeclareRuntime(name, FunctionType) - lazily declares extern "C" runtime
 *  functions, cached in runtimeFuncs map.
 *
 * coerceArg(value, fromType, toType):
 *  i64->i1: ICmpNE(value, 0)
 *  i1->i64: ZExt
 *  ptr->i64: PtrToInt
 *  i64->ptr: IntToPtr
 *
 * Scope-Exit RC Cleanup:
 *  At function return and scope exit, all heap-kind local variables are decref'd.
 *  VarKind determines which decref to call and whether cleanup is needed.
 *
 * Module Globals:
 *  Module-level vars -> llvm::GlobalVariable with InternalLinkage.
 *  .dr: scope-chain access. .py: requires "global x" declaration.
 *
 * Generator Compilation:
 *  Generator functions detected by containsYield() scan.
 *  Body compiled as foo__gen_body(i8* gen, params...) -> void.
 *  Wrapper foo(params...) -> dragon_generator_create(body_fn, args, nargs).
 *  Trampoline: dragon_gen_trampoline dispatches via switch on nargs.
 *
 * Exception Handling:
 *  setjmp/longjmp pattern. dragon_exc_push_frame() -> setjmp.
 *  Finally blocks inlined at return/break/continue via finallyStack.
 *
 * Class Instance Layout:
 *  DragonObjectHeader (16 bytes) prepended to class struct.
 *  Field indices shifted +2 to account for header.
 *  Per-class __dealloc__ and __traverse__ generated for GC.
 */
