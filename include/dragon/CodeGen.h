#ifndef DRAGON_CODEGEN_H
#define DRAGON_CODEGEN_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>

// TODO: split Impl out of this header once the api reference catches up
// Forward declarations for LLVM types
namespace llvm {
class LLVMContext;
class Module;
class Function;
class FunctionType;
class Value;
class Type;
class BasicBlock;
}

namespace dragon {

/// Code generation diagnostic
struct CodeGenDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

/// GC mode for code generation
enum class GCMode { None, RC };

/// Configuration for code generation
struct CodeGenOptions {
    /// Optimization level (0-3)
    int optimizationLevel = 0;

    /// Garbage collection mode (RC = reference counting, None = leak everything)
    GCMode gcMode = GCMode::RC;

    /// Target triple (default: host)
    std::string targetTriple;

    /// Generate debug info
    bool debugInfo = false;

    /// 4.6: emit `__builtin_*_overflow` intrinsics for int +/-/*/** so that
    /// silent 64-bit wraparound becomes a runtime OverflowError instead.
    /// Off by default; opt-in via `--check-overflow` for correctness-critical
    /// programs that don't want CPython's BigInt cost.
    bool checkOverflow = false;

    /// Output filename
    std::string outputFile = "a.out";

    /// Path to dragon_runtime static library for linking
    std::string runtimeLibPath;

    /// Path to bundled sqlite3 static library for linking
    std::string sqlite3LibPath;

    /// Path to bundled PCRE2 (8-bit) static library for linking
    std::string pcre2LibPath;

    /// Path to bundled llhttp static library for linking
    std::string llhttpLibPath;

    /// Path to bundled mbedTLS static library for linking (TLS engine)
    std::string mbedtlsLibPath;

    /// Path to a bundled zstd static archive (macOS, where no system libzstd)
    /// exists). EMpty on linux: the shared system lib is linked via -lzstd.
    std::string zstdLibPath;

    /// Extra libraries to link (e.g. "m", "pthread", "curl")
    std::vector<std::string> linkedLibraries;

    /// Extra library search paths (e.g. "/usr/local/lib")
    std::vector<std::string> librarySearchPaths;

    /// ADR 041 - C/C++ shim sources to compile and link (--cc-source). Each is
    /// compiled with `c++` (.cpp/.cc/.cxx/.C) or `cc` (otherwise) to a temp
    /// object and added to the final link; the link driver switches to `c++`
    /// when any C++ shim is present.
    std::vector<std::string> ccSources;

    /// Include directories (-I) forwarded to the shim compiler for --cc-source.
    std::vector<std::string> includePaths;
};

/// LLVM IR code generator for Dragon
///
/// Visits the type-checked AST and produces LLVM IR.
class CodeGen : public ASTVisitor {
public:
    explicit CodeGen(CodeGenOptions options = {});
    ~CodeGen();

    /// Generate LLVM IR for a module
    bool generate(dragon::Module& module);

    /// Generate LLVM IR for a multi-file project
    bool generate(dragon::Module& entryModule,
                  const std::vector<dragon::Module*>& depModules);

    /// Get the generated LLVM module
    llvm::Module* getLLVMModule();

    /// Write IR to file (.ll)
    bool writeIR(const std::string& filename);

    /// Write bitcode to file (.bc)
    bool writeBitcode(const std::string& filename);

    /// Compile to object file (.o)
    bool compileToObject(const std::string& filename);

    /// Link and create executable
    bool linkExecutable(const std::string& outputFile,
                        const std::string& objectFile);

    /// Get diagnostics
    const std::vector<CodeGenDiagnostic>& diagnostics() const;
    bool hasErrors() const;

    // Visitor methods
    void visit(NamedTypeExpr& node) override;
    void visit(GenericTypeExpr& node) override;
    void visit(OptionalTypeExpr& node) override;
    void visit(UnionTypeExpr& node) override;
    void visit(CallableTypeExpr& node) override;
    void visit(TupleTypeExpr& node) override;
    void visit(IntegerLiteral& node) override;
    void visit(FloatLiteral& node) override;
    void visit(StringLiteral& node) override;
    void visit(BooleanLiteral& node) override;
    void visit(NoneLiteral& node) override;
    void visit(NameExpr& node) override;
    void visit(BinaryExpr& node) override;
    void visit(ChainedCompExpr& node) override;
    void visit(WalrusExpr& node) override;
    void visit(UnaryExpr& node) override;
    void visit(CallExpr& node) override;
    void visit(AttributeExpr& node) override;
    void visit(SubscriptExpr& node) override;
    void visit(SliceExpr& node) override;
    void visit(ListExpr& node) override;
    void visit(TupleExpr& node) override;
    void visit(DictExpr& node) override;
    void visit(SetExpr& node) override;
    void visit(ListCompExpr& node) override;
    void visit(DictCompExpr& node) override;
    void visit(SetCompExpr& node) override;
    void visit(GeneratorExpr& node) override;
    void visit(LambdaExpr& node) override;
    void visit(IfExpr& node) override;
    void visit(AwaitExpr& node) override;
    void visit(FireExpr& node) override;
    void visit(YieldExpr& node) override;
    void visit(StarredExpr& node) override;
    void visit(TemplateExpr& node) override;
    void visit(TemplateFileExpr& node) override;
    // D032: parameter-extraction lowering for content types declaring `build`
    // (SQL). Constant-folds the canonical $$N text + FNV-1a hash and emits a
    // native-typed param pack; sets lastValue to a `contentType` instance.
    void emitSqlTemplate(TemplateExpr& node, const std::string& contentType);
    void visit(ExprStmt& node) override;
    void visit(AssignStmt& node) override;
    void visit(AugAssignStmt& node) override;
    void visit(AnnAssignStmt& node) override;
    void visit(IfStmt& node) override;
    void visit(WhileStmt& node) override;
    void visit(ForStmt& node) override;
    void visit(TryStmt& node) override;
    void visit(WithStmt& node) override;
    void visit(ThreadStmt& node) override;
    void visit(DeferStmt& node) override;
    void visit(MatchStmt& node) override;
    void visit(ReturnStmt& node) override;
    void visit(RaiseStmt& node) override;
    void visit(BreakStmt& node) override;
    void visit(ContinueStmt& node) override;
    void visit(PassStmt& node) override;
    void visit(AssertStmt& node) override;
    void visit(GlobalStmt& node) override;
    void visit(NonlocalStmt& node) override;
    void visit(DeleteStmt& node) override;
    void visit(ImportStmt& node) override;
    void visit(FromImportStmt& node) override;
    void visit(FunctionDecl& node) override;
    void visit(ClassDecl& node) override;
    void visit(TypeAliasStmt& node) override;
    void visit(dragon::Module& node) override;

    // Internal dispatch helpers (split across codegen/*.cpp files)
    bool emitBuiltinCall(CallExpr& node, const std::string& name);
    bool emitMethodCall(CallExpr& node, AttributeExpr& attr);
    // Emit a generator function: an inner body fn (a `gen` ptr, an optional
    // leading `self`, then the user params) plus the body of `wrapper`, which
    // creates and returns the generator object. Shared by free-function
    // generators (hasSelf=false) and generator methods (hasSelf=true, with
    // `selfClass` typing `self` for field access). `userParamStart` is where the
    // user params begin in node.params (skips an explicit self/cls). `siteName`
    // is the mangled symbol used to name the body fn / args struct. Defined in
    // codegen/Functions.cpp.
    void emitGeneratorFn(FunctionDecl& node, llvm::Function* wrapper,
                         const std::string& siteName, bool hasSelf,
                         const std::string& selfClass, size_t userParamStart);
    // Emit a call to a variadic (`*args`/`**kwargs`) function: packs the
    // trailing positional args into a monomorphized list and the keyword args
    // into a dict, per the callee's VarArgInfo, then emits the call and sets
    // lastValue. `func` must be a variadic callee (present in funcVarArgInfo).
    // Shared by the same-module/imported NameExpr path and the module-qualified
    // (`mod.fn(...)`) path so both lower varargs identically.
    void emitVarArgCall(llvm::Function* func, CallExpr& node);
    // C9-B call-site spread (`*tuple` / `*list` / `**dict`). callHasSpread is
    // the cheap detector every call path uses to decide whether to route to the
    // spread machinery. emitSpreadDispatch resolves a NameExpr / module-attr
    // callee (free function or class ctor) and emits the expanded call, or
    // returns false for an unsupported spread target (so the caller diagnoses).
    // emitSpreadCall is the shared core: given a resolved llvm::Function and any
    // already-evaluated prefix args (e.g. `self`), it expands node.args (with
    // *spread) and node.kwArgs (with **spread) into the full positional vector,
    // honoring the box/coerce/refcount/default-fill discipline, then emits the
    // call and sets lastValue. Spread elements are BORROWED from the source
    // container (no argTemps entry) - the callee increfs whatever it retains.
    static bool callHasSpread(CallExpr& node);
    static bool callHasStarArg(CallExpr& node);
    // Total positional arity of a call when every `*spread` is a `*tuple`
    // (static length). Returns false (arity unknowable at compile time) if any
    // spread is a `*list` or a `**dict` is present - used to pick an overloaded
    // ctor's `_new_N` body.
    static bool spreadStaticArity(CallExpr& node, int64_t& arityOut);
    bool emitSpreadDispatch(CallExpr& node);
    void emitSpreadCall(llvm::Function* func, CallExpr& node,
                        std::vector<llvm::Value*> prefixArgs,
                        const std::string& dispName);
    // Emit code that prints one argument with NO trailing newline (the `_raw`
    // runtime printers). Shared by single- and multi-arg print(); the caller
    // adds spaces between args and one trailing newline.
    void emitPrintArgRaw(Expr* argExpr);
    // Emit an indirect call to a callable VALUE whose closure-ness is not known
    // statically (a closure-returning call result, a Callable list element, a
    // Callable parameter, ...). Discriminates at runtime via the
    // DragonObjectHeader type_tag: TAG_CLOSURE => unwrap {fn, env} and call
    // fn(args, env) (with an env==null sub-case for a bare fn wrapped as a
    // DragonClosure); otherwise call the value as a raw bare fn pointer.
    // `fnPtrVal` is the evaluated callee, `userFnType` its user-facing signature
    // (no trailing env), `args` the already-evaluated+coerced user arguments.
    // Sets lastValue to the (normalized) result, or null for a void callee. When
    // `ownedClosure` is true the callee is an owned temporary and is decref'd
    // (tag-gated) after the call. `label` names the emitted basic blocks.
    void emitCallableValueCall(llvm::Value* fnPtrVal,
                               llvm::FunctionType* userFnType,
                               const std::vector<llvm::Value*>& args,
                               bool ownedClosure, const std::string& label);
    void emitNestedFunctionDecl(FunctionDecl& node);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_CODEGEN_H
