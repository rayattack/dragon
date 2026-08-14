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

    /// 4.6: emit `__builtin_*_overflow` intrinsics so wraparound raises
    /// OverflowError. Off by default; opt-in via `--check-overflow`.
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

    /// Bundled zstd static archive (macOS only; empty on Linux, which uses -lzstd)
    std::string zstdLibPath;

    /// Extra libraries to link (e.g. "m", "pthread", "curl")
    std::vector<std::string> linkedLibraries;

    /// Extra library search paths (e.g. "/usr/local/lib")
    std::vector<std::string> librarySearchPaths;

    /// ADR 041: C/C++ shim sources to compile and link (--cc-source), each
    /// compiled to a temp object; the link driver switches to `c++` if any is C++.
    std::vector<std::string> ccSources;

    /// D031: platform/webview_linux.cpp, resolved by the Driver; compiled+linked
    /// (webkit2gtk) when the program imports ui. Empty fails link with a clear error.
    std::string webviewShimPath;

    /// D031: the program's assets/ dir, embedded into the binary for the
    /// app:// scheme handler when it imports ui. Empty = an empty table is emitted.
    std::string assetsDir;

    /// Include directories (-I) forwarded to the shim compiler for --cc-source.
    std::vector<std::string> includePaths;
};

/// Visits the type-checked AST and produces LLVM IR.
class CodeGen : public ASTVisitor {
public:
    explicit CodeGen(CodeGenOptions options = {});
    ~CodeGen();

    bool generate(dragon::Module& module);

    /// Generates IR for a multi-file project (entry plus its dependency modules).
    bool generate(dragon::Module& entryModule,
                  const std::vector<dragon::Module*>& depModules);

    llvm::Module* getLLVMModule();
    bool writeIR(const std::string& filename);       // .ll
    bool writeBitcode(const std::string& filename);  // .bc
    bool compileToObject(const std::string& filename);
    bool linkExecutable(const std::string& outputFile,
                        const std::string& objectFile);

    const std::vector<CodeGenDiagnostic>& diagnostics() const;
    bool hasErrors() const;

    // Visitor methods
    void visit(NamedTypeExpr& node) override;
    void visit(GenericTypeExpr& node) override;
    void visit(OptionalTypeExpr& node) override;
    void visit(UnionTypeExpr& node) override;
    void visit(CallableTypeExpr& node) override;
    void visit(TupleTypeExpr& node) override;
    void visit(ContractSetTypeExpr& node) override;
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
    void visit(AsCastExpr& node) override;
    void visit(FireExpr& node) override;
    void visit(YieldExpr& node) override;
    void visit(StarredExpr& node) override;
    void visit(TemplateExpr& node) override;
    void visit(TemplateFileExpr& node) override;
    // D032: parameter-extraction lowering for content types declaring `build`
    // (SQL); folds canonical $$N text + FNV-1a hash into a native param pack.
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
    void visit(ContractDecl& node) override;
    void visit(TypeAliasStmt& node) override;
    void visit(dragon::Module& node) override;

    // Internal dispatch helpers, split across codegen/*.cpp.
    bool emitBuiltinCall(CallExpr& node, const std::string& name);
    bool emitMethodCall(CallExpr& node, AttributeExpr& attr);
    // Emits the generator body fn plus `wrapper`'s body (creates/returns the
    // generator object). Shared by free functions and methods (hasSelf, selfClass).
    void emitGeneratorFn(FunctionDecl& node, llvm::Function* wrapper,
                         const std::string& siteName, bool hasSelf,
                         const std::string& selfClass, size_t userParamStart);
    // Calls a variadic (`*args`/`**kwargs`) function: packs trailing positional
    // args into a list and kwargs into a dict per VarArgInfo, then calls.
    void emitVarArgCall(llvm::Function* func, CallExpr& node);
    // C9-B call-site spread (`*tuple`/`*list`/`**dict`): callHasSpread routes,
    // emitSpreadDispatch resolves+emits, emitSpreadCall expands args (borrowed).
    static bool callHasSpread(CallExpr& node);
    static bool callHasStarArg(CallExpr& node);
    // Static positional arity when every spread is a `*tuple`; false (unknowable)
    // if a `*list` or `**dict` is present. Used to pick an overloaded ctor body.
    static bool spreadStaticArity(CallExpr& node, int64_t& arityOut);
    bool emitSpreadDispatch(CallExpr& node);
    void emitSpreadCall(llvm::Function* func, CallExpr& node,
                        std::vector<llvm::Value*> prefixArgs,
                        const std::string& dispName);
    // Prints one argument with no trailing newline (the `_raw` runtime
    // printers); print() adds inter-arg spaces and the final newline itself.
    void emitPrintArgRaw(Expr* argExpr);
    // Indirect call to a callable of unknown closure-ness, discriminated via
    // the type_tag (TAG_CLOSURE unwraps {fn, env}); ownedClosure decrefs after.
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

