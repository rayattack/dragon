#ifndef DRAGON_CODEGEN_IMPL_H
#define DRAGON_CODEGEN_IMPL_H

#include "dragon/ValueTags.h"
#include <execinfo.h>
#include <limits>
#include "dragon/CodeGen.h"
#include "dragon/TypeChecker.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdlib>
#include <stack>
#include <climits>
#if !defined(_WIN32)
  #include <sys/wait.h>
#endif
#include <map>
#include <set>

namespace dragon {

struct CodeGen::Impl {
    CodeGenOptions options;
    std::vector<CodeGenDiagnostic> diagnostics;

    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    llvm::Value* lastValue = nullptr;

    enum class VarKind { Int, Float, Bool, Str, StrLiteral, List, Dict, Tuple, Set, File, ClassInstance, Generator, Type, Closure, Union, Deque, Other };

    struct Scope {
        std::unordered_map<std::string, llvm::AllocaInst*> vars;
        std::unordered_map<std::string, VarKind> varKinds;
        std::unordered_set<std::string> borrowed;
        std::unordered_set<std::string> cellBacked;
        std::unordered_set<std::string> stackAllocated;
        std::unordered_set<std::string> detachOnExit;
        std::unordered_set<std::string> lockDestroyOnExit;
        std::unordered_map<std::string, llvm::AllocaInst*> cleanupSlots;
        llvm::AllocaInst* cleanupBaseAlloca = nullptr;
        struct DeferEntry {
            llvm::Function* thunk = nullptr;
            llvm::AllocaInst* argSlots = nullptr;
            unsigned argc = 0;
            std::vector<VarKind> drainKinds;
        };
        std::vector<DeferEntry> deferred;
    };
    std::vector<Scope> scopes;

    void collectNestedMutatedCaptures(const std::vector<std::unique_ptr<Stmt>>& body,
                                      std::unordered_set<std::string>& out);
    void collectNestedMutatedCaptures(Stmt* s,
                                      std::unordered_set<std::string>& out);
    void collectNestedMutatedCaptures(Expr* e,
                                      std::unordered_set<std::string>& out);

    void computeStackAllocSites(Module& entryModule,
                                const std::vector<Module*>& depModules);
    void analyzeBlockForStackAlloc(const std::vector<std::unique_ptr<Stmt>>& stmts,
                                   bool isModuleTopLevel = false);
    bool exprEscapes(Expr* e, const std::string& name);
    bool stmtEscapes(Stmt* s, const std::string& name);
    bool nodeMentionsName(Expr* e, const std::string& name);
    bool nodeMentionsName(Stmt* s, const std::string& name);

    bool taskLocalTransferEscapes(Stmt* s, const std::string& name);
    std::unordered_set<std::string> cellPromotedLocals;

    llvm::Function* currentFunction = nullptr;

    struct LoopInfo {
        llvm::BasicBlock* breakBlock;
        llvm::BasicBlock* continueBlock;
        size_t scopeDepth;
        size_t tryFrameDepth = 0;
        size_t exitCleanupDepth = 0;
    };
    std::stack<LoopInfo> loopStack;
    std::vector<llvm::Function*> tryFrameFuncs;

    size_t currentFnTryFrames() {
        size_t n = 0;
        for (auto it = tryFrameFuncs.rbegin(); it != tryFrameFuncs.rend(); ++it) {
            if (*it != currentFunction) break;
            ++n;
        }
        return n;
    }

    void emitExcFramePops(size_t n) {
        if (n == 0) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (size_t i = 0; i < n; ++i)
            builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
    }

    struct WithCleanupItem {
        bool isClassCtx;
        bool isLock;
        std::string className;
        llvm::Value* val;
        llvm::Value* enterResult = nullptr;
        llvm::Function* exitFn = nullptr;
        bool isLockTemp = false;
        bool subjectOwned = true;
                                   // walrus): with-exit must not decref `val` (A/B-proven UAF, test_rc_with_subject.dr); the __enter__ result's +1 is always dropped.
    };

    struct ExitCleanup {
        bool isWith = false;
        std::vector<Stmt*> finallyBody;
        std::vector<WithCleanupItem> withItems;
        llvm::Function* func = nullptr;
        size_t scopeDepth = 0;
    };
    std::vector<ExitCleanup> exitCleanupStack;

    size_t currentFnExitCleanupBase() {
        size_t i = exitCleanupStack.size();
        while (i > 0 && exitCleanupStack[i - 1].func == currentFunction) --i;
        return i;
    }

    std::vector<std::string> handlerExcVars;

    llvm::AllocaInst* generatorPtr = nullptr;
    std::unordered_set<std::string> generatorFunctions;

    std::unordered_set<std::string> funcReturnsType;

    std::unordered_set<std::string> funcReturnsPtr;

    std::unordered_set<std::string> funcReturnsClosure;

    bool functionReturnsClosure(FunctionDecl& node);

    std::unordered_set<std::string> varIsPtrCallable;

    std::unordered_set<std::string> decoratedClassesBySym;
    std::unordered_map<std::string, std::vector<Expr*>> classDecoratorExprsBySym;

    std::unordered_set<std::string> dataclassClassNamesBySym;
    std::unordered_map<std::string, std::vector<std::string>> dataclassFieldNamesBySym;

    enum class EnumKind { Plain, Int, Str };
    std::unordered_map<std::string, EnumKind> enumKindBySym;
    std::unordered_map<std::string, std::vector<std::string>> enumMemberNamesBySym;

    static Type::Kind elemVarKindToTypeKind(VarKind ek);

    void trackPtrParam(const std::string& paramName, TypeExpr* typeExpr);

    llvm::Value* emitNewTypedList(int64_t elemTag, bool isAny, llvm::Value* capVal);

    void emitTypedListAppend(llvm::Value* list, llvm::Value* val, Expr* elemExpr,
                             int64_t elemTag, bool isAny, CodeGen& cg);

    llvm::FunctionType* callableTypeExprToFnType(CallableTypeExpr* callable) {
        std::vector<llvm::Type*> pts;
        pts.reserve(callable->paramTypes.size());
        for (auto& pt : callable->paramTypes)
            pts.push_back(typeExprToLLVM(pt.get()));
        llvm::Type* rt = typeExprToLLVM(callable->returnType.get());
        return llvm::FunctionType::get(rt, pts, false);
    }

    llvm::Type* i64Type = nullptr;
    llvm::Type* intcType = nullptr;
    llvm::Type* f64Type = nullptr;
    llvm::Type* i1Type = nullptr;
    llvm::Type* i8PtrType = nullptr;
    llvm::Type* voidType = nullptr;

    llvm::MDNode* tbaaRoot = nullptr;
    llvm::MDNode* tbaaListHeader = nullptr;
    llvm::MDNode* tbaaListData = nullptr;

    llvm::StructType* boxType = nullptr;

    std::unordered_map<std::string, llvm::Function*> runtimeFuncs;

    int lambdaCounter = 0;

    int excCounter = 0;

    int forIterCounter = 0;

    int64_t excTypeCode(const std::string& name);

    bool isBuiltinExcName(const std::string& name);

    bool isExcType(const std::string& name) {
        return isBuiltinExcName(name) ||
               userExcCodesBySym.count(classSymPrefix(name)) > 0;
    }

    int64_t userExcNextCode = 1000;
    std::unordered_map<std::string, int64_t> userExcCodesBySym;
    std::unordered_map<int64_t, int64_t> userExcParentCodes;

    std::pair<int64_t, int64_t> excTypeRange(int64_t code) {
        switch (code) {
            case 0:   return {0, 105};
            case 10:  return {10, 105};
            case 20:  return {20, 23};
            case 30:  return {30, 31};
            case 40:  return {40, 42};
            case 44:  return {44, 45};
            case 50:  return {50, 61};
            case 57:  return {57, 61};
            case 70:  return {70, 72};
            case 90:  return {90, 94};
            case 91:  return {91, 94};
            case 100: return {100, 105};
            default:  return {code, code};
        }
    }

    std::set<std::string> fileResolvedModules;

    std::set<std::string> classNames;
    std::string currentClassName;
    std::unordered_map<std::string, llvm::StructType*> classStructTypesBySym;
    std::set<std::string> typedDictClassesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> typedDictFieldKindsBySym;
    std::unordered_map<std::string, std::string> varTypedDictClass;
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classFieldIndicesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, llvm::Type*>> classFieldTypesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, VarKind>> classFieldKindsBySym;
    std::unordered_map<std::string, std::vector<std::string>> classFieldOrderBySym;
    std::unordered_map<std::string, llvm::GlobalVariable*> classIdGlobalsBySym;
    std::unordered_map<std::string, std::string> classDocstringsBySym;
    std::unordered_map<std::string, std::string> functionDocstrings;
    std::unordered_map<std::string, llvm::Constant*> functionDocConstants;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> methodDocstringsBySym;
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::Constant*>> methodDocConstantsBySym;
    std::unordered_map<std::string, std::string> moduleDocstrings;
    std::unordered_map<std::string, llvm::Constant*> moduleDocConstants;
    struct DeferredClassInit {
        std::string className;
        std::string classSymPrefix;
        std::string owningModule;
        llvm::GlobalVariable* descriptorGlobal;
        llvm::Function* deallocFn;
        llvm::GlobalVariable* classIdGlobal;
        llvm::Function* traverseFn;
        llvm::Function* clearFn;
        llvm::Function* markSharedFn;
    };
    std::vector<DeferredClassInit> deferredClassInits;
    std::unordered_map<std::string, std::string> varClassNames;
    std::unordered_map<std::string, Type::Kind> varListElemKinds;
    std::unordered_set<std::string> varListElemIsType;
    std::unordered_set<std::string> varDictValueIsType;
    std::unordered_map<std::string, Type::Kind> varDictValueKinds;
    std::unordered_map<std::string, Type::Kind> varDictKeyKinds;
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldListElemKindsBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldDictValueKindsBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldDictKeyKindsBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldListElemClassNameBySym;

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldClassNameBySym;
    std::unordered_map<std::string, std::string> varListElemClassName;
    std::unordered_map<std::string, llvm::FunctionType*> varListElemCallableType;
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::FunctionType*>>
            classFieldListElemCallableTypeBySym;
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::FunctionType*>>
            classFieldCallableTypeBySym;
    std::unordered_map<std::string, std::string> classParentNamesBySym;
    std::unordered_map<std::string, std::vector<std::pair<std::string, Expr*>>> classPerInstanceDefaultsBySym;
    std::unordered_map<std::string, std::string> methodReturnClassNames;
    std::unordered_map<std::string, std::string> funcReturnClassNames;
    std::unordered_map<std::string, Type::Kind> methodReturnKinds;

    std::unordered_map<std::string, llvm::GlobalVariable*> classDescriptorGlobalsBySym;
    bool resolvingCallTarget = false;

    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classMethodVtableIndicesBySym;
    std::unordered_map<std::string, std::vector<std::string>> classVtableMethodOrderBySym;

    std::vector<const ContractDecl*> contractDeclsInOrder;
    std::set<const ContractDecl*> contractDeclSeen;
    std::unordered_set<std::string> contractTypeNames;
    std::map<std::pair<const ContractDecl*, std::string>, unsigned> contractMethodSlots;
    bool contractSlotsAssigned = false;
    void collectContracts(dragon::Module& mod);
    void assignContractSlots();
    bool emitContractMethodCall(CodeGen& cg, CallExpr& node, AttributeExpr& attr);

    std::unordered_map<std::string, std::vector<std::string>> classOwnMethodsBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, uint8_t>> classMethodKindsBySym;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, llvm::Function*>> classMethodBoundThunksBySym;

    std::unordered_map<std::string, std::unordered_set<std::string>> classPropertiesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classPropertySettersBySym;

    static std::string userFuncName(const std::string& name) {
        if (name == "main") return "_dragon_user_main";
        return name;
    }

    static std::string mangleFunc(const std::string& modName,
                                   const std::string& funcName) {
        if (funcName == "main") return "_dragon_user_main";
        if (modName.empty()) return funcName;
        std::string out;
        out.reserve(modName.size() + funcName.size() + 2);
        for (char c : modName) out += (c == '.') ? '_' : c;
        out += "__";
        out += funcName;
        return out;
    }

    std::string currentModuleName;

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedFuncAliasesByModule;

    std::string lookupImportedAlias(const std::string& bareName) const {
        auto modIt = importedFuncAliasesByModule.find(currentModuleName);
        if (modIt == importedFuncAliasesByModule.end()) return "";
        auto nameIt = modIt->second.find(bareName);
        if (nameIt == modIt->second.end()) return "";
        return nameIt->second;
    }

    std::string resolveCalleeSymbol(const std::string& name) const {
        std::string aliasSym = lookupImportedAlias(name);
        if (!aliasSym.empty()) {
            if (module && module->getFunction(aliasSym)) return aliasSym;
        }
        std::string mangled = mangleFunc(currentModuleName, name);
        if (module && module->getFunction(mangled)) return mangled;
        return userFuncName(name);
    }

    static std::string mangleClass(const std::string& modName,
                                    const std::string& className) {
        if (modName.empty()) return className;
        std::string out;
        out.reserve(modName.size() + className.size() + 2);
        for (char c : modName) out += (c == '.') ? '_' : c;
        out += "__";
        out += className;
        return out;
    }

    static std::string mangleGlobal(const std::string& modName,
                                     const std::string& varName) {
        return mangleClass(modName, varName);
    }

    std::unordered_map<std::string, std::string> classOwningModule;

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedClassAliasesByModule;

    std::string lookupImportedClassAlias(const std::string& bareName) const {
        auto modIt = importedClassAliasesByModule.find(currentModuleName);
        if (modIt == importedClassAliasesByModule.end()) return "";
        auto nameIt = modIt->second.find(bareName);
        if (nameIt == modIt->second.end()) return "";
        return nameIt->second;
    }

    std::string resolveClassOwningModule(const std::string& bareName) const {
        return resolveClassOwningModuleFrom(currentModuleName, bareName);
    }

    std::string resolveClassOwningModuleFrom(const std::string& fromModule,
                                             const std::string& bareName) const {
        auto modIt = importedClassAliasesByModule.find(fromModule);
        if (modIt != importedClassAliasesByModule.end()) {
            auto nameIt = modIt->second.find(bareName);
            if (nameIt != modIt->second.end()) return nameIt->second;
        }
        if (module) {
            std::string mangled = mangleClass(fromModule, bareName);
            if (module->getFunction(mangled + "_new") ||
                module->getFunction(mangled + "_new_0") ||
                module->getFunction(mangled + "___init__") ||
                module->getFunction(mangled + "___init___0")) {
                return fromModule;
            }
        }
        auto cmIt = classOwningModule.find(bareName);
        if (cmIt != classOwningModule.end()) return cmIt->second;
        return fromModule;
    }

    std::string classSymPrefix(const std::string& bareName) const {
        return mangleClass(resolveClassOwningModule(bareName), bareName);
    }

    std::string classSym(const std::string& name) const {
        return (classNames.count(name) || classOwningModule.count(name))
                   ? classSymPrefix(name) : name;
    }

    llvm::Function* methodFromClassType(const ClassType* ct,
                                        const std::string& method) const {
        for (int guard = 0; ct && module && guard < 256; ++guard) {
            auto* f = module->getFunction(
                mangleClass(ct->definingModule, ct->name) + "_" + method);
            if (f) return f;
            ct = (ct->parentClass && ct->parentClass->kind() == Type::Kind::Class)
                     ? static_cast<const ClassType*>(ct->parentClass.get())
                     : nullptr;
        }
        return nullptr;
    }

    bool methodIsOverridden(const std::string& baseClass,
                            const std::string& method) const;

    llvm::Function* resolveMethodFunction(
        const std::string& owningModule,
        const std::string& className,
        const std::string& methodName,
        std::string* resolvedSymbol = nullptr) const;

    std::unordered_map<std::string, std::string> varClassOwningModule;

    std::unordered_set<std::string> knownNonNeg;

    bool isExprDefinitelyNonNeg(Expr* e) const;

    std::unordered_map<std::string, llvm::GlobalVariable*> utf8LiteralGlobals;
    std::vector<std::string> utf8LiteralOrder;

    std::unordered_map<std::string, llvm::GlobalVariable*> asciiLiteralGlobals;

    std::vector<std::string> templateContextStack;

    std::unordered_map<std::string, llvm::Value*> sqlCanonicalGlobals;

    llvm::Value* internSqlCanonical(const std::string& canon) {
        bool ascii = true;
        for (unsigned char c : canon) { if (c >= 0x80) { ascii = false; break; } }
        if (!ascii) return emitStringLiteralBytes(canon);
        auto it = sqlCanonicalGlobals.find(canon);
        if (it != sqlCanonicalGlobals.end()) return it->second;
        llvm::Value* g = builder->CreateGlobalString(canon, ".sql.canon");
        sqlCanonicalGlobals[canon] = g;
        return g;
    }

    uint64_t sqlCanonicalHash(const std::string& s) const {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) { h ^= (uint64_t)c; h *= 0x100000001b3ULL; }
        return h;
    }

    std::vector<llvm::Value*> templateBlockBufferStack;

    std::unordered_map<std::string, std::unordered_map<std::string, llvm::GlobalVariable*>> staticFieldGlobalsBySym;
    std::unordered_set<std::string> staticMethods;

    std::unordered_map<std::string, size_t> classCtorCountBySym;
    std::unordered_map<std::string, std::vector<std::pair<size_t, int>>> classCtorAritiesBySym;

    std::unordered_map<std::string, llvm::GlobalVariable*> moduleGlobals;
    std::unordered_map<std::string, VarKind> moduleGlobalKinds;

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedGlobalAliasesByModule;

    struct GlobalClassBinding {
        std::string className;
        std::string owningModule;
    };
    std::unordered_map<std::string, GlobalClassBinding> moduleGlobalClassNames;
    std::unordered_set<std::string> entryGlobalsAwaitingInit;
    llvm::Function* mainFunction = nullptr;
    size_t moduleBodyScopeDepth = 0;
    bool isDragonFile = false;
    std::vector<dragon::Module*> depModulePtrs;
    dragon::Module* entryModulePtr = nullptr;

    std::unordered_set<std::string> globalDeclaredVars;
    std::unordered_set<std::string> nonlocalDeclaredVars;
    std::unordered_map<std::string, llvm::GlobalVariable*> nonlocalProxyGlobals;

    std::set<std::string> externLibs;

    struct DeferredStaticInit {
        Expr* valueExpr;
        llvm::GlobalVariable* gv;
    };
    std::vector<DeferredStaticInit> deferredStaticInits;

    bool needsPthread = false;

    std::unordered_map<std::string, std::vector<VarKind>> funcParamKinds;
    // Aligned with funcParamKinds (methods include self at 0), true for
    // `own p: T` params. The caller must not drain an owned temp bound to one: ownership transfers, and caller-drain + callee-release double-freed it (A/B-proven, fresh-temp-exemption probe).
    std::unordered_map<std::string, std::vector<bool>> funcParamOwns;
    bool paramIsOwn(const std::string& funcName, unsigned idx) {
        auto it = funcParamOwns.find(funcName);
        return it != funcParamOwns.end() && idx < it->second.size() &&
               it->second[idx];
    }
    std::unordered_map<std::string, std::vector<bool>> funcCallableParam;

    std::unordered_set<std::string> externFuncNames;
    // FFI v0: extern "C" args are borrowed for the call, and a managed return is a fresh
    // +1, never aliasing an arg; owned temps passed to a managed-typed param drain like any borrow callee (stdlib http leaked one string per header without this). Declared `ptr` return opts out (may alias an arg: leak-over-UAF). Members: externs whose return isn't `ptr`.
    std::unordered_set<std::string> externDrainableFuncs;

    std::unordered_map<std::string, std::vector<Expr*>> funcParamDefaults;

    std::unordered_map<std::string, std::string> funcDefiningModule;

    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;

    std::unordered_map<std::string, std::vector<VarKind>> unionMemberKinds;

    std::unordered_map<std::string, llvm::FunctionType*> callableTypes;

    struct NestedAliasInfo {
        llvm::Function* fn;
        llvm::FunctionType* userFnType;
        llvm::Value* envValue;
    };
    std::unordered_map<std::string, NestedAliasInfo> nestedFunctionAliases;

    llvm::FunctionType* lastClosureCallableType = nullptr;

    bool lastValueIsType = false;

    std::unordered_map<std::string, llvm::GlobalVariable*> decoratedFunctions;

    void preregisterDecoratedFunction(FunctionDecl& node);

    struct VarArgInfo {
        size_t numRegularParams = 0;
        bool hasVarArg = false;
        bool hasKwArg = false;
        std::string varArgName;
        std::string kwArgName;
        int64_t varArgElemTag = 0;
        bool    varArgElemIsAny = false;
    };
    std::unordered_map<std::string, VarArgInfo> funcVarArgInfo;

    std::string lastDynConstructClassName;

    bool needsSqlite3 = false;

    bool needsPcre2 = false;

    bool needsMbedtls = false;

    bool needsZ = false;
    bool needsZstd = false;

    bool needsWebview = false;

    std::unordered_map<std::string, std::set<std::string>> classDunderMethodsBySym;

    std::string resolveExprClassName(Expr* expr);

    static bool annAssignIsDeque(AnnAssignStmt* ann) {
        if (!ann) return false;
        if (auto* gt = dynamic_cast<GenericTypeExpr*>(ann->annotation.get())) {
            if (auto* gb = dynamic_cast<NamedTypeExpr*>(gt->base.get()))
                if (gb->name == "deque") return true;
        } else if (auto* nt = dynamic_cast<NamedTypeExpr*>(ann->annotation.get())) {
            if (nt->name == "deque") return true;
        }
        if (auto* cv = dynamic_cast<CallExpr*>(ann->value.get()))
            if (auto* cn = dynamic_cast<NameExpr*>(cv->callee.get()))
                return cn->name == "deque";
        return false;
    }

    bool isLockExpr(Expr* e);

    VarKind resolveExprVarKind(Expr* expr);

    bool hasDunder(const std::string& className, const std::string& dunder) {
        std::string cls = classSym(className);
        while (!cls.empty()) {
            auto it = classDunderMethodsBySym.find(cls);
            if (it != classDunderMethodsBySym.end() && it->second.count(dunder))
                return true;
            auto pit = classParentNamesBySym.find(cls);
            cls = (pit != classParentNamesBySym.end()) ? pit->second : "";
        }
        return false;
    }

    std::string findDunderClass(const std::string& className, const std::string& dunder) {
        std::string cls = classSym(className);
        while (!cls.empty()) {
            auto it = classDunderMethodsBySym.find(cls);
            if (it != classDunderMethodsBySym.end() && it->second.count(dunder))
                return cls;
            auto pit = classParentNamesBySym.find(cls);
            cls = (pit != classParentNamesBySym.end()) ? pit->second : "";
        }
        return "";
    }

    llvm::Value* callDunder(const std::string& className, const std::string& dunder,
                            llvm::Value* self, const std::vector<llvm::Value*>& extraArgs = {});

    llvm::Value* emitDunderCall(llvm::Function* func, const std::string& dunder,
                                llvm::Value* self,
                                const std::vector<llvm::Value*>& extraArgs = {});

    llvm::Value* toBool(llvm::Value* val, Expr* exprNode = nullptr);

    void init();

    void pushScope() { scopes.push_back({}); }
    void popScope() { if (!scopes.empty()) scopes.pop_back(); }

    static bool isHeapKind(VarKind k) {
        return k == VarKind::Str || k == VarKind::List || k == VarKind::Dict ||
               k == VarKind::Tuple || k == VarKind::Set ||
               k == VarKind::File || k == VarKind::ClassInstance || k == VarKind::Generator ||
               k == VarKind::Deque ||
               k == VarKind::Closure ||
               k == VarKind::Union;
    }

    static bool envCaptureIsCyclic(VarKind kind, bool isCellRelay) {
        if (isCellRelay) return true;
        return kind == VarKind::List || kind == VarKind::Dict ||
               kind == VarKind::Tuple || kind == VarKind::Set ||
               kind == VarKind::ClassInstance || kind == VarKind::Generator ||
               kind == VarKind::Deque || kind == VarKind::Closure;
    }

    struct EnvCaptureDesc { VarKind kind; bool isCellRelay; };

    llvm::Function* emitEnvGcFn(const std::string& baseName,
                                llvm::StructType* envStructType,
                                const std::vector<EnvCaptureDesc>& caps);

    bool isOwnedStrResult(llvm::Value* v);

    bool isBorrowedStrReturnerName(const std::string& name);

    bool isOwnedPtrResult(llvm::Value* v);

    bool isOwnedBoxResult(llvm::Value* v);

    static int64_t typeKindToTag(Type::Kind k);

    static int64_t varKindToTag(VarKind vk);

    int64_t pendingDictCheckTag = -1;

    int64_t pendingListViewElemTag = kNoListElemCheck;

    Type::Kind resolveDictKeyKind(Expr* expr);

    bool dictKeyUsesIntEngine(Expr* expr) {
        Type::Kind k = resolveDictKeyKind(expr);
        return k == Type::Kind::Int || k == Type::Kind::Float;
    }

    llvm::Value* emitFloatDictKeyBits(llvm::Value* key) {
        if (key->getType() == i64Type)
            key = builder->CreateSIToFP(key, f64Type, "fkey.widen");
        if (key->getType() != f64Type) return key;
        auto* isNan = builder->CreateFCmpUNO(key, key, "fkey.isnan");
        auto* bits = builder->CreateBitCast(key, i64Type, "fkey.bits");
        auto* noNegZero = builder->CreateSelect(
            builder->CreateICmpEQ(
                bits, llvm::ConstantInt::get(i64Type, 0x8000000000000000ULL),
                "fkey.isnegz"),
            llvm::ConstantInt::get(i64Type, 0), bits);
        return builder->CreateSelect(
            isNan, llvm::ConstantInt::get(i64Type, 0x7FF8000000000000LL),
            noNegZero, "fkey.norm");
    }

    Type::Kind resolveDictValueKind(Expr* expr);

    static int64_t typeKindToElemTag(dragon::Type::Kind k);

    static int64_t taskResultReleaseTag(dragon::Type::Kind k) {
        switch (k) {
            case Type::Kind::Str:      return TAG_STR;
            case Type::Kind::Bytes:    return TAG_BYTES;
            case Type::Kind::List:
            case Type::Kind::Set:
            case Type::Kind::Tuple:    return TAG_LIST;
            case Type::Kind::Dict:     return TAG_DICT;
            case Type::Kind::Instance: return 7;
            case Type::Kind::Function: return TAG_CALLABLE;
            case Type::Kind::Task:     return TAG_TASK_HANDLE;
            default:                   return TAG_INT;
        }
    }

    std::string containerReprFn(Expr* e);

    std::string recordVarClassFromValue(const std::string& varName, Expr* value);

    int64_t getListElemTag(Expr* listExpr) {
        if (listExpr && listExpr->type) {
            if (auto* lt = dynamic_cast<ListType*>(listExpr->type.get())) {
                if (lt->elementType) return typeKindToElemTag(lt->elementType->kind());
            }
        }
        return TAG_INT;
    }

    static VarKind typeKindToVarKind(Type::Kind k);

    llvm::Type* typeKindToLLVM(Type::Kind k) const;

    static bool isHeapTypeKind(Type::Kind k);

    Type::Kind getIterableElementKind(Expr* iterable) {
        // The receiver's own resolved type is authoritative when concrete: preferred over
        // varListElemKinds, a program-wide bare-name-keyed map never cleared between functions, so a same-named `list[SomeClass]` elsewhere left a stale entry causing this `out: list[int]` to route through dragon_list_append_ptr + incref on a raw i64 (SEGV/UAF). The map is only a fallback for unpinned element types (unannotated `out = []`).
        if (iterable && iterable->type) {
            if (auto* lt = dynamic_cast<ListType*>(iterable->type.get())) {
                if (lt->elementType &&
                    lt->elementType->kind() != Type::Kind::Unknown)
                    return lt->elementType->kind();
            }
            if (auto* dt = dynamic_cast<DictType*>(iterable->type.get())) {
                if (dt->keyType && dt->keyType->kind() != Type::Kind::Unknown)
                    return dt->keyType->kind();
            }
        }
        if (auto* iterName = dynamic_cast<NameExpr*>(iterable)) {
            auto it = varListElemKinds.find(iterName->name);
            if (it != varListElemKinds.end()) return it->second;
        }
        return Type::Kind::Int;
    }

    bool isBareDictIterable(Expr* expr);

    int64_t inferPtrValueTag(Expr* expr);

    llvm::Value* ensureHeapString(llvm::Value* val, Expr* expr) {
        if (options.gcMode != GCMode::RC) return val;
        bool isLiteral = false;
        if (auto* sl = dynamic_cast<StringLiteral*>(expr)) {
            isLiteral = !sl->isFString;
        } else if (auto* nameExpr = dynamic_cast<NameExpr*>(expr)) {
            isLiteral = (lookupVarKind(nameExpr->name) == VarKind::StrLiteral);
        }
        if (isLiteral && val->getType()->isPointerTy()) {
            auto* ptr = toI8Ptr(val);
            if (ptr)
                return builder->CreateCall(runtimeFuncs["dragon_string_dup"], {ptr}, "str.heap");
        }
        return val;
    }

    static constexpr int DCLEAN_STR      = 1;
    static constexpr int DCLEAN_CALLABLE = 2;
    static constexpr int DCLEAN_OBJ      = 3;
    static constexpr int DCLEAN_UNION    = 4;
    static constexpr int DCLEAN_DEFER_CALL = 5;

    int cleanupKindFor(VarKind k) {
        switch (k) {
            case VarKind::Str:     return DCLEAN_STR;
            case VarKind::Closure: return DCLEAN_CALLABLE;
            case VarKind::Union:   return DCLEAN_UNION;
            default:               return isHeapKind(k) ? DCLEAN_OBJ : 0;
        }
    }

    llvm::Value* cleanupValToI64(llvm::Value* v) {
        if (v->getType() == i64Type) return v;
        if (v->getType()->isPointerTy())
            return builder->CreatePtrToInt(v, i64Type, "clean.v");
        return builder->CreateZExtOrBitCast(v, i64Type);
    }

    llvm::GlobalVariable* activeFramesGlobal = nullptr;
    llvm::GlobalVariable* getActiveFramesGlobal() {
        if (!activeFramesGlobal) {
            activeFramesGlobal = new llvm::GlobalVariable(
                *module, llvm::Type::getInt32Ty(*context), false,
                llvm::GlobalValue::ExternalLinkage, nullptr,
                "__dragon_active_frames", nullptr,
                llvm::GlobalValue::InitialExecTLSModel);
        }
        return activeFramesGlobal;
    }

    llvm::Value* emitActiveFramesNonZero() {
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* af = builder->CreateLoad(i32Ty, getActiveFramesGlobal(), "active.frames");
        return builder->CreateICmpNE(af, llvm::ConstantInt::get(i32Ty, 0), "frame.live");
    }

    llvm::AllocaInst* createEntryAllocaI32(llvm::Function* func,
                                           const std::string& name, int initVal) {
        llvm::IRBuilder<> tmp(&func->getEntryBlock(), func->getEntryBlock().begin());
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* a = tmp.CreateAlloca(i32Ty, nullptr, name);
        tmp.CreateStore(llvm::ConstantInt::get(i32Ty, initVal), a);
        return a;
    }

    llvm::AllocaInst* findCleanupSlot(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->cleanupSlots.find(name);
            if (found != it->cleanupSlots.end()) return found->second;
            if (it->vars.count(name)) return nullptr;
        }
        return nullptr;
    }

    void emitCleanupPush(const std::string& name, llvm::Value* value,
                         int cleanupKind, llvm::Value* tagVal = nullptr);

    void emitCleanupUpdate(const std::string& name, llvm::Value* value,
                           llvm::Value* tagVal = nullptr);

    llvm::Value* emitCleanupPushTemp(llvm::Value* ptr, int cleanupKind);

    std::vector<llvm::Value*> pushArgTempCleanups(
        const std::vector<std::pair<llvm::Value*, VarKind>>& argTemps);

    void popArgTempCleanups(const std::vector<llvm::Value*>& bases);

    /// Rewinds the cleanup stack past a temp pushed by emitCleanupPushTemp; call at the
    /// loop's normal-exit decref site so a later unwind doesn't double-free the stale snapshot.
    void emitCleanupPopTemp(llvm::Value* baseAlloca);

    void emitScopeCleanupFor(Scope& scope);

    void emitScopeCleanup() {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        emitScopeCleanupFor(scopes.back());
    }

    void emitAllScopeCleanup() {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            emitScopeCleanupFor(*it);
        }
    }

    void emitScopeCleanupToDepth(size_t targetDepth) {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (size_t i = scopes.size(); i > targetDepth; --i) {
            emitScopeCleanupFor(scopes[i - 1]);
        }
    }

    void replayExitCleanup(CodeGen& cg, ExitCleanup& e) {
        if (!e.isWith) {
            for (auto* stmt : e.finallyBody) stmt->accept(cg);
        } else {
            for (auto& it : e.withItems) {
                if (it.isClassCtx) {
                    if (it.exitFn) emitDunderCall(it.exitFn, "__exit__", it.val);
                    else callDunder(it.className, "__exit__", it.val);
                    if (options.gcMode == GCMode::RC) {
                        if (it.subjectOwned)
                            builder->CreateCall(runtimeFuncs["dragon_decref"], {it.val});
                        if (it.enterResult && it.enterResult->getType()->isPointerTy())
                            builder->CreateCall(runtimeFuncs["dragon_decref"], {it.enterResult});
                    }
                } else if (it.isLock) {
                    builder->CreateCall(runtimeFuncs["dragon_lock_release"], {it.val});
                    if (it.isLockTemp)
                        builder->CreateCall(runtimeFuncs["dragon_lock_destroy"], {it.val});
                }
            }
        }
    }

    void emitEarlyExitCleanups(CodeGen& cg, size_t targetScopeDepth,
                               size_t ecDownTo) {
        size_t ecIdx = exitCleanupStack.size();
        auto blocked = [&]() {
            auto* bb = builder->GetInsertBlock();
            return !bb || bb->getTerminator();
        };
        const bool rc = options.gcMode == GCMode::RC;
        for (size_t depth = scopes.size(); depth > targetScopeDepth; --depth) {
            while (ecIdx > ecDownTo &&
                   exitCleanupStack[ecIdx - 1].scopeDepth >= depth) {
                if (blocked()) return;
                --ecIdx;
                replayExitCleanup(cg, exitCleanupStack[ecIdx]);
            }
            if (blocked()) return;
            if (rc) emitScopeCleanupFor(scopes[depth - 1]);
        }
        while (ecIdx > ecDownTo) {
            if (blocked()) return;
            --ecIdx;
            replayExitCleanup(cg, exitCleanupStack[ecIdx]);
        }
    }

    static bool containsYield(const std::vector<std::unique_ptr<Stmt>>& body) {
        struct YieldFinder : public DefaultASTVisitor {
            bool found = false;
            void visit(YieldExpr&) override { found = true; }
            void visit(FunctionDecl&) override {}
            void visit(ClassDecl&) override {}
        };
        YieldFinder finder;
        for (auto& stmt : body) {
            if (finder.found) break;
            stmt->accept(finder);
        }
        return finder.found;
    }

    static bool bodyReturnsValue(const std::vector<std::unique_ptr<Stmt>>& body) {
        struct RetFinder : public DefaultASTVisitor {
            bool found = false;
            void visit(ReturnStmt& r) override { if (r.value) found = true; }
            void visit(FunctionDecl&) override {}
            void visit(ClassDecl&) override {}
            void visit(LambdaExpr&) override {}
        };
        RetFinder finder;
        for (auto& stmt : body) {
            if (finder.found) break;
            stmt->accept(finder);
        }
        return finder.found;
    }

    llvm::Type* unannotatedReturnType(const std::vector<std::unique_ptr<Stmt>>& body) {
        return bodyReturnsValue(body) ? i64Type : voidType;
    }

    VarKind inferYieldKind(const std::vector<std::unique_ptr<Stmt>>& body);

    std::unordered_map<std::string, VarKind> generatorYieldKinds;

    std::unordered_map<std::string, VarKind> varGenYieldKinds;

    bool expandSpreadCallArgs(
        CodeGen& cg, llvm::Function* func, CallExpr& node,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName);

    bool packVarArgMethodArgs(
        CodeGen& cg, CallExpr& node, const std::string& methodFuncName,
        llvm::FunctionType* methodFuncType,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName);

    void bindParamSlotsFromDict(
        CodeGen& cg, llvm::Function* func, llvm::Value* d,
        std::vector<llvm::Value*>& args, const std::vector<size_t>& bindIdx,
        const std::vector<std::string>& paramNames, const std::string& dispName);

    void fillDefaultArgs(const std::string& funcName, llvm::Function* func,
                         std::vector<llvm::Value*>& args, CodeGen& cg,
                         std::vector<std::pair<llvm::Value*, VarKind>>* defaultTemps = nullptr);

    void emitAtomicIncref(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (!val->getType()->isPointerTy()) return;
        if (kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_str"], {val});
            builder->CreateCall(runtimeFuncs["dragon_incref_str_atomic"], {val});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_deep"], {val});
            builder->CreateCall(runtimeFuncs["dragon_incref_atomic"], {val});
        }
    }

    void emitFieldSharedBarrier(llvm::Value* objPtr, llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (!objPtr || !objPtr->getType()->isPointerTy()) return;
        if (!val || !val->getType()->isPointerTy()) return;
        auto* func = currentFunction;
        auto* i8Ty = llvm::Type::getInt8Ty(*context);
        auto* flagsPtr = builder->CreateInBoundsGEP(
            i8Ty, objPtr, llvm::ConstantInt::get(i64Type, 9), "obj.gcflags.p");
        auto* flags = builder->CreateLoad(i8Ty, flagsPtr, "obj.gcflags");
        auto* sharedBit = builder->CreateAnd(
            flags, llvm::ConstantInt::get(i8Ty, 0x04), "obj.shared.bit");
        auto* isShared = builder->CreateICmpNE(
            sharedBit, llvm::ConstantInt::get(i8Ty, 0), "obj.is.shared");
        auto* markBB = llvm::BasicBlock::Create(*context, "fieldshr.mark", func);
        auto* contBB = llvm::BasicBlock::Create(*context, "fieldshr.cont", func);
        builder->CreateCondBr(isShared, markBB, contBB);
        builder->SetInsertPoint(markBB);
        if (kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_str"], {val});
        } else if (kind == VarKind::Closure) {
            auto* asI64 = builder->CreatePtrToInt(val, i64Type, "fieldshr.clos.i64");
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_boxed"],
                {llvm::ConstantInt::get(i64Type, 10), asI64});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_deep"], {val});
        }
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
    }

    void emitMarkSharedGlobal(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (!val->getType()->isPointerTy()) return;
        if (kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_str"], {val});
        } else if (kind == VarKind::Closure) {
            auto* asI64 = builder->CreatePtrToInt(val, i64Type, "shr.clos.i64");
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_boxed"],
                {llvm::ConstantInt::get(i64Type, 10), asI64});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_deep"], {val});
        }
    }

    static bool isBorrowedHeapExpr(Expr* expr) {
        if (auto* cast = dynamic_cast<AsCastExpr*>(expr))
            return isBorrowedHeapExpr(cast->operand.get());
        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            if (dynamic_cast<SliceExpr*>(sub->index.get()) != nullptr)
                return false;
            if (sub->object && sub->object->type &&
                sub->object->type->kind() == Type::Kind::Str)
                return false;
            if (sub->object && dynamic_cast<CallExpr*>(sub->object.get()) &&
                sub->object->type &&
                sub->object->type->kind() == Type::Kind::Dict && expr->type) {
                switch (expr->type->kind()) {
                    case Type::Kind::Str:
                    case Type::Kind::Bytes:
                    case Type::Kind::List:
                    case Type::Kind::Dict:
                    case Type::Kind::Set:
                    case Type::Kind::Tuple:
                    case Type::Kind::Instance:
                        return false;
                    default: break;
                }
            }
            return true;
        }
        // A walrus target adopts its value's +1 (store skips the incref), so it hands the
        // consumer a borrow, like reading the name. Classifying it owned let a call site drain `takes(x := ...)` while x still held the pointer (A/B-proven UAF, test_rc_walrus.dr).
        if (dynamic_cast<WalrusExpr*>(expr) != nullptr) return true;
        if (auto* nm = dynamic_cast<NameExpr*>(expr)) {
            return !nm->isMoveMarked && !nm->isDubMarked;
        }
        if (auto* at = dynamic_cast<AttributeExpr*>(expr)) {
            return !dynamic_cast<CallExpr*>(at->object.get());
        }
        return false;
    }

    void emitNullSlot(llvm::AllocaInst* alloca) {
        if (alloca->getAllocatedType() == boxType)
            builder->CreateStore(llvm::Constant::getNullValue(boxType), alloca);
        else if (alloca->getAllocatedType()->isPointerTy())
            builder->CreateStore(
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(alloca->getAllocatedType())),
                alloca);
        else
            builder->CreateStore(
                llvm::ConstantInt::get(alloca->getAllocatedType(), 0), alloca);
    }

    void emitMoveOutIfMarked(Expr* value) {
        if (options.gcMode != GCMode::RC || !value) return;
        auto* nm = dynamic_cast<NameExpr*>(value);
        if (!nm || !nm->isMoveMarked) return;
        if (auto* alloca = lookupVar(nm->name)) {
            emitNullSlot(alloca);
            emitCleanupUpdate(
                nm->name,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(i8PtrType)),
                nullptr);
        }
    }

    void emitMoveOutSlots(CallExpr& node) {
        if (options.gcMode != GCMode::RC) return;
        for (auto& a : node.args) {
            auto* nm = dynamic_cast<NameExpr*>(a.get());
            if (!nm || !nm->isMoveMarked) continue;
            if (auto* alloca = lookupVar(nm->name)) {
                emitNullSlot(alloca);
                emitCleanupUpdate(
                    nm->name,
                    llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(i8PtrType)),
                    nullptr);
            }
        }
    }

    void increfBorrowedSetdefaultKey(Expr* keyExpr, llvm::Value* key) {
        if (options.gcMode != GCMode::RC || !key || !key->getType()->isPointerTy())
            return;
        bool keyIsLiteral =
            dynamic_cast<StringLiteral*>(keyExpr) ||
            (dynamic_cast<NameExpr*>(keyExpr) &&
             lookupVarKind(static_cast<NameExpr*>(keyExpr)->name) == VarKind::StrLiteral);
        if (!keyIsLiteral && isBorrowedHeapExpr(keyExpr))
            builder->CreateCall(runtimeFuncs["dragon_incref_str"], {key});
    }

    llvm::Value* toI8Ptr(llvm::Value* val) {
        if (!val || !val->getType()->isPointerTy()) return nullptr;
        if (val->getType() == i8PtrType) return val;
        return builder->CreateBitCast(val, i8PtrType);
    }

    void emitIncrefByKind(llvm::Value* val, VarKind kind);

    void emitDecrefByKind(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (kind == VarKind::Union) {
            if (val && val->getType() == boxType)
                emitUnionDecref(boxPayloadI64(val, "u.dec.p"),
                                boxTag(val, "u.dec.t"));
            return;
        }
        auto* ptr = toI8Ptr(val);
        if (!ptr) return;
        if (kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_decref_str"], {ptr});
        } else if (kind == VarKind::Closure) {
            builder->CreateCall(runtimeFuncs["dragon_decref_callable"], {ptr});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_decref"], {ptr});
        }
    }

    VarKind argTempDecrefKind(Expr* argExpr, VarKind paramKind, llvm::Value* rawVal) {
        if (options.gcMode != GCMode::RC) return VarKind::Other;
        if (paramKind == VarKind::Union) {
            // Callee-borrows: the caller drains owned Union/Any temps (adopting the +1 double-frees, test_rc_any_field.dr);
            // owned box results drain even when isBorrowedHeapExpr reads them borrowed (else one payload leaks per `f(anyVal[k])`), borrowed-box returners stay undrained.
            if (rawVal && rawVal->getType() == boxType)
                return isOwnedBoxResult(rawVal) ? VarKind::Union : VarKind::Other;
            if (isBorrowedHeapExpr(argExpr)) return VarKind::Other;
            return ownedTempDrainKind(argExpr, rawVal);
        }
        if (!isHeapKind(paramKind))
            return VarKind::Other;
        if (rawVal && rawVal->getType() == boxType)
            return isOwnedBoxResult(rawVal) ? VarKind::Union : VarKind::Other;
        if (isBorrowedHeapExpr(argExpr)) return VarKind::Other;
        if (paramKind == VarKind::Str && !isOwnedStrResult(rawVal))
            return VarKind::Other;
        return paramKind;
    }

    void collectArgTemp(const std::string& funcName, Expr* srcExpr,
                        llvm::Value* rawArg, unsigned paramIdx,
                        std::vector<std::pair<llvm::Value*, VarKind>>& out) {
        if (options.gcMode != GCMode::RC) return;
        if (externFuncNames.count(funcName)) {
            // A ptr-returning extern is not drainable (interior-pointer hazard). A drainable
            // extern is classified by the arg's own static type, not the declared param kind (the same C symbol can carry disagreeing arg types across modules, so classifying by param kind would drain a borrowed pointer -> UAF).
            if (!externDrainableFuncs.count(funcName)) return;
            VarKind dk = ownedTempDrainKind(srcExpr, rawArg);
            if (dk != VarKind::Other) out.emplace_back(rawArg, dk);
            return;
        }
        auto it = funcParamKinds.find(funcName);
        if (it == funcParamKinds.end() || paramIdx >= it->second.size()) return;
        if (paramIsOwn(funcName, paramIdx)) return;
        VarKind dk = argTempDecrefKind(srcExpr, it->second[paramIdx], rawArg);
        if (dk != VarKind::Other) out.emplace_back(rawArg, dk);
    }

    VarKind ownedTempDrainKind(Expr* e, llvm::Value* v) {
        if (options.gcMode != GCMode::RC) return VarKind::Other;
        if (!v || !e || !e->type || isBorrowedHeapExpr(e)) return VarKind::Other;
        switch (e->type->kind()) {
            case Type::Kind::Str:
                return isOwnedStrResult(v) ? VarKind::Str : VarKind::Other;
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
            case Type::Kind::Contract:
                return (v->getType()->isPointerTy() && isOwnedPtrResult(v))
                           ? VarKind::List : VarKind::Other;
            case Type::Kind::Unknown:
                if (v->getType()->isPointerTy() && dynamic_cast<CallExpr*>(e) &&
                    isOwnedPtrResult(v))
                    addError("internal error: owned call result has no static "
                             "type; its release would be silently skipped "
                             "(typechecker signature gap)", e->location());
                return VarKind::Other;
            default:
                return VarKind::Other;
        }
    }

    llvm::Value* trackBorrowTemp(Expr* e, llvm::Value* v,
                                 std::vector<std::pair<llvm::Value*, VarKind>>& sink) {
        VarKind k = ownedTempDrainKind(e, v);
        if (k != VarKind::Other) sink.emplace_back(v, k);
        return v;
    }

    void drainBorrowTemps(const std::vector<std::pair<llvm::Value*, VarKind>>& temps) {
        for (auto& [v, k] : temps) emitDecrefByKind(v, k);
    }

    void pushTempCleanupByKind(llvm::Value* v, VarKind k,
                               std::vector<llvm::Value*>& bases) {
        int ck = cleanupKindFor(k);
        if (ck == DCLEAN_STR || ck == DCLEAN_CALLABLE || ck == DCLEAN_OBJ)
            bases.push_back(emitCleanupPushTemp(v, ck));
    }

    llvm::Value* trackBorrowTempGuarded(Expr* e, llvm::Value* v,
                                 std::vector<std::pair<llvm::Value*, VarKind>>& sink,
                                 std::vector<llvm::Value*>& bases) {
        VarKind k = ownedTempDrainKind(e, v);
        if (k != VarKind::Other) {
            sink.emplace_back(v, k);
            pushTempCleanupByKind(v, k, bases);
        }
        return v;
    }

    llvm::Value* nativeToCellI64(llvm::Value* val, VarKind kind) {
        auto* ty = val->getType();
        if (ty == i64Type) return val;
        if (ty == i1Type) return builder->CreateZExt(val, i64Type, "cell.zext");
        if (ty == f64Type) return builder->CreateBitCast(val, i64Type, "cell.fbits");
        if (ty->isPointerTy()) return builder->CreatePtrToInt(val, i64Type, "cell.ptoi");
        if (ty->isIntegerTy()) {
            unsigned bits = ty->getIntegerBitWidth();
            if (bits < 64) return builder->CreateZExt(val, i64Type, "cell.zext");
            if (bits > 64) return builder->CreateTrunc(val, i64Type, "cell.trunc");
            return val;
        }
        return val;
    }

    llvm::Value* cellI64ToNative(llvm::Value* i64Val, VarKind kind);

    llvm::Value* emitCellAlloc(llvm::Value* valueI64, VarKind kind,
                                Type::Kind typeKind = Type::Kind::Unknown) {
        int64_t tag = typeKindToTag(typeKind);
        if (tag < 0) tag = varKindToTag(kind);
        if (tag < 0) tag = 0;
        int64_t holdsHeap = isHeapKind(kind) && kind != VarKind::Union ? 1 : 0;
        auto* tagC = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), tag);
        auto* heapC = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), holdsHeap);
        return builder->CreateCall(
            runtimeFuncs["dragon_cell_alloc"], {valueI64, tagC, heapC}, "cell");
    }

    llvm::Value* emitCellRead(llvm::AllocaInst* alloca, VarKind kind,
                              const std::string& name) {
        auto* cellPtr = builder->CreateLoad(i8PtrType, alloca, name + ".cell");
        auto* raw = builder->CreateCall(
            runtimeFuncs["dragon_cell_get"], {cellPtr}, name + ".raw");
        return cellI64ToNative(raw, kind);
    }

    void emitCellWrite(llvm::AllocaInst* alloca, VarKind kind,
                       llvm::Value* newVal, const std::string& name,
                       bool newIsBorrowed = true) {
        auto* cellPtr = builder->CreateLoad(i8PtrType, alloca, name + ".cell.w");
        if (newIsBorrowed) emitIncrefByKind(newVal, kind);
        auto* newI64 = nativeToCellI64(newVal, kind);
        auto* oldI64 = builder->CreateCall(
            runtimeFuncs["dragon_cell_set"], {cellPtr, newI64}, name + ".old");
        if (isHeapKind(kind) && kind != VarKind::Union) {
            auto* oldPtr = builder->CreateIntToPtr(oldI64, i8PtrType, name + ".old.p");
            emitDecrefByKind(oldPtr, kind);
        }
    }

    void emitUnionDecref(llvm::Value* val, llvm::Value* tag);

    void emitUnionIncref(llvm::Value* val, llvm::Value* tag);

    void storeWithRCOverwrite(llvm::Value* slotPtr, llvm::Type* slotValueType,
                              llvm::Value* newVal,
                              VarKind oldKind, VarKind newKind,
                              bool newIsBorrowed,
                              const std::string& name = "");

    bool consumeBorrowedSlot(const std::string& name);

    // Non-mutating peek of the mark consumeBorrowedSlot clears: is `name`'s innermost
    // binding currently borrowed? Gates the owned-str->StrLiteral downgrade guard: a literal store must keep an owned slot's cleanup kind Str, but never promote a borrowed slot (decref on a not-taken branch would UAF).
    bool isBorrowedSlot(const std::string& name) {
        if (name.empty()) return false;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            bool hasVar = it->vars.count(name) != 0;
            bool isBorrowed = it->borrowed.count(name) != 0;
            if (hasVar || isBorrowed)
                return isBorrowed;
        }
        return false;
    }

    void emitStrAppendInplace(llvm::Value* slotPtr, llvm::Value* cur,
                              llvm::Value* rhs, const std::string& name);

    template <typename EmitInline>
    llvm::Value* emitGuardedIntDivOp(llvm::Value* a, llvm::Value* b,
                                     const char* fallback, const char* label,
                                     EmitInline emitInline) {
        if (auto* cst = llvm::dyn_cast<llvm::ConstantInt>(b)) {
            if (!cst->isZero()) return emitInline(a, b);
            return builder->CreateCall(runtimeFuncs[fallback], {a, b}, label);
        }
        llvm::Value* zero = llvm::ConstantInt::get(i64Type, 0);
        llvm::Value* isZero = builder->CreateICmpEQ(b, zero, std::string(label) + ".dz");
        auto* func = currentFunction;
        auto* zeroBB = llvm::BasicBlock::Create(*context, std::string(label) + ".byzero", func);
        auto* okBB   = llvm::BasicBlock::Create(*context, std::string(label) + ".ok", func);
        auto* contBB = llvm::BasicBlock::Create(*context, std::string(label) + ".cont", func);
        builder->CreateCondBr(isZero, zeroBB, okBB);

        builder->SetInsertPoint(zeroBB);
        llvm::Value* zres = builder->CreateCall(runtimeFuncs[fallback], {a, b}, label);
        builder->CreateBr(contBB);
        auto* zeroExit = builder->GetInsertBlock();

        builder->SetInsertPoint(okBB);
        llvm::Value* ires = emitInline(a, b);
        builder->CreateBr(contBB);
        auto* okExit = builder->GetInsertBlock();

        builder->SetInsertPoint(contBB);
        auto* phi = builder->CreatePHI(i64Type, 2, label);
        phi->addIncoming(zres, zeroExit);
        phi->addIncoming(ires, okExit);
        return phi;
    }

    llvm::Value* emitIntMod(llvm::Value* a, llvm::Value* b) {
        return emitGuardedIntDivOp(a, b, "dragon_mod_int", "mod",
            [&](llvm::Value* n, llvm::Value* d) -> llvm::Value* {
                llvm::Value* zero = llvm::ConstantInt::get(i64Type, 0);
                llvm::Value* r = builder->CreateSRem(n, d, "mod.r");
                llvm::Value* nz = builder->CreateICmpNE(r, zero, "mod.nz");
                llvm::Value* neg = builder->CreateICmpSLT(
                    builder->CreateXor(r, d, "mod.xor"), zero, "mod.neg");
                llvm::Value* fix = builder->CreateAnd(nz, neg, "mod.fix");
                llvm::Value* radj = builder->CreateAdd(r, d, "mod.adj");
                return builder->CreateSelect(fix, radj, r, "mod");
            });
    }

    llvm::Value* emitIntFloorDiv(llvm::Value* a, llvm::Value* b) {
        return emitGuardedIntDivOp(a, b, "dragon_floordiv_int", "fdiv",
            [&](llvm::Value* n, llvm::Value* d) -> llvm::Value* {
                llvm::Value* zero = llvm::ConstantInt::get(i64Type, 0);
                llvm::Value* q = builder->CreateSDiv(n, d, "fdiv.q");
                llvm::Value* r = builder->CreateSRem(n, d, "fdiv.r");
                llvm::Value* rnz = builder->CreateICmpNE(r, zero, "fdiv.rnz");
                llvm::Value* neg = builder->CreateICmpSLT(
                    builder->CreateXor(n, d, "fdiv.xor"), zero, "fdiv.neg");
                llvm::Value* fix = builder->CreateAnd(rnz, neg, "fdiv.fix");
                llvm::Value* qm1 = builder->CreateSub(
                    q, llvm::ConstantInt::get(i64Type, 1), "fdiv.qm1");
                return builder->CreateSelect(fix, qm1, q, "fdiv");
            });
    }

    llvm::Value* emitIntAugOp(llvm::Value* cur, llvm::Value* rhs, TokenType op) {
        switch (op) {
            case TokenType::PLUS_EQUAL:         return builder->CreateAdd(cur, rhs, "aug.add");
            case TokenType::MINUS_EQUAL:        return builder->CreateSub(cur, rhs, "aug.sub");
            case TokenType::STAR_EQUAL:         return builder->CreateMul(cur, rhs, "aug.mul");
            case TokenType::PERCENT_EQUAL:      return emitIntMod(cur, rhs);
            case TokenType::DOUBLE_SLASH_EQUAL: return emitIntFloorDiv(cur, rhs);
            case TokenType::POWER_EQUAL:
                return builder->CreateCall(runtimeFuncs["dragon_pow_int"], {cur, rhs}, "pow");
            case TokenType::AMPERSAND_EQUAL:    return builder->CreateAnd(cur, rhs, "aug.and");
            case TokenType::PIPE_EQUAL:         return builder->CreateOr(cur, rhs, "aug.or");
            case TokenType::CARET_EQUAL:        return builder->CreateXor(cur, rhs, "aug.xor");
            case TokenType::LEFT_SHIFT_EQUAL:   return builder->CreateShl(cur, rhs, "aug.shl");
            case TokenType::RIGHT_SHIFT_EQUAL:  return builder->CreateAShr(cur, rhs, "aug.shr");
            default:                            return nullptr;
        }
    }

    llvm::Value* coerceToF64(llvm::Value* v) {
        if (v->getType() == i1Type) v = builder->CreateZExt(v, i64Type);
        if (v->getType() == i64Type) return builder->CreateSIToFP(v, f64Type);
        if (v->getType() == f64Type) return v;
        return nullptr;
    }

    llvm::Value* emitFloatAugOp(llvm::Value* cur, llvm::Value* rhs, TokenType op) {
        switch (op) {
            case TokenType::PLUS_EQUAL:         return builder->CreateFAdd(cur, rhs, "augf.add");
            case TokenType::MINUS_EQUAL:        return builder->CreateFSub(cur, rhs, "augf.sub");
            case TokenType::STAR_EQUAL:         return builder->CreateFMul(cur, rhs, "augf.mul");
            case TokenType::SLASH_EQUAL:        return builder->CreateFDiv(cur, rhs, "augf.div");
            case TokenType::DOUBLE_SLASH_EQUAL: return emitFloatFloorDiv(cur, rhs);
            case TokenType::PERCENT_EQUAL:      return emitFloatMod(cur, rhs);
            default:                            return nullptr;
        }
    }

    llvm::Value* emitFloatFloorDiv(llvm::Value* a, llvm::Value* b) {
        llvm::Value* q = builder->CreateFDiv(a, b, "ffdiv.q");
        llvm::Function* floorFn = llvm::Intrinsic::getOrInsertDeclaration(
            module.get(), llvm::Intrinsic::floor, {f64Type});
        return builder->CreateCall(floorFn, {q}, "ffdiv");
    }

    llvm::Value* emitFloatMod(llvm::Value* a, llvm::Value* b) {
        llvm::Value* zero = llvm::ConstantFP::get(f64Type, 0.0);
        llvm::Value* r = builder->CreateFRem(a, b, "fmod.r");
        llvm::Value* nz = builder->CreateFCmpONE(r, zero, "fmod.nz");
        llvm::Value* rNeg = builder->CreateFCmpOLT(r, zero, "fmod.rneg");
        llvm::Value* bNeg = builder->CreateFCmpOLT(b, zero, "fmod.bneg");
        llvm::Value* diff = builder->CreateXor(rNeg, bNeg, "fmod.signdiff");
        llvm::Value* fix = builder->CreateAnd(nz, diff, "fmod.fix");
        llvm::Value* radj = builder->CreateFAdd(r, b, "fmod.adj");
        return builder->CreateSelect(fix, radj, r, "fmod");
    }

    llvm::AllocaInst* lookupVar(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->vars.find(name);
            if (found != it->vars.end()) return found->second;
        }
        return nullptr;
    }

    llvm::AllocaInst* lookupVarInCurrentScope(const std::string& name) {
        if (scopes.empty()) return nullptr;
        auto found = scopes.back().vars.find(name);
        return found != scopes.back().vars.end() ? found->second : nullptr;
    }

    void setVar(const std::string& name, llvm::AllocaInst* alloca,
                 VarKind kind = VarKind::Other);

    bool isCellBacked(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->cellBacked.count(name)) return true;
            if (it->vars.count(name)) return false;
        }
        return false;
    }

    void markCellBacked(const std::string& name) {
        if (!scopes.empty()) scopes.back().cellBacked.insert(name);
    }

    void markStackAllocated(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->vars.count(name)) { it->stackAllocated.insert(name); return; }
        }
        if (!scopes.empty()) scopes.back().stackAllocated.insert(name);
    }

    bool lastWasStackInstance = false;

    std::unordered_set<const CallExpr*> stackAllocSites;

    std::unordered_set<const AnnAssignStmt*> detachableTaskDecls;

    std::unordered_set<std::string> stackEligibleClassesBySym;

    // RAII scope for per-variable metadata maps: snapshots at function entry, restores at
    // exit, so a body's entries die with it and a same-named local elsewhere can't inherit stale types (the documented cross-function SEGV/UAF family). Module-level entries made before lowering survive in the baseline.
    struct VarMetaScope {
        Impl& I;
        decltype(varClassNames) a;  decltype(varClassOwningModule) b;
        decltype(varListElemKinds) c;  decltype(varListElemIsType) d;
        decltype(varDictValueIsType) e;  decltype(varDictValueKinds) f;
        decltype(varDictKeyKinds) g;  decltype(varListElemClassName) h;
        decltype(varListElemCallableType) i;  decltype(varTypedDictClass) j;
        decltype(varIsPtrCallable) k;  decltype(knownNonNeg) l;
        decltype(callableTypes) m;  decltype(varGenYieldKinds) n;
        decltype(unionMemberKinds) o;
        explicit VarMetaScope(Impl& impl) : I(impl),
            a(impl.varClassNames), b(impl.varClassOwningModule),
            c(impl.varListElemKinds), d(impl.varListElemIsType),
            e(impl.varDictValueIsType), f(impl.varDictValueKinds),
            g(impl.varDictKeyKinds), h(impl.varListElemClassName),
            i(impl.varListElemCallableType), j(impl.varTypedDictClass),
            k(impl.varIsPtrCallable), l(impl.knownNonNeg),
            m(impl.callableTypes), n(impl.varGenYieldKinds),
            o(impl.unionMemberKinds) {}
        ~VarMetaScope() {
            I.varClassNames = std::move(a); I.varClassOwningModule = std::move(b);
            I.varListElemKinds = std::move(c); I.varListElemIsType = std::move(d);
            I.varDictValueIsType = std::move(e); I.varDictValueKinds = std::move(f);
            I.varDictKeyKinds = std::move(g); I.varListElemClassName = std::move(h);
            I.varListElemCallableType = std::move(i); I.varTypedDictClass = std::move(j);
            I.varIsPtrCallable = std::move(k); I.knownNonNeg = std::move(l);
            I.callableTypes = std::move(m); I.varGenYieldKinds = std::move(n);
            I.unionMemberKinds = std::move(o);
        }
    };

    VarKind lookupVarKind(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->varKinds.find(name);
            if (found != it->varKinds.end()) return found->second;
        }
        std::string key = resolveGlobalKey(name);
        if (!key.empty()) {
            auto mgIt = moduleGlobalKinds.find(key);
            if (mgIt != moduleGlobalKinds.end()) return mgIt->second;
        }
        return VarKind::Other;
    }

    bool exprIsBytes(Expr* expr);

    std::string resolveGlobalKey(const std::string& name) const {
        auto modIt = importedGlobalAliasesByModule.find(currentModuleName);
        if (modIt != importedGlobalAliasesByModule.end()) {
            auto aIt = modIt->second.find(name);
            if (aIt != modIt->second.end() && moduleGlobals.count(aIt->second))
                return aIt->second;
        }
        std::string own = mangleGlobal(currentModuleName, name);
        if (moduleGlobals.count(own)) return own;
        return "";
    }

    std::string globalKeyOrOwn(const std::string& name) const {
        std::string k = resolveGlobalKey(name);
        return k.empty() ? mangleGlobal(currentModuleName, name) : k;
    }

    llvm::GlobalVariable* lookupModuleGlobal(const std::string& name) {
        std::string key = resolveGlobalKey(name);
        if (key.empty()) return nullptr;
        return moduleGlobals.find(key)->second;
    }

    const GlobalClassBinding* globalClassBindingFor(const std::string& name) {
        if (lookupVar(name)) return nullptr;
        std::string key = resolveGlobalKey(name);
        if (key.empty()) return nullptr;
        auto it = moduleGlobalClassNames.find(key);
        if (it != moduleGlobalClassNames.end()) return &it->second;
        return nullptr;
    }

    void bindGlobalClassVar(const std::string& globalKey,
                            const std::string& bareVarName, TypeExpr* typeExpr) {
        auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr);
        if (!named) return;
        std::string cn = resolveAnnotationClassName(named->name);
        if (cn.empty()) return;
        moduleGlobalClassNames[globalKey] = {cn, resolveClassOwningModule(cn)};
        varClassNames[bareVarName] = cn;
        varClassOwningModule[bareVarName] = resolveClassOwningModule(cn);
    }

    bool shouldUseModuleGlobal(const std::string& name) {
        (void)name;
        return true;
    }

    llvm::AllocaInst* createEntryAlloca(llvm::Function* func,
                                         const std::string& name,
                                         llvm::Type* type);

    std::string resolveAnnotationClassName(const std::string& name) const {
        if (classNames.count(name)) return name;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            std::string leaf = name.substr(dot + 1);
            if (classNames.count(leaf)) return leaf;
        }
        return "";
    }

    void bindClassVar(const std::string& varName, TypeExpr* typeExpr) {
        auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr);
        if (!named) return;
        std::string cn = resolveAnnotationClassName(named->name);
        if (cn.empty()) return;
        varClassNames[varName] = cn;
        varClassOwningModule[varName] = resolveClassOwningModule(cn);
    }

    std::string typeExprCanonicalName(TypeExpr* t) const;

    std::string genericInstanceClassName(TypeExpr* t) const {
        if (!dynamic_cast<GenericTypeExpr*>(t)) return "";
        std::string c = typeExprCanonicalName(t);
        return classNames.count(c) ? c : "";
    }

    Type::Kind typeExprToTypeKind(TypeExpr* typeExpr);

    VarKind typeExprToKind(TypeExpr* typeExpr);

    std::vector<VarKind> typeExprToUnionMembers(TypeExpr* typeExpr) {
        std::vector<VarKind> members;
        if (auto* ut = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
            for (auto& t : ut->types) {
                members.push_back(typeExprToKind(t.get()));
            }
        }
        return members;
    }

    std::string typeExprUnionClassName(TypeExpr* typeExpr) {
        if (auto* ut = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
            for (auto& t : ut->types) {
                if (auto* nm = dynamic_cast<NamedTypeExpr*>(t.get())) {
                    if (classNames.count(nm->name)) return nm->name;
                }
            }
        }
        return "";
    }

    TypeExpr* unionNicheMember(TypeExpr* typeExpr);

    llvm::Value* nativeToPayloadI64(llvm::Value* val) {
        auto* ty = val->getType();
        if (ty == i64Type) return val;
        if (ty == f64Type) return builder->CreateBitCast(val, i64Type, "box.payload.f");
        if (ty == i1Type)  return builder->CreateZExt(val, i64Type, "box.payload.b");
        if (ty->isPointerTy()) return builder->CreatePtrToInt(val, i64Type, "box.payload.p");
        if (ty->isIntegerTy()) return builder->CreateSExt(val, i64Type, "box.payload.i");
        return val;
    }

    llvm::Value* makeBox(llvm::Value* tag, llvm::Value* payloadNative) {
        llvm::Value* payloadI64 = nativeToPayloadI64(payloadNative);
        llvm::Value* box = llvm::UndefValue::get(boxType);
        box = builder->CreateInsertValue(box, tag, 0, "box.t");
        box = builder->CreateInsertValue(box, payloadI64, 1, "box");
        return box;
    }

    llvm::Value* makeBoxConstTag(int64_t tagConst, llvm::Value* payloadNative) {
        return makeBox(llvm::ConstantInt::get(i64Type, tagConst), payloadNative);
    }

    llvm::Value* boxTag(llvm::Value* box, const std::string& name = "tag") {
        return builder->CreateExtractValue(box, 0, name);
    }

    llvm::Value* boxPayloadI64(llvm::Value* box, const std::string& name = "payload") {
        return builder->CreateExtractValue(box, 1, name);
    }

    llvm::Value* boxPayloadAsKind(llvm::Value* box, VarKind k);

    int64_t binopOpcodeForToken(TokenType t) {
        switch (t) {
            case TokenType::PLUS: case TokenType::PLUS_EQUAL: return 0;
            case TokenType::MINUS: case TokenType::MINUS_EQUAL: return 1;
            case TokenType::STAR: case TokenType::STAR_EQUAL: return 2;
            case TokenType::SLASH: case TokenType::SLASH_EQUAL: return 3;
            case TokenType::DOUBLE_SLASH: case TokenType::DOUBLE_SLASH_EQUAL: return 4;
            case TokenType::PERCENT: case TokenType::PERCENT_EQUAL: return 5;
            case TokenType::POWER: case TokenType::POWER_EQUAL: return 6;
            default: return -1;
        }
    }

    llvm::Value* boxNativeOperand(CodeGen& cg, Expr* e, llvm::Value* v) {
        if (v->getType() == boxType) return v;
        llvm::Value* tag;
        if (v->getType() == i64Type)
            tag = llvm::ConstantInt::get(i64Type, TAG_INT);
        else if (v->getType() == f64Type)
            tag = llvm::ConstantInt::get(i64Type, TAG_FLOAT);
        else if (v->getType() == i1Type)
            tag = llvm::ConstantInt::get(i64Type, TAG_BOOL);
        else
            tag = emitTagForExpr(e, cg);
        return makeBox(tag, v);
    }

    llvm::Value* emitBoxBinop(CodeGen& cg, Expr* lExpr, llvm::Value* lhs,
                              Expr* rExpr, llvm::Value* rhs, int64_t opcode) {
        llvm::Value* boxA = boxNativeOperand(cg, lExpr, lhs);
        llvm::Value* boxB = boxNativeOperand(cg, rExpr, rhs);
        llvm::Value* res = builder->CreateCall(runtimeFuncs["dragon_box_binop"],
            {boxA, boxB, llvm::ConstantInt::get(i64Type, opcode)}, "box.binop");
        drainOwnedNativeBoxOperands(lhs, rhs);
        return res;
    }

    void drainOwnedNativeBoxOperands(llvm::Value* lhs, llvm::Value* rhs) {
        if (options.gcMode != GCMode::RC) return;
        for (llvm::Value* v : {lhs, rhs}) {
            if (v->getType() != boxType && v->getType()->isPointerTy() &&
                isOwnedPtrResult(v))
                builder->CreateCall(runtimeFuncs["dragon_decref"], {v});
        }
    }

    llvm::Value* emitBoxCmp(CodeGen& cg, Expr* lExpr, llvm::Value* lhs,
                            Expr* rExpr, llvm::Value* rhs, int64_t cmpOp) {
        llvm::Value* boxA = boxNativeOperand(cg, lExpr, lhs);
        llvm::Value* boxB = boxNativeOperand(cg, rExpr, rhs);
        llvm::Value* res = builder->CreateCall(runtimeFuncs["dragon_box_cmp"],
            {boxA, boxB, llvm::ConstantInt::get(i64Type, cmpOp)}, "box.cmp");
        drainOwnedNativeBoxOperands(lhs, rhs);
        return res;
    }

    static constexpr int64_t kNoListElemCheck =
        std::numeric_limits<int64_t>::min();

    llvm::Value* unboxBoxResultChecked(llvm::Value* box, llvm::Type* targetType,
                                       VarKind vk,
                                       int64_t wantListElemTag = kNoListElemCheck,
                                       Type::Kind staticKind = Type::Kind::Unknown);

    int64_t listViewWantElemTag(TypeExpr* ann);

    llvm::Value* containerSlotToNative(llvm::Value* raw, Type* elemType) {
        Type::Kind ek = elemType ? elemType->kind() : Type::Kind::Int;
        switch (ek) {
            case Type::Kind::Float:
                return builder->CreateBitCast(raw, f64Type, "spread.f64");
            case Type::Kind::Bool:
                return builder->CreateICmpNE(
                    raw, llvm::ConstantInt::get(i64Type, 0), "spread.bool");
            case Type::Kind::Str:      case Type::Kind::Bytes:
            case Type::Kind::List:     case Type::Kind::Dict:
            case Type::Kind::Set:      case Type::Kind::Tuple:
            case Type::Kind::Instance: case Type::Kind::Ptr:
                return builder->CreateIntToPtr(raw, i8PtrType, "spread.ptr");
            default:
                return raw;
        }
    }

    std::pair<llvm::Value*, llvm::Value*> boxArgTagPayload(
            Expr* argExpr, llvm::Value* val, bool takesOwnership);

    llvm::Value* emitTagForExpr(Expr* expr, CodeGen& cg);

    llvm::Type* typeExprToLLVM(TypeExpr* typeExpr);

    llvm::Value* coerceArgFromExpr(Expr* expr,
                                    llvm::Value* arg,
                                    llvm::Type* paramType) {
        if (paramType == boxType) {
            auto tp = boxArgTagPayload(expr, arg, false);
            return makeBox(tp.first, tp.second);
        }
        return coerceArg(arg, paramType);
    }

    llvm::Value* emitTagForExprNoCG(Expr* expr);

    llvm::Value* coerceArg(llvm::Value* arg, llvm::Type* paramType);

    llvm::Value* normalizeIntC(llvm::Value* val) {
        if (val->getType() == intcType)
            return builder->CreateSExt(val, i64Type, "intc_ext");
        return val;
    }

    llvm::Value* resultToI64(llvm::Value* res) {
        auto* ty = res->getType();
        if (ty == i64Type) return res;
        if (ty == voidType) return llvm::ConstantInt::get(i64Type, 0);
        if (ty == i1Type)  return builder->CreateZExt(res, i64Type, "res.i64");
        if (ty == f64Type) return builder->CreateBitCast(res, i64Type, "res.i64");
        if (ty->isPointerTy()) return builder->CreatePtrToInt(res, i64Type, "res.i64");
        if (ty->isIntegerTy()) return builder->CreateSExt(res, i64Type, "res.i64");
        return llvm::ConstantInt::get(i64Type, 0);
    }

    llvm::Value* taskResultFromI64(llvm::Value* rawI64, Type* resultType);

    llvm::StructType* makeSpawnArgsStructType(
        const std::vector<llvm::Type*>& argTypes,
        const std::string& name) {
        std::vector<llvm::Type*> fields;
        fields.push_back(i8PtrType);
        for (auto* t : argTypes) fields.push_back(t);
        return llvm::StructType::create(*context, fields, name);
    }

    llvm::Value* coerceToFieldType(llvm::Value* val, llvm::Type* fieldType) {
        if (val->getType() == fieldType) return val;
        if (fieldType == i64Type && val->getType()->isPointerTy())
            return builder->CreatePtrToInt(val, fieldType);
        if (fieldType->isPointerTy() && val->getType() == i64Type)
            return builder->CreateIntToPtr(val, fieldType);
        if (fieldType == f64Type && val->getType() == i64Type)
            return builder->CreateSIToFP(val, fieldType);
        if (fieldType == i64Type && val->getType() == f64Type)
            return builder->CreateBitCast(val, fieldType);
        if (fieldType == i64Type && val->getType() == i1Type)
            return builder->CreateZExt(val, fieldType);
        if (fieldType == i1Type && val->getType() == i64Type)
            return builder->CreateICmpNE(val, llvm::ConstantInt::get(i64Type, 0));
        return builder->CreateBitCast(val, fieldType);
    }

    llvm::Function* buildFireTrampoline(
        llvm::Function* targetFn,
        llvm::StructType* argsStructType,
        const std::vector<VarKind>& argKinds,
        const std::string& siteName,
        int64_t resultTag);

    void emitAsyncMethodWrapper(llvm::Function* wrapper,
                                llvm::Function* bodyFn,
                                const FunctionDecl& decl,
                                const std::string& methodSym);

    llvm::Function* buildDeferThunk(llvm::Function* targetFn,
                                    const std::string& siteName,
                                    int vtableIndex = -1);

    llvm::Function* buildGeneratorTrampoline(
        llvm::Function* bodyFn,
        llvm::StructType* argsStructType,
        const std::string& siteName);

    llvm::Function* buildGeneratorDecrefFn(
        llvm::StructType* argsStructType,
        const std::vector<VarKind>& argKinds,
        const std::string& siteName);

    void populateSpawnArgs(
        llvm::Value* argsAlloca,
        llvm::StructType* argsStructType,
        const std::vector<llvm::Value*>& userArgs);

    llvm::AllocaInst* bindListElemTyped(
        llvm::Function* func,
        llvm::Value* listVal,
        llvm::Value* idx,
        const std::string& varName,
        VarKind loopKind);

    llvm::AllocaInst* bindListElemByTypeKind(
        llvm::Function* func,
        llvm::Value* listVal,
        llvm::Value* idx,
        const std::string& varName,
        Type::Kind elemKind);

    llvm::Value* emitStringLiteralBytes(const std::string& bytes,
                                        const llvm::Twine& twine = "");

    std::string processEscapes(const std::string& raw, bool isRaw);

    llvm::Function* getOrDeclareRuntime(const std::string& name,
                                         llvm::FunctionType* funcType);

    void declareRuntimeFunctions();

    void addError(const std::string& msg, SourceLocation loc = {}) {
        diagnostics.push_back({CodeGenDiagnostic::Level::Error, loc, msg});
    }

    void runOptimizationPasses();

    llvm::Type* inferExprLLVMType(Expr* expr);

    void forwardDeclareFunctions(dragon::Module& mod);

    bool classLayoutPass = false;

    void forwardDeclareClasses(dragon::Module& mod);
    void synthesizeDataclassMethods(ClassDecl& node);

    void synthesizeEnumMethods(ClassDecl& node);
};

}

#endif
