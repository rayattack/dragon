#ifndef DRAGON_CODEGEN_H
#define DRAGON_CODEGEN_H

#include "dragon/AST.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
class Function;
class FunctionType;
class StructType;
class Value;
class Type;
class BasicBlock;
class AllocaInst;
}

namespace dragon {

struct CodeGenDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

enum class GCMode { None, RC };

struct CodeGenOptions {
    int optimizationLevel = 0;

    GCMode gcMode = GCMode::RC;

    std::string targetTriple;

    bool debugInfo = false;

    bool checkOverflow = false;

    std::string outputFile = "a.out";

    std::string runtimeLibPath;

    std::string sqlite3LibPath;

    std::string pcre2LibPath;

    std::string llhttpLibPath;

    std::string mbedtlsLibPath;

    std::string zstdLibPath;

    std::vector<std::string> linkedLibraries;

    std::vector<std::string> librarySearchPaths;

    std::vector<std::string> ccSources;

    std::string webviewShimPath;

    std::string assetsDir;

    std::vector<std::string> includePaths;

    bool jitTarget = false;

    bool replMode = false;

    int replTurnIndex = 0;

    size_t replResidentStmtCount = 0;

    std::vector<std::string> replResidentModules;

    bool replTeardown = false;
};

class CodeGen : public ASTVisitor {
public:
    explicit CodeGen(CodeGenOptions options = {});
    ~CodeGen();

    bool generate(dragon::Module& module);

    bool generate(dragon::Module& entryModule,
                  const std::vector<dragon::Module*>& depModules);

    llvm::Module* getLLVMModule();
    std::unique_ptr<llvm::Module> takeModule();
    std::unique_ptr<llvm::LLVMContext> takeContext();
    bool writeIR(const std::string& filename);
    bool writeBitcode(const std::string& filename);
    bool compileToObject(const std::string& filename);
    bool linkExecutable(const std::string& outputFile,
                        const std::string& objectFile);

    const std::vector<CodeGenDiagnostic>& diagnostics() const;
    bool hasErrors() const;

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
    void emitSqlTemplate(TemplateExpr& node, const std::string& contentType);
    llvm::Value* emitTemplateJoin(TemplateExpr& node, const TemplatePart& part,
                                  Expr* listExpr, llvm::Value* listVal,
                                  const std::string& contentType,
                                  bool elementsRaw);
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

    bool emitBuiltinCall(CallExpr& node, const std::string& name);
    bool emitMethodCall(CallExpr& node, AttributeExpr& attr);
    void emitSuperCtorCall(CallExpr& node);
    void emitGeneratorFn(FunctionDecl& node, llvm::Function* wrapper,
                         const std::string& siteName, bool hasSelf,
                         const std::string& selfClass, size_t userParamStart);
    void emitVarArgCall(llvm::Function* func, CallExpr& node);
    static bool callHasSpread(CallExpr& node);
    static bool callHasStarArg(CallExpr& node);
    static bool spreadStaticArity(CallExpr& node, int64_t& arityOut);
    bool emitSpreadDispatch(CallExpr& node);
    void emitSpreadCall(llvm::Function* func, CallExpr& node,
                        std::vector<llvm::Value*> prefixArgs,
                        const std::string& dispName);
    void emitPrintArgRaw(Expr* argExpr);
    bool emitReplEcho(ExprStmt& node);
    void emitCallableValueCall(llvm::Value* fnPtrVal,
                               llvm::FunctionType* userFnType,
                               const std::vector<llvm::Value*>& args,
                               bool ownedClosure, const std::string& label);
    void emitNestedFunctionDecl(FunctionDecl& node);

private:
    void emitCompAppendBody(Expr* element, Expr* condition,
                            llvm::AllocaInst* listAlloca, int64_t elemTag,
                            const std::string& bbPrefix);
    llvm::Value* emitCompFilterTruth(llvm::Value* cond);
    struct FnCapture;
    std::vector<FnCapture> collectFnCaptures(
        const std::vector<std::string>& capturedVars,
        const std::vector<std::string>& mutatedCapturedVars);
    void bindFnParams(llvm::Function* fn, llvm::FunctionType* fnType,
                      const std::vector<std::pair<std::string, TypeExpr*>>& params);
    llvm::StructType* buildFnEnvStruct(const std::string& envName,
                                       const std::vector<FnCapture>& captures);
    llvm::Value* bindFnEnvCaptures(llvm::Function* fn,
                                   llvm::StructType* envStructType,
                                   const std::vector<FnCapture>& captures);
    llvm::Value* materializeClosureEnv(const std::string& fnName,
                                       llvm::Function* fn,
                                       llvm::StructType* envStructType,
                                       const std::vector<FnCapture>& captures);
    void emitCompRangeArgs(CallExpr* call, llvm::Value*& start,
                           llvm::Value*& end, llvm::Value*& step);
    void bindCompElemVar(Expr* iterable, const std::string& varName,
                         llvm::Value* collVal, llvm::Value* curIdx);
    void emitCompExtraClauses(std::vector<CompClause>& clauses, size_t clauseIdx,
                              const std::function<void()>& innermost);
    void emitCompLoopNest(Expr* iterable, const std::vector<std::string>& varNames,
                          std::vector<CompClause>& extraClauses,
                          const std::string& bbPrefix,
                          const std::string& idxAllocaName,
                          const std::function<void()>& innermost,
                          const std::function<void(llvm::Value*, llvm::Value*)>&
                              bindElemVars);
    struct BuiltinLowering;
    struct MethodCallLowering;
    void emitResolvedMethodCall(CallExpr& node, MethodCallLowering& ml);
    bool emitBuiltinCallInner(CallExpr& node, const std::string& name,
                              BuiltinLowering& bl);
    bool emitLenBuiltin(CallExpr& node, BuiltinLowering& bl);
    bool emitIsinstanceBuiltin(CallExpr& node);
    bool tryEmitIsinstanceNicheCheck(CallExpr& node, const std::string& typeName);
    bool emitTypeBuiltin(CallExpr& node, BuiltinLowering& bl);
    bool tryEmitListFromRange(CallExpr& node);
    bool tryEmitPrintDictSubscriptRaw(SubscriptExpr& sub, llvm::Value* arg,
                                      bool staticContainerVal);
    bool tryEmitPrintDictAttrRaw(AttributeExpr& attr, llvm::Value* arg,
                                 bool staticContainerVal);
    bool tryEmitPrintUnionRaw(Expr* argExpr, llvm::Value* arg);
    void emitShortCircuit(BinaryExpr& node);
    bool tryEmitStrSelfAppend(AssignStmt& node);
    bool tryEmitSetitemOverloadStore(SubscriptExpr& sub, llvm::Value* val);
    bool tryEmitDictSubscriptStore(SubscriptExpr& sub, AssignStmt& node,
                                   llvm::Value* val);
    bool tryEmitListSubscriptStore(SubscriptExpr& sub, AssignStmt& node,
                                   llvm::Value* val);
    bool tryEmitDictAttrStore(AttributeExpr& attr, AssignStmt& node,
                              llvm::Value* val);
    bool tryEmitStaticFieldStore(AttributeExpr& attr, AssignStmt& node,
                                 llvm::Value* val);
    bool tryEmitPropertySetterStore(AttributeExpr& attr, AssignStmt& node,
                                    llvm::Value* val);
    bool tryEmitInstanceFieldStore(AttributeExpr& attr, AssignStmt& node,
                                   llvm::Value* val);
    void emitTupleUnpackAssign(TupleExpr& tupleTarget, AssignStmt& node,
                               llvm::Value* val);
    void emitNameAssign(NameExpr& name, AssignStmt& node, llvm::Value* val);
    bool tryEmitExistingGlobalStore(NameExpr& name, AssignStmt& node,
                                    llvm::Value* val);
    void emitNewModuleGlobalStore(NameExpr& name, AssignStmt& node,
                                  llvm::Value* val);
    void emitLocalSlotStore(NameExpr& name, AssignStmt& node, llvm::Value* val,
                            llvm::AllocaInst* alloca, bool hadExistingSlot);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif

