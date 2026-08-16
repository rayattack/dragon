#ifndef DRAGON_CODEGEN_IMPL_H
#define DRAGON_CODEGEN_IMPL_H

/// Dragon CodeGen Private Implementation Header: the CodeGen::Impl struct shared across
/// all codegen TUs.

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

    // LLVM core
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Value stack: visitor methods push results here
    llvm::Value* lastValue = nullptr;

    // Variable storage: name -> alloca, tagged by VarKind (Str = heap DragonObjectHeader,
    // decref'd via dragon_decref_str; StrLiteral = no header, never decref'd; ClassInstance = GC header, decref via dragon_decref). D030 §5: VarKind::Bytes deleted, bytes now use List (dispatch via Type::Kind/typeKindToTag).
    enum class VarKind { Int, Float, Bool, Str, StrLiteral, List, Dict, Tuple, Set, File, ClassInstance, Generator, Type, Closure, Union, Deque, Other };

    struct Scope {
        std::unordered_map<std::string, llvm::AllocaInst*> vars;
        std::unordered_map<std::string, VarKind> varKinds;
        std::unordered_set<std::string> borrowed;  // params - don't decref at scope exit
        // D027.1: heap-boxed via DragonCell; the alloca holds the cell ptr, reads route
        // through dragon_cell_get, writes through dragon_cell_set. Set at both the cell-promoted definition site and any nested fn's env-load site.
        std::unordered_set<std::string> cellBacked;
        // B Phase 1 (escape analysis): class instances entry-alloca'd instead of heap.
        // Ordinary ClassInstance for field/method access, but never malloc'd/gc_tracked, so scope cleanup must not decref it; only non-escaping scalar-only-class locals land here.
        std::unordered_set<std::string> stackAllocated;
        // Task-detach tail: Task locals bound to `fire ...` that provably never escape or
        // get joined/awaited get dragon_vthread_detach'd at scope exit. Detach is idempotent with join (`joined` CAS), safe even if a later edit adds a join.
        std::unordered_set<std::string> detachOnExit;
        // docs/002 ADR 2.10: bare Lock locals are owned by their scope, destroyed at exit
        // (null-gated: `del` skips a deleted lock). Sound because OwnershipCheck bars every second-owner path (E8/E15/E16). Module-level Locks are not armed (globals live for the process).
        std::unordered_set<std::string> lockDestroyOnExit;
        // Exception-unwind cleanup (DragonCleanupStack): cleanupSlots maps an owned heap
        // local to the i32 alloca holding its cleanup slot index, so reassignment can refresh it. cleanupBaseAlloca holds the depth at this scope's first push; normal exit rewinds to it so a sibling exception can't re-free already-decref'd locals.
        std::unordered_map<std::string, llvm::AllocaInst*> cleanupSlots;
        llvm::AllocaInst* cleanupBaseAlloca = nullptr;
        // defer f(x) snapshots (defer.md): appended in source order, run in reverse (LIFO)
        // ahead of the RC decref pass on every exit edge so borrowed snapshots stay alive. argSlots holds the snapshot values; drainKinds[i] != Other means slot i owns a +1 released after the call (Other = an own param already consumed it).
        struct DeferEntry {
            llvm::Function* thunk = nullptr;      // void(i64*) per-site
            llvm::AllocaInst* argSlots = nullptr; // [max(argc,1) x i64]
            unsigned argc = 0;
            std::vector<VarKind> drainKinds;
        };
        std::vector<DeferEntry> deferred;
    };
    std::vector<Scope> scopes;

    // D027.1: walks a function body collecting `mutatedCapturedVars` from every nested
    // FunctionDecl/LambdaExpr, to find which outer locals need cell-promotion. Recurses through statement bodies so a `nonlocal` two levels deep still surfaces.
    void collectNestedMutatedCaptures(const std::vector<std::unique_ptr<Stmt>>& body,
                                      std::unordered_set<std::string>& out);
    void collectNestedMutatedCaptures(Stmt* s,
                                      std::unordered_set<std::string>& out);
    void collectNestedMutatedCaptures(Expr* e,
                                      std::unordered_set<std::string>& out);

    // B Phase 1 escape analysis (EscapeAnalysis.cpp): walks the entry module's top-level
    // statements + function bodies, recording each non-escaping `v: T = T(args)` ctor CallExpr* in `stackAllocSites` (conservative: any use beyond a plain `v.field` read disqualifies). The CallExpr fork applies the remaining gates (scalar-only class, single non-self-escaping ctor).
    void computeStackAllocSites(Module& entryModule,
                                const std::vector<Module*>& depModules);
    // isModuleTopLevel: direct children of the module body are module globals (whole-program
    // visibility), so they're never stack-allocated; only their nested blocks are analyzed. Function bodies and nested blocks get full candidate detection.
    void analyzeBlockForStackAlloc(const std::vector<std::unique_ptr<Stmt>>& stmts,
                                   bool isModuleTopLevel = false);
    // True if `name` is used in a way that lets the instance escape its
    // declaring block (or is rebound); default-escapes for unhandled nodes. A nested
    // def/lambda/fire/thread body referencing `name` is a capture => escape, and ctor
    // self-escape (target="self") is covered too.
    bool exprEscapes(Expr* e, const std::string& name);
    bool stmtEscapes(Stmt* s, const std::string& name);
    // Exhaustive "does `name` appear anywhere in this subtree" probe: the sound fallback
    // for nodes exprEscapes/stmtEscapes don't special-case, and for capture sites (lambda/fire/thread/nested def).
    bool nodeMentionsName(Expr* e, const std::string& name);
    bool nodeMentionsName(Stmt* s, const std::string& name);

    // Task-detach tail: does Task local `name` transfer out (return/store/pass/capture/
    // rebind) vs merely consumed (await/join, safe to also detach) or read (is_alive)? Distinct from exprEscapes/stmtEscapes so it never weakens stack-alloc; unrecognized mentions count as transfers.
    bool taskLocalTransferEscapes(Stmt* s, const std::string& name);
    std::unordered_set<std::string> cellPromotedLocals;

    // Current function being generated
    llvm::Function* currentFunction = nullptr;

    // Loop control: break/continue targets
    struct LoopInfo {
        llvm::BasicBlock* breakBlock;
        llvm::BasicBlock* continueBlock;
        size_t scopeDepth;  // scopes.size() when loop was entered (before body push)
        size_t tryFrameDepth = 0;  // tryFrameFuncs.size() at loop entry; break/continue
                                   // pop only try/with frames opened inside the loop body.
        size_t exitCleanupDepth = 0;  // exitCleanupStack.size() at loop entry; break/continue
                                      // replay only finally/with __exit__ cleanups opened inside the loop, not enclosing ones.
    };
    std::stack<LoopInfo> loopStack;
    std::vector<llvm::Function*> tryFrameFuncs;

    // Count of the current function's live try/with exception frames (the trailing run of
    // tryFrameFuncs equal to currentFunction). A `return` escapes all of them.
    size_t currentFnTryFrames() {
        size_t n = 0;
        for (auto it = tryFrameFuncs.rbegin(); it != tryFrameFuncs.rend(); ++it) {
            if (*it != currentFunction) break;
            ++n;
        }
        return n;
    }

    // Emits `n` dragon_exc_pop_frame calls at the current insertion point (no-op if
    // already terminated); used by return/break/continue to unwind the exception frames their jump bypasses.
    void emitExcFramePops(size_t n) {
        if (n == 0) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (size_t i = 0; i < n; ++i)
            builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
    }

    // One escaped-scope cleanup action for a `with` context manager: call __exit__ (class
    // CM) or release a lock. Carries the SSA context handle defined at with-entry, which dominates every early-exit point.
    struct WithCleanupItem {
        bool isClassCtx;
        bool isLock;
        std::string className;
        llvm::Value* val;
        llvm::Value* enterResult = nullptr;  // __enter__ result (class CMs); may == val
        llvm::Function* exitFn = nullptr;    // true-identity __exit__; null = name-resolved
        bool isLockTemp = false;  // `with Lock()`: an anonymous lock the `with` owns and destroys (not just releases) on exit.
        bool subjectOwned = true;  // false when the subject is a BORROW (local/attribute/
                                   // walrus): with-exit must not decref `val` (A/B-proven UAF, test_rc_with_subject.dr); the __enter__ result's +1 is always dropped.
    };

    // Unified exit-cleanup stack: a try/finally body or a with's __exit__/lock-release
    // set, pushed innermost-last; return/break/continue replay innermost-first before jumping so cleanups run on every exit edge. One stack (not two) so nested try/with interleave correctly.
    struct ExitCleanup {
        bool isWith = false;
        std::vector<Stmt*> finallyBody;          // isWith == false (owned by TryStmt)
        std::vector<WithCleanupItem> withItems;  // isWith == true
        llvm::Function* func = nullptr;          // owning function (depth isolation)
        // scopes.size() when this entry was pushed; early exits interleave exit-cleanup
        // replays with per-scope cleanup by nesting depth (emitEarlyExitCleanups), so a defer inside a try body runs before that try's finally.
        size_t scopeDepth = 0;
    };
    std::vector<ExitCleanup> exitCleanupStack;

    // Index below which exitCleanupStack entries belong to enclosing functions (an inline
    // nested fn/lambda/comprehension keeps its parent's frames); `return` replays only currentFunction's trailing run. Mirrors currentFnTryFrames.
    size_t currentFnExitCleanupBase() {
        size_t i = exitCleanupStack.size();
        while (i > 0 && exitCleanupStack[i - 1].func == currentFunction) --i;
        return i;
    }

    // Names of the exception vars bound by the except handlers currently being emitted
    // (innermost last); lets `raise e` recognize a re-raise, since the bound var only holds the message and the type must come from dragon_exc_get_type.
    std::vector<std::string> handlerExcVars;

    // Generator state: when compiling a generator body, this holds the gen pointer alloca
    llvm::AllocaInst* generatorPtr = nullptr;
    // Set of function names that are generators (contain yield)
    std::unordered_set<std::string> generatorFunctions;

    // D025: function names whose declared return type is `type` (a class value); callers
    // set VarKind::Type on the receiver. Classes are compile-time entities now, so constructing through or isinstance-ing such a value is a compile error (no runtime class-descriptor dispatch).
    std::unordered_set<std::string> funcReturnsType;

    // D025: functions declared to return `ptr` let callers call the result as a function
    // pointer (`dbl = get_doubler(); dbl(x)`). Tracked so the indirect-call fallback doesn't conflate with class-descriptor vars.
    std::unordered_set<std::string> funcReturnsPtr;

    // D027: functions returning a CLOSURE (capturing nested def/lambda, a heap DragonClosure
    // with an env) vs a bare fn pointer; both type as `Callable[...]` but dispatch differs (a closure unpacks into fn+env). Populated only when every return is provably a closure.
    std::unordered_set<std::string> funcReturnsClosure;

    // Predicate backing funcReturnsClosure: true iff `node` is `-> Callable[...]` and every
    // return is provably a closure. Must run from the forward-declaration pre-pass, since class method bodies emit before a free function's visit(FunctionDecl).
    bool functionReturnsClosure(FunctionDecl& node);

    // D025: variable/parameter names declared `ptr`. The bare-fn-pointer indirect-call
    // fallback in CallExpr.cpp needs this to avoid conflating with unannotated params that may carry class descriptors.
    std::unordered_set<std::string> varIsPtrCallable;

    // D024, post ADR-025: classes with user-defined decorators. Decorators are dropped
    // (would need runtime descriptor construction, which ADR-025 removed); constructing such a class is a compile error (CallExpr.cpp).
    // @dataclass / @staticmethod / @classmethod / @property / NamedTuple are
    // compile-time synthesis and are NOT tracked here.
    std::unordered_set<std::string> decoratedClassesBySym;
    // Per-class decorator AST expressions (raw pointers; AST owns them).
    std::unordered_map<std::string, std::vector<Expr*>> classDecoratorExprsBySym;

    // 6.18: @dataclass/NamedTuple synthesis: classNames here had __init__/__eq__/__repr__
    // auto-generated from field declarations; dataclassFieldNamesBySym holds the ordered field names those synthesized methods use.
    std::unordered_set<std::string> dataclassClassNamesBySym;
    std::unordered_map<std::string, std::vector<std::string>> dataclassFieldNamesBySym;

    // Enum synthesis (`from enum import Enum`): a class deriving Enum/IntEnum/StrEnum is
    // rewritten by synthesizeEnumMethods into singleton member instances. enumKindBySym picks equality semantics (Plain: pointer identity; Int/Str: value-compare); enumMemberNamesBySym holds member order.
    enum class EnumKind { Plain, Int, Str };
    std::unordered_map<std::string, EnumKind> enumKindBySym;          // className -> kind
    std::unordered_map<std::string, std::vector<std::string>> enumMemberNamesBySym;

    // Maps a VarKind for a list/dict element annotation to the Type::Kind used by
    // varListElemKinds/varDictValueKinds, mirroring Assign.cpp's per-VarKind switch.
    static Type::Kind elemVarKindToTypeKind(VarKind ek);

    // D025: marks `paramName` ptr-typed if annotated `ptr` (gates the bare-fn-pointer
    // indirect-call fallback); derives Callable[[A,B],R]'s LLVM FunctionType; and populates list[T]/dict[K,V] element-kind tables so for-in/subscript dispatch at the right native type.
    void trackPtrParam(const std::string& paramName, TypeExpr* typeExpr);

    /// Allocates a monomorphized list matching `elemTag` (and `isAny` for the box list),
    /// mirroring visit(ListExpr)'s variant selection. Defined in Collections.cpp; elemTag 0 + !isAny = legacy i64 DragonList.
    llvm::Value* emitNewTypedList(int64_t elemTag, bool isAny, llvm::Value* capVal);

    /// Appends an already-evaluated `val` to a list built by emitNewTypedList; `elemExpr`
    /// drives tag inference and the borrow/incref + ensureHeapString discipline. Defined in Collections.cpp.
    void emitTypedListAppend(llvm::Value* list, llvm::Value* val, Expr* elemExpr,
                             int64_t elemTag, bool isAny, CodeGen& cg);

    /// Builds an llvm::FunctionType from a Callable[[A, B], R] AST node. Used by
    /// trackPtrParam (params/locals) and the for-loop site (list[Callable[...]] element propagation).
    llvm::FunctionType* callableTypeExprToFnType(CallableTypeExpr* callable) {
        std::vector<llvm::Type*> pts;
        pts.reserve(callable->paramTypes.size());
        for (auto& pt : callable->paramTypes)
            pts.push_back(typeExprToLLVM(pt.get()));
        llvm::Type* rt = typeExprToLLVM(callable->returnType.get());
        return llvm::FunctionType::get(rt, pts, false);
    }

    // Cached LLVM types
    llvm::Type* i64Type = nullptr;
    llvm::Type* intcType = nullptr;  // C int (target-dependent: i16 on 16-bit, i32 elsewhere)
    llvm::Type* f64Type = nullptr;
    llvm::Type* i1Type = nullptr;
    llvm::Type* i8PtrType = nullptr;
    llvm::Type* voidType = nullptr;

    // TBAA metadata for alias analysis (enables LICM for inline list access)
    llvm::MDNode* tbaaRoot = nullptr;
    llvm::MDNode* tbaaListHeader = nullptr;  // list struct fields (data ptr, size)
    llvm::MDNode* tbaaListData = nullptr;    // list element array

    // D030 Phase 4: %dragon.box = { i64 tag (DragonValueTag), i64 payload (opaque 8-byte
    // storage, narrowed to its native type at consumption sites)}. {i64,i64} (not the doc's original {i8,i64}) is locked in: sysv ABI passes it in two registers, natural alignment, room for richer tag metadata later.
    llvm::StructType* boxType = nullptr;

    // Runtime function cache
    std::unordered_map<std::string, llvm::Function*> runtimeFuncs;

    // Lambda counter for unique names
    int lambdaCounter = 0;

    // Exception handling counter for unique block naming
    int excCounter = 0;

    // Per-for-loop counter so each loop's owned iterable temp gets a unique scope-cleanup
    // name. Two loops sharing the name "__iter" let the second setVar clobber the first, leaking one keys()/items()/comprehension temp per extra loop.
    int forIterCounter = 0;

    // Maps an exception class name to a hierarchical type code; codes are assigned so a
    // parent's children are contiguous, enabling range-based subtype matching (e.g. ArithmeticError catches ZeroDivisionError/OverflowError/FloatingPointError).
    int64_t excTypeCode(const std::string& name);

    // Check if a name is a built-in exception type
    bool isBuiltinExcName(const std::string& name);

    // Check if a name is any exception type (built-in or user-defined)
    bool isExcType(const std::string& name) {
        return isBuiltinExcName(name) ||
               userExcCodesBySym.count(classSymPrefix(name)) > 0;
    }

    // User-defined exception tracking
    int64_t userExcNextCode = 1000;
    std::unordered_map<std::string, int64_t> userExcCodesBySym;        // className -> code
    std::unordered_map<int64_t, int64_t> userExcParentCodes;      // childCode -> parentCode

    // Return the inclusive range [lo, hi] of type codes caught by a given
    // exception code. Parent exceptions have ranges spanning their children.
    std::pair<int64_t, int64_t> excTypeRange(int64_t code) {
        switch (code) {
            case 0:   return {0, 105};    // BaseException
            case 10:  return {10, 105};   // Exception
            case 20:  return {20, 23};    // ArithmeticError
            case 30:  return {30, 31};    // ImportError
            case 40:  return {40, 42};    // LookupError
            case 44:  return {44, 45};    // NameError
            case 50:  return {50, 61};    // OSError
            case 57:  return {57, 61};    // ConnectionError
            case 70:  return {70, 72};    // RuntimeError
            case 90:  return {90, 94};    // ValueError
            case 91:  return {91, 94};    // UnicodeError
            case 100: return {100, 105};  // Warning
            default:  return {code, code}; // Leaf - exact match
        }
    }

    // Modules resolved as .dr/.py files, from the compile-time import DAG.
    std::set<std::string> fileResolvedModules;

    // Class support: each class becomes an LLVM StructType with fields extracted from
    // __init__ (self.x = ...). Symbols: ClassName_new (malloc+init), ClassName___init__, ClassName_methodName.
    std::set<std::string> classNames;         // Known class names for constructor dispatch
    std::string currentClassName;              // Set when emitting class methods
    std::unordered_map<std::string, llvm::StructType*> classStructTypesBySym;
    // TypedDict: className -> {field -> Type::Kind}. Variables are VarKind::Dict at
    // runtime but access uses checked get with tags from typeKindToTag; stored as Type::Kind (not VarKind) so per-field bytes-ness survives the VarKind::Bytes deletion (D030 §5).
    std::set<std::string> typedDictClassesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> typedDictFieldKindsBySym;
    // Variable name -> TypedDict class name (so we know which schema to use)
    std::unordered_map<std::string, std::string> varTypedDictClass;
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classFieldIndicesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, llvm::Type*>> classFieldTypesBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, VarKind>> classFieldKindsBySym; // Phase 5: per-field VarKind for dealloc
    // Own (non-inherited) instance-field order per class, from the AST `instanceFieldOrder`
    // helper (same source TypeChecker fills ClassType::fieldOrder from). Drives positional `match` destructuring (`case Point(x, y)`); ancestors prepend via classParentNamesBySym.
    std::unordered_map<std::string, std::vector<std::string>> classFieldOrderBySym;
    std::unordered_map<std::string, llvm::GlobalVariable*> classIdGlobalsBySym; // Phase 5: class_id globals
    // Class docstrings, populated by visit(ClassDecl) when present; looked up at
    // dragon_class_descriptor_create time so the descriptor's `doc` field powers `Cls.__doc__`.
    std::unordered_map<std::string, std::string> classDocstringsBySym;
    // Function docstrings: keyed by mangleFunc(modName, funcName). Populated by
    // visit(FunctionDecl) when ClassDecl.docstring is present. Powers `f.__doc__`.
    std::unordered_map<std::string, std::string> functionDocstrings;
    // Cached `.rodata` i8* constants for function docstring bytes, built lazily on first
    // attribute-access; unused docstrings cost zero (never emitted).
    std::unordered_map<std::string, llvm::Constant*> functionDocConstants;
    // Method docstrings: className -> methodName -> docstring, powering `Cls.method.__doc__`
    // and `instance.method.__doc__`. Matched in Attributes.cpp on the AttrExpr(AttrExpr(...), "__doc__") chain, the only shape supported since methods aren't first-class values.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> methodDocstringsBySym;
    // Cached `.rodata` constants for method docstrings: className -> methodName.
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::Constant*>> methodDocConstantsBySym;
    // Module docstrings keyed by module name (entry module = "", matching
    // currentModuleName = ""); populated upfront in generate() from each Module.docstring.
    std::unordered_map<std::string, std::string> moduleDocstrings;
    // Cached `.rodata` i8* constants for module docstring bytes. Same lazy
    // shape as functionDocConstants.
    std::unordered_map<std::string, llvm::Constant*> moduleDocConstants;
    // Phase 5: deferred class registration for main() preamble
    struct DeferredClassInit {
        std::string className;       // bare class name (for metadata maps)
        std::string classSymPrefix;  // <mod>__<className> (for LLVM symbols)
        std::string owningModule;    // module that defined this class
        llvm::GlobalVariable* descriptorGlobal; // per-instance descriptor (bypass
                                                // last-wins on classDescriptorGlobalsBySym)
        llvm::Function* deallocFn;
        llvm::GlobalVariable* classIdGlobal;
        llvm::Function* traverseFn;  // Phase 5e: per-class traverse function
        llvm::Function* clearFn;     // Phase 5: per-class clear function (cycle collector)
        llvm::Function* markSharedFn; // D018: per-class SHARED-mark fn (BFS walker)
    };
    std::vector<DeferredClassInit> deferredClassInits;
    std::unordered_map<std::string, std::string> varClassNames;  // varName -> className
    std::unordered_map<std::string, Type::Kind> varListElemKinds; // varName -> list element Type::Kind
    // D025: vars whose declared element type is `type` - list[type] / dict[k, type].
    // Iterating yields VarKind::Type so callsites dispatch through descriptor.
    std::unordered_set<std::string> varListElemIsType;
    std::unordered_set<std::string> varDictValueIsType;
    // dict[K, V] value Type::Kind tracking, used by `for k, v in d.items()` to set v's
    // VarKind so later uses dispatch correctly. Mirrors varListElemKinds for lists.
    std::unordered_map<std::string, Type::Kind> varDictValueKinds;
    // D030 Phase 3.G: dict[K, V] key Type::Kind tracking; codegen routes subscript/`in`/
    // print/iteration on int-keyed dicts to dragon_dict_int_* instead of the str-keyed default (absent entry = str-keyed).
    std::unordered_map<std::string, Type::Kind> varDictKeyKinds;
    // className -> fieldName -> list element Type::Kind (for self.field list iterations)
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldListElemKindsBySym;
    // className -> fieldName -> dict value Type::Kind, mirroring varDictValueKinds so
    // `obj.field["k"]` routes to the typed runtime op (dragon_dict_get_str_ptr/_str_f64) instead of the polymorphic i64-returning one.
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldDictValueKindsBySym;
    // D030 Phase 3.G: class-field dict key Type::Kind, mirroring varDictKeyKinds for
    // self.<field> dicts.
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldDictKeyKindsBySym;
    // className -> fieldName -> element class name (for list[ClassName] field iterations)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldListElemClassNameBySym;

    // Class name of a class-instance field, populated when extractFields sees `self.x =
    // Foo(...)` or `self.x = param: Foo`. Read by resolveExprClassName(AttributeExpr) so `obj.x.field` resolves to Foo's struct layout.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldClassNameBySym;
    // varName -> element class name (for local list[ClassName] iterations)
    std::unordered_map<std::string, std::string> varListElemClassName;
    // Callable[[...], R] element typing for list iterations (varName or className.F ->
    // element FunctionType); `for f in xs` picks up callableTypes[f] from these maps so the callsite has a known signature, not the i64-default fallback.
    std::unordered_map<std::string, llvm::FunctionType*> varListElemCallableType;
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::FunctionType*>>
            classFieldListElemCallableTypeBySym;
    // Direct Callable[[A,B,...], R] field types (`handler: Callable[[Req,Res,Ctx], None]`),
    // recorded so `obj.handler(args)` builds the real FunctionType instead of an all-i64 signature, and appends the trailing env arg when the field holds a DragonClosure.
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::FunctionType*>>
            classFieldCallableTypeBySym;
    std::unordered_map<std::string, std::string> classParentNamesBySym; // className -> parentClassName
    // className -> (field, per-instance default-expr), persisted from the layout pre-pass
    // (visits every class in source order before any _new body) so emitNewBody applies inherited defaults regardless of source order. Expr* is AST-owned, valid for CodeGen's lifetime.
    std::unordered_map<std::string, std::vector<std::pair<std::string, Expr*>>> classPerInstanceDefaultsBySym;
    std::unordered_map<std::string, std::string> methodReturnClassNames; // "<classSym>_method" -> returnClassName
    std::unordered_map<std::string, std::string> funcReturnClassNames;   // top-level funcName -> returnClassName
    // "<classSym>_method" -> declared return Type::Kind, needed because `ptr` is overloaded
    // (str/list/dict/bytes/instance all lower to ptr); the AST kind disambiguates so callers pick the right VarKind, e.g. `for x in iter` binding x correctly when __next__() -> str.
    std::unordered_map<std::string, Type::Kind> methodReturnKinds;       // "<classSym>_method" -> Type::Kind

    // Decision 025: First-class class descriptors
    std::unordered_map<std::string, llvm::GlobalVariable*> classDescriptorGlobalsBySym; // className -> @ClassName__descriptor
    bool resolvingCallTarget = false; // true when visiting callee of a CallExpr (suppresses descriptor load)

    // Decision 026: Vtable support
    // className -> methodName -> vtable index (0-based)
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classMethodVtableIndicesBySym;
    // className -> ordered list of method names in vtable order
    std::unordered_map<std::string, std::vector<std::string>> classVtableMethodOrderBySym;

    // ADR 054 type contracts: coloring assigns every contract method a globally unique
    // vtable slot (base = the largest natural vtable), so a contract call is load-vtable + call-slot on a plain instance pointer. Keys are the ContractDecl* identity (D053), never bare names.
    std::vector<const ContractDecl*> contractDeclsInOrder;
    std::set<const ContractDecl*> contractDeclSeen;
    std::unordered_set<std::string> contractTypeNames;  // annotation mapping only
    std::map<std::pair<const ContractDecl*, std::string>, unsigned> contractMethodSlots;
    bool contractSlotsAssigned = false;
    void collectContracts(dragon::Module& mod);
    void assignContractSlots();
    bool emitContractMethodCall(CodeGen& cg, CallExpr& node, AttributeExpr& attr);

    // D033: method-name reflection. Each class's own (non-inherited) method names in
    // declaration order plus parallel kind bytes (0=instance,1=static,2=classmethod), populated in ImplInit's class-body scan and consumed at main-init to emit the __method_* globals.
    std::unordered_map<std::string, std::vector<std::string>> classOwnMethodsBySym;
    std::unordered_map<std::string, std::unordered_map<std::string, uint8_t>> classMethodKindsBySym;
    // D033 Phase 3: per-(class, method) bound-thunk fn, emitted alongside the method body
    // (signature = user args minus self + env). NULL entries are valid for static methods (which skip the bind path in dragon_getattr).
    std::unordered_map<std::string,
                       std::unordered_map<std::string, llvm::Function*>> classMethodBoundThunksBySym;

    // 4.1 @property: per-class set of property names whose getter is the same name.
    // Populated in ImplInit when scanning class bodies for FunctionDecl with isProperty=true.
    std::unordered_map<std::string, std::unordered_set<std::string>> classPropertiesBySym;
    // className -> propertyName -> mangled setter func name ("<propName>__setter")
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classPropertySettersBySym;

    /// Mangles user function names that collide with codegen-reserved symbols: "main"
    /// collides with the C entry point, so `def main()` becomes `_dragon_user_main`. All by-name resolution routes through here so the rename stays invisible at the source level.
    static std::string userFuncName(const std::string& name) {
        if (name == "main") return "_dragon_user_main";
        return name;
    }

    // Per-module symbol mangling for top-level function names: Dragon links every import
    // into one LLVM module, so `gzip.open`/`zstandard.open`/`tarfile.open` would collide on one `@open` without a module-path prefix. `_dragon_user_main` stays program-unique; unnamed (entry) modules keep the bare name.
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

    // The module currently being lowered; set/restored in CodeGen::generate around each
    // dependency and the entry module. Read by mangleFunc sites needing a same-module symbol with no AttributeExpr base to name the owning module.
    std::string currentModuleName;

    // Per-importing-module alias scope: importingModule -> (bareName -> mangled). `from os
    // import listdir` in module A binds only A's `listdir`; keyed by importing module so a single global map can't clobber same-named aliases from different modules.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedFuncAliasesByModule;

    // Resolves an alias under the current module's scope; empty string when none is in
    // effect. All readers (CallExpr/Expressions/Assign function-as-value) funnel through here for consistency.
    std::string lookupImportedAlias(const std::string& bareName) const {
        auto modIt = importedFuncAliasesByModule.find(currentModuleName);
        if (modIt == importedFuncAliasesByModule.end()) return "";
        auto nameIt = modIt->second.find(bareName);
        if (nameIt == modIt->second.end()) return "";
        return nameIt->second;
    }

    // Resolves a bare callee name to its LLVM symbol: alias -> mangleFunc(module, name) ->
    // userFuncName(name), picking the first candidate that exists. Used for lookups and for indexing side-channel maps (funcParamKinds etc.) keyed by mangled symbol so same-named stdlib functions don't clobber each other.
    std::string resolveCalleeSymbol(const std::string& name) const {
        std::string aliasSym = lookupImportedAlias(name);
        if (!aliasSym.empty()) {
            if (module && module->getFunction(aliasSym)) return aliasSym;
        }
        std::string mangled = mangleFunc(currentModuleName, name);
        if (module && module->getFunction(mangled)) return mangled;
        return userFuncName(name);
    }

    // Per-module mangling for class symbols, same shape as mangleFunc: prefix for every
    // class-related LLVM symbol (struct type, _new/___init__/_<method>, __vtable/__descriptor, dealloc/traverse/clear/mark_shared helpers). Entry-module (unnamed) classes keep the bare name.
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

    // Same `mod__name` scheme as classes/functions (entry stays bare); a
    // distinct entry point so global-keying sites grep cleanly.
    static std::string mangleGlobal(const std::string& modName,
                                     const std::string& varName) {
        return mangleClass(modName, varName);
    }

    // Bare class name -> owning module, populated in forwardDeclareClasses. Last-write-wins
    // on duplicate names; resolveClassOwningModule prefers a same-module probe over this map so same-module callers always win first.
    std::unordered_map<std::string, std::string> classOwningModule;

    // Per-importing-module class alias scope: importingMod -> (bareName -> owningMod).
    // `from b import Conflict` pins A's `Conflict` to module b regardless of same-named classes elsewhere. Mirrors importedFuncAliasesByModule.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedClassAliasesByModule;

    std::string lookupImportedClassAlias(const std::string& bareName) const {
        auto modIt = importedClassAliasesByModule.find(currentModuleName);
        if (modIt == importedClassAliasesByModule.end()) return "";
        auto nameIt = modIt->second.find(bareName);
        if (nameIt == modIt->second.end()) return "";
        return nameIt->second;
    }

    // Resolves which module owns a bare class name: imported alias, then a same-module
    // `<mod>__<className>_new` probe, then the global owning-module map (last-wins fallback), then currentModuleName.
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
            // Same-module probe - robust to last-write-wins on classOwningModule.
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

    // Returns the LLVM symbol prefix for a bare class name, resolved from the current
    // module's perspective. Use this for every emitter reaching a class-owned LLVM function or global.
    std::string classSymPrefix(const std::string& bareName) const {
        return mangleClass(resolveClassOwningModule(bareName), bareName);
    }

    // Idempotent sym: a bare known class (incl. TypedDicts, in classOwningModule but not
    // classNames) resolves via classSymPrefix; anything else passes through unchanged.
    std::string classSym(const std::string& name) const {
        return (classNames.count(name) || classOwningModule.count(name))
                   ? classSymPrefix(name) : name;
    }

    // TRUE-identity method resolution: walk the ClassType parent chain, each level
    // mangled with ITS definingModule - immune to same-named classes elsewhere.
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

    // D026 devirtualization gate: if no strict subclass of `baseClass` overrides `method`,
    // a call on a baseClass-typed receiver devirtualizes to a direct call; otherwise it must dispatch through the vtable. Exact because Dragon compiles whole-program: an override always gets its own `mangleClass(mod,sub)+"_"+method` symbol.
    bool methodIsOverridden(const std::string& baseClass,
                            const std::string& method) const;

    // Walks the inheritance chain from (owningModule, className), trying each level's
    // `mangleClass(mod,cls)+"_"+methodName` symbol; sets *resolvedSymbol on match. The single lookup point (CallMethods/Classes/Concurrency all use it) since a drifted per-caller copy miscompiles cross-module dispatch like `fire self._method()`.
    llvm::Function* resolveMethodFunction(
        const std::string& owningModule,
        const std::string& className,
        const std::string& methodName,
        std::string* resolvedSymbol = nullptr) const;

    // Per-instance owning module: var -> owning module of the class instance it holds.
    // Mirrors varClassNames so method dispatch picks the right `<owner>__<className>_<method>` symbol when two modules define same-named classes.
    std::unordered_map<std::string, std::string> varClassOwningModule;

    /// 6.12(B): a variable is in this set when its value is provably >= 0 (int literal,
    /// len()/abs(), or +/*/** of non-negative operands). Lets Attributes.cpp/Assign.cpp skip the `idx + (idx<0 ? size : 0)` correction; cleared on any not-provably-non-negative assignment.
    std::unordered_set<std::string> knownNonNeg;

    /// True when `e` is provably >= 0 from its AST shape. Recursive and purely structural,
    /// no flow-sensitive reasoning beyond the tracked `knownNonNeg` set.
    bool isExprDefinitelyNonNeg(Expr* e) const;

    // 4.7 PEP 393-lite: non-ASCII string literals. Each unique UTF-8 byte sequence gets
    // a module-level i8* global, lazily interned at the top of main() (one-shot, immortal); use sites just load it, zero per-access cost.
    std::unordered_map<std::string, llvm::GlobalVariable*> utf8LiteralGlobals;
    std::vector<std::string> utf8LiteralOrder;

    // ASCII string literals emit as immortal DragonString constants (real header +
    // len/kind/cap + NUL-terminated bytes), deduped by byte sequence; a pointer to `data` keeps dragon_is_heap_string/_len/_eq/decref reading an in-bounds header and makes incref/decref no-ops, at zero per-access/startup cost.
    std::unordered_map<std::string, llvm::GlobalVariable*> asciiLiteralGlobals;

    // D017 Phase 4.B: template content-type context stack, pushed/popped around
    // visit(TemplateExpr/TemplateFileExpr) so `:{ ... }` fragments inherit the parent's content type for auto-escape/wrapping. Empty stack means no enclosing template (top-level stays untyped str).
    std::vector<std::string> templateContextStack;

    // D032: interned canonical `$$N` query texts for `template[SQL]` sites, one ASCII
    // global per unique canonical so structurally identical sites share a pointer for prepared-statement cache pointer-compare. Rare non-ASCII canonicals fall back uninterned.
    std::unordered_map<std::string, llvm::Value*> sqlCanonicalGlobals;

    llvm::Value* internSqlCanonical(const std::string& canon) {
        bool ascii = true;
        for (unsigned char c : canon) { if (c >= 0x80) { ascii = false; break; } }
        if (!ascii) return emitStringLiteralBytes(canon);  // rare; not interned
        auto it = sqlCanonicalGlobals.find(canon);
        if (it != sqlCanonicalGlobals.end()) return it->second;
        llvm::Value* g = builder->CreateGlobalString(canon, ".sql.canon");
        sqlCanonicalGlobals[canon] = g;
        return g;
    }

    // 64-bit FNV-1a over `s`'s bytes; must match dragon_str_fnv1a in runtime_sqltemplate.cpp
    // so a compile-time folded constant and a runtime-built canonical with the same text share a cache bucket.
    uint64_t sqlCanonicalHash(const std::string& s) const {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) { h ^= (uint64_t)c; h *= 0x100000001b3ULL; }
        return h;
    }

    // D017 Phase 4.B: template block-interp buffer stack. Each `!{...}` falling into block
    // mode allocates a list[str] and pushes its alloca here; `:{}` ExprStmts append to the top buffer, then CodeGen pops it and dragon_str_join_ptr flattens it into the !{}'s value.
    std::vector<llvm::Value*> templateBlockBufferStack;

    // Static member support
    // staticFieldGlobalsBySym[className][fieldName] -> LLVM GlobalVariable for that static field
    std::unordered_map<std::string, std::unordered_map<std::string, llvm::GlobalVariable*>> staticFieldGlobalsBySym;
    // staticMethods tracks "ClassName_methodName" entries that are static (no self param)
    std::unordered_set<std::string> staticMethods;

    // Multi-constructor support: className -> number of __init__ overloads
    std::unordered_map<std::string, size_t> classCtorCountBySym;
    // className -> vector of (arity, constructorIndex) pairs for dispatch
    std::unordered_map<std::string, std::vector<std::pair<size_t, int>>> classCtorAritiesBySym;

    // Module-level globals, keyed by mangleGlobal (symbol "global." + key); a flat
    // bare-name key silently aliased same-named globals across modules. Resolve via resolveGlobalKey.
    std::unordered_map<std::string, llvm::GlobalVariable*> moduleGlobals;
    std::unordered_map<std::string, VarKind> moduleGlobalKinds;

    // importingMod -> (bareName -> mangled global key), the global twin of
    // importedFuncAliasesByModule: `from beta import SHARED` pins beta's global.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedGlobalAliasesByModule;

    // Class binding of class-typed globals keyed by mangled global key; readers
    // consult this before the flat varClassNames so foreign same-named globals can't misdirect dispatch.
    struct GlobalClassBinding {
        std::string className;
        std::string owningModule;
    };
    std::unordered_map<std::string, GlobalClassBinding> moduleGlobalClassNames;
    // Entry-module globals forward-declared (so method bodies can resolve them) but not
    // yet initialized; their first assignment is a definition, not an overwrite, so it must not decref the null initializer. Erased on first init.
    std::unordered_set<std::string> entryGlobalsAwaitingInit;
    llvm::Function* mainFunction = nullptr;  // pointer to main() for detecting module level
    // Scope-stack depth at which the entry module's top-level body executes; a
    // declaration in main() is a module global only at this depth, deeper means block-local.
    size_t moduleBodyScopeDepth = 0;
    bool isDragonFile = false;               // .dr vs .py mode
    std::vector<dragon::Module*> depModulePtrs; // stored dep modules for cross-module type lookups
    dragon::Module* entryModulePtr = nullptr;   // entry module - used by class-field type
                                                // inference to resolve callees defined in
                                                // the same file as the class

    // .py mode: names declared `global` or `nonlocal` in current function
    std::unordered_set<std::string> globalDeclaredVars;
    std::unordered_set<std::string> nonlocalDeclaredVars;
    // nonlocal proxy globals: __nonlocal_<funcname>_<varname> -> GlobalVariable
    std::unordered_map<std::string, llvm::GlobalVariable*> nonlocalProxyGlobals;

    // Extern library hints collected from extern "C" from "lib" { } blocks
    std::set<std::string> externLibs;

    // Deferred static field initializers - collected when ClassDecl is visited
    // before main() exists (e.g., dependency module classes), emitted in main() preamble.
    struct DeferredStaticInit {
        Expr* valueExpr;
        llvm::GlobalVariable* gv;
    };
    std::vector<DeferredStaticInit> deferredStaticInits;

    // Threading: set to true when `fire` is used, triggers -lpthread at link time
    bool needsPthread = false;

    // GC Phase 4: per-function parameter VarKinds (for atomic incref at fire/async spawn)
    std::unordered_map<std::string, std::vector<VarKind>> funcParamKinds;
    // docs/002 2.8: aligned with funcParamKinds (methods include self at 0), true for
    // `own p: T` params. The caller must not drain an owned temp bound to one: ownership transfers, and caller-drain + callee-release double-freed it (A/B-proven, fresh-temp-exemption probe).
    std::unordered_map<std::string, std::vector<bool>> funcParamOwns;
    bool paramIsOwn(const std::string& funcName, unsigned idx) {
        auto it = funcParamOwns.find(funcName);
        return it != funcParamOwns.end() && idx < it->second.size() &&
               it->second[idx];
    }
    // D027: per-function flags marking param i as `Callable[...]`. A bare fn passed there
    // gets wrapped as DragonClosure(fn, null) so the param always holds a real closure (no tag-guess, no crash); freed post-call via argTemps.
    std::unordered_map<std::string, std::vector<bool>> funcCallableParam;

    // `extern "C"` function names (keyed like funcParamKinds). Extern callees don't follow
    // Dragon's borrow-and-incref RC convention (args may be borrowed interior pointers like dragon_bytes_data), so the call site must never release owned-temp args passed to them.
    std::unordered_set<std::string> externFuncNames;
    // FFI v0: extern "C" args are borrowed for the call, and a managed return is a fresh
    // +1, never aliasing an arg; owned temps passed to a managed-typed param drain like any borrow callee (stdlib http leaked one string per header without this). Declared `ptr` return opts out (may alias an arg: leak-over-UAF). Members: externs whose return isn't `ptr`.
    std::unordered_set<std::string> externDrainableFuncs;

    // Default parameter values: funcName -> vector of Expr* (one per LLVM param,
    // nullptr for params without defaults). Used at call sites to fill missing args.
    std::unordered_map<std::string, std::vector<Expr*>> funcParamDefaults;

    // Defining module per LLVM function symbol, recorded alongside funcParamDefaults.
    // fillDefaultArgs swaps currentModuleName to this while evaluating a default's AST so module-private lookups resolve in the defining module, not the call site's (else a default referencing a private helper is unreachable cross-module).
    std::unordered_map<std::string, std::string> funcDefiningModule;

    // D040: declared parameter names (one per LLVM param), populated alongside
    // funcParamDefaults. CallExpr.cpp's non-vararg path binds keyword args to matching positions before fillDefaultArgs fills the rest.
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;

    // Union type support: member VarKinds per union-typed variable, used by isinstance
    // narrowing (computes the "else" type for 2-member unions and validates against declared members).
    std::unordered_map<std::string, std::vector<VarKind>> unionMemberKinds;
    // D030 Phase 4: unionTagAllocas/funcUnionTagMask deleted; tag is now structural in
    // the {i64, i64} box value.

    // First-class function support: LLVM FunctionType of callable variables, populated
    // when a lambda or named function reference is assigned (`fn = double`). visit(CallExpr) uses it for indirect calls when module->getFunction(name) fails.
    std::unordered_map<std::string, llvm::FunctionType*> callableTypes;

    // Nested `def` aliases: a nested def's LLVM function is mangled (`__dragon_nested_3__inner`)
    // to avoid sibling collisions, but source calls it by the bare name. While emitting the nested def's own body, this map resolves `inner(...)` to a direct LLVM call (env auto-appended for capturing variants) for self-recursion.
    struct NestedAliasInfo {
        llvm::Function* fn;            // mangled LLVM function (params + optional trailing env)
        llvm::FunctionType* userFnType;// user-visible signature (no trailing env)
        llvm::Value* envValue;         // null for non-capturing; else the body's __env arg
    };
    std::unordered_map<std::string, NestedAliasInfo> nestedFunctionAliases;

    // D027: non-null after LambdaExpr codegen means the last value was a closure; holds
    // the user-facing function type (no trailing env). Assignment path checks it to set VarKind::Closure and callableTypes.
    llvm::FunctionType* lastClosureCallableType = nullptr;

    // D025 Phase 4: set to true when type() returns a class descriptor (i64).
    // Assignment path checks this to set VarKind::Type.
    bool lastValueIsType = false;

    // D024: functions wrapped by user-defined decorators, mapping the original name to a
    // module global holding the decorated callable. Call dispatch checks this before direct calls.
    std::unordered_map<std::string, llvm::GlobalVariable*> decoratedFunctions;

    // Pre-registers a decorated top-level function's indirect-dispatch global before class
    // method bodies emit (which happens before visit(FunctionDecl) applies decorators), so a method calling it doesn't bind the undecorated original. Idempotent; visit(FunctionDecl) reuses the global.
    void preregisterDecoratedFunction(FunctionDecl& node);

    // *args/**kwargs tracking: function name -> vararg info
    struct VarArgInfo {
        size_t numRegularParams = 0; // params before *args
        bool hasVarArg = false;
        bool hasKwArg = false;
        std::string varArgName;      // name of *args param
        std::string kwArgName;       // name of **kwargs param
        // Element representation for *args, from the declared `*args: T` annotation, so the
        // call site packs into the monomorphized list variant: tag2->ListF64, tag1/5/6/7->ListPtr, isAny->ListBox, else legacy DragonList.
        int64_t varArgElemTag = 0;
        bool    varArgElemIsAny = false;
    };
    std::unordered_map<std::string, VarArgInfo> funcVarArgInfo;

    // Propagated class name from last dynamic descriptor call
    // Used by assignment visitors to set varClassNames for the destination
    std::string lastDynConstructClassName;

    // SQLite3: set to true when sqlite3_* functions are encountered
    bool needsSqlite3 = false;

    // PCRE2: set to true when pcre2_* functions are encountered
    bool needsPcre2 = false;

    // mbedTLS: set when the dragon_tls_* shim or mbedTLS-backed crypto digests/HMAC
    // (dragon_sha*/dragon_md5*/dragon_hmac, ADR 038 Phase 7) are referenced; both pull mbedtls_* symbols from libdragon_mbedtls.a.
    bool needsMbedtls = false;

    // System libz/libzstd: set when dragon_zlib_*/dragon_zstd_* externs show up, so
    // linkExecutable knows whether to pass -lz/-lzstd. Programs that skip compression don't pay.
    bool needsZ = false;
    bool needsZstd = false;

    // Webview shell (D031 `import ui`): set when dragon_webview_* externs show up, so
    // linkExecutable compiles the platform shell and links webkit2gtk via pkg-config. Non-UI programs never carry GTK/webkit.
    bool needsWebview = false;

    // Dunder method tracking: className -> set of dunder names (e.g. "__str__", "__eq__")
    std::unordered_map<std::string, std::set<std::string>> classDunderMethodsBySym;

    // Resolve the class name of an expression (for dunder dispatch and field
    // access). Returns "" if the expression is not a known class instance.
    std::string resolveExprClassName(Expr* expr);

    /// True when a module-level annotated declaration binds a deque: the annotation names
    /// deque (`X: deque[T]`) or the RHS is a deque(...) ctor call.
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

    /// True for a receiver denoting the intrinsic Lock: a tagged local/global
    /// (`lock.acquire()`) via varClassNames, or a Lock-typed field (`self._lock.acquire()`) via classFieldClassNameBySym. The old NameExpr-only check silently dropped field locks (found via concurrent-mutation detector on Router._storage_lock). Defined in ImplMethods.cpp.
    bool isLockExpr(Expr* e);

    // Resolves the VarKind of an arbitrary expression, used by print() and other dispatch
    // sites needing a non-NameExpr argument's kind (e.g. `obj.names[0]` would otherwise fall through to default-int print). Returns VarKind::Other if unknown.
    VarKind resolveExprVarKind(Expr* expr);

    // Check if a class has a specific dunder method (walks inheritance chain).
    // Accepts a bare name or a sym; the chain itself is sym -> parent sym.
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

    // Find the class that defines the dunder (for MRO). Returns its SYM.
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

    // Call a dunder method on a class instance. Returns the result or nullptr if not found.
    llvm::Value* callDunder(const std::string& className, const std::string& dunder,
                            llvm::Value* self, const std::vector<llvm::Value*>& extraArgs = {});

    // Emit the call to an ALREADY-resolved dunder (None-fills missing params).
    llvm::Value* emitDunderCall(llvm::Function* func, const std::string& dunder,
                                llvm::Value* self,
                                const std::vector<llvm::Value*>& extraArgs = {});

    // Converts a value to i1 for use in conditions; for class instances calls __bool__ if
    // available (defaults to true). exprNode is optional, used to resolve the class name for dunder dispatch.
    llvm::Value* toBool(llvm::Value* val, Expr* exprNode = nullptr);

    void init(); // defined in codegen/ImplInit.cpp

    void pushScope() { scopes.push_back({}); }
    void popScope() { if (!scopes.empty()) scopes.pop_back(); }

    /// True if a VarKind is heap-allocated with a DragonObjectHeader, safely decref'able.
    /// Str (dynamic) is included (dragon_decref_str navigates from data ptr to header); StrLiteral is not (no header).
    static bool isHeapKind(VarKind k) {
        return k == VarKind::Str || k == VarKind::List || k == VarKind::Dict ||
               k == VarKind::Tuple || k == VarKind::Set ||
               k == VarKind::File || k == VarKind::ClassInstance || k == VarKind::Generator ||
               k == VarKind::Deque ||
               k == VarKind::Closure ||  // D027: closure wrapper is refcounted
               k == VarKind::Union;  // conservative: union may hold heap types
    }

    // True when a closure env capture can be a cycle node: a heap object the env holds
    // a +1 to that can transitively point back (instance/list -> closure -> env -> ...). Strings are leaves and unions are boxed, so neither counts. Drives both the env's gc-track gate and which captures gc_fn's TRAVERSE visits.
    static bool envCaptureIsCyclic(VarKind kind, bool isCellRelay) {
        if (isCellRelay) return true;
        return kind == VarKind::List || kind == VarKind::Dict ||
               kind == VarKind::Tuple || kind == VarKind::Set ||
               kind == VarKind::ClassInstance || kind == VarKind::Generator ||
               kind == VarKind::Deque || kind == VarKind::Closure;
    }

    // Per-capture descriptor for the shared env-GC-hook emitter, one per env field (field
    // i+1 of the env struct); `kind`/`isCellRelay` mirror the closure-site CaptureInfo.
    struct EnvCaptureDesc { VarKind kind; bool isCellRelay; };

    // Emits the per-closure-site multi-op env GC hook (DEALLOC/TRAVERSE/CLEAR over the
    // env's heap captures), replacing the old single-purpose dealloc fn. Both closure sites (LambdaExpr + nested def) route through this one emitter so they can't drift.
    llvm::Function* emitEnvGcFn(const std::string& baseName,
                                llvm::StructType* envStructType,
                                const std::vector<EnvCaptureDesc>& caps);

    /// True if `v` is an owned (+1, heap) intermediate string a consuming op must decref
    /// or it leaks. All dragon_* i8*-returning fns return owned strings except the borrowed-returner blocklist here; non-CallInst values (literals, var loads) are never owned.
    bool isOwnedStrResult(llvm::Value* v);

    /// True when a named callee returns a borrowed string (TLS slots, container element
    /// reads, foreign C pointers) that must never be decref'd. Shared by isOwnedStrResult and the mixed-shape comparison drain in Expressions.cpp.
    bool isBorrowedStrReturnerName(const std::string& name);

    /// Generic-pointer analog of isOwnedStrResult: true if `v` is an owned (+1, fresh)
    /// heap pointer (list/dict/set/tuple/instance/bytes) a consumer must release. Same blocklist shape; container element reads are borrows, and non-CallInst values are conservatively borrows.
    bool isOwnedPtrResult(llvm::Value* v);

    /// True if `v` is an owned box temporary (`{tag,payload}`, refcounted payload
    /// carrying a +1). Box equivalent of isOwnedStrResult, same blocklist shape (container element-reads are borrows); ReturnStmt increfs a borrowed payload before returning, so a call's box result always owns it. Non-CallInst/indirect-call values are conservatively borrows.
    bool isOwnedBoxResult(llvm::Value* v);

    /// D030 §5: Type::Kind -> DragonValueTag, the source-of-truth tag derivation,
    /// replacing varKindToTag wherever the source expression's static type is known. Critical for Bytes-typed values in box/union/typedDict slots, whose VarKind collapses to a generic heap kind.
    static int64_t typeKindToTag(Type::Kind k);

    /// Map VarKind to DragonValueTag. Returns -1 if no specific tag (e.g. Other/Any).
    static int64_t varKindToTag(VarKind vk);

    /// When >= 0, the next dict subscript/dot-access emits dragon_dict_get_checked
    /// with this tag, then resets to -1. Set by AnnAssignStmt when RHS is dict access.
    int64_t pendingDictCheckTag = -1;

    /// Companion to pendingDictCheckTag for list-annotated LHS: the dict checked-get
    /// verifies the stored tag is "list" but can't distinguish a monomorphized DragonList from a DragonListBox, so the get site view-checks the returned pointer (`xs: list[int] = anyDict["k"]` raises TypeError instead of misreading a list[str]'s pointers as ints).
    int64_t pendingListViewElemTag = kNoListElemCheck;

    /// D030 Phase 3.G: resolves the static key Type::Kind of a dict `expr` evaluates to,
    /// used by subscript/`in`/print to branch str-keyed vs int-keyed. Returns Unknown when no annotation reached this site.
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

    /// True iff `expr` denotes a dict[int, V].
    /// Resolves the VALUE kind of a dict expression (the V in dict[K, V]), mirroring
    /// resolveDictKeyKind but reading the value-kind maps. Used to pick the typed path for `d[k] OP= v`.
    Type::Kind resolveDictValueKind(Expr* expr);

    /// Phase 5: maps a Type::Kind to a DragonValueTag for container elem_tag (0/TAG_INT
    /// for non-heap types). 6.12: TAG_BOOL=3 unlocks 1-byte packed storage, so list[bool] of 1M elements drops from 8MB to 1MB and fits in L2.
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

    /// If `e` is a container (list/dict/set/tuple), returns the runtime function that
    /// renders it to a DragonString, else "". Used by str()/f-strings so a container renders as its repr instead of an empty misread string pointer. Sets type as ListType, so VarKind/AST disambiguates list vs set first.
    std::string containerReprFn(Expr* e);

    /// Records the class (and owning module) constructed by an assignment's
    /// value expression for `varName`; returns the class name or "" if the
    /// value is not a class instance or the var is already classified. The
    /// single definition replaces four drifting copies (one of which skipped
    /// varClassOwningModule, breeding cross-module collisions).
    std::string recordVarClassFromValue(const std::string& varName, Expr* value);

    /// Phase 5: Get elem_tag for a list expression from its resolved type.
    int64_t getListElemTag(Expr* listExpr) {
        if (listExpr && listExpr->type) {
            if (auto* lt = dynamic_cast<ListType*>(listExpr->type.get())) {
                if (lt->elementType) return typeKindToElemTag(lt->elementType->kind());
            }
        }
        return TAG_INT; // TAG_INT (unknown)
    }

    /// Maps a Type::Kind to the matching VarKind, used when binding a comprehension/for-loop
    /// variable to the iterable's element type. Returns Int for primitives not tracked as heap kinds.
    static VarKind typeKindToVarKind(Type::Kind k);

    /// D030 §5: single source of truth for native-LLVM-type derivation from Type::Kind
    /// (what shape a value has at the ABI level). Replaces ad-hoc translation switches; drives loop-var allocas, list-get return shapes, dict-value bindings, etc.
    llvm::Type* typeKindToLLVM(Type::Kind k) const;

    /// Type::Kind-based heap classification, replacing `isHeapKind(VarKind)` at
    /// refcount-on-iteration sites so the test is driven from the static type. Mirrors isHeapKind minus the Other/Union/File branches VarKind tracked for non-Type-shaped slots.
    static bool isHeapTypeKind(Type::Kind k);

    /// Determines the element Type::Kind of an iterable expression: checks `varListElemKinds`
    /// for plain NameExpr iterables (matches ForLoop.cpp), falls back to the resolved AST type, else Int.
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

    /// True when iterating `expr` directly means iterating a dict's keys, i.e. `expr` is
    /// a bare dict (not a `.keys()`/`.items()`/`.values()` call). For-loops/comprehensions must convert via dragon_dict_keys first; indexing the dict pointer directly walks raw bytes (SIGSEGV).
    bool isBareDictIterable(Expr* expr);

    /// Determine the correct DragonValueTag for a pointer-typed expression.
    /// Used by DictExpr to tag values properly (not blindly TAG_STR for all pointers).
    int64_t inferPtrValueTag(Expr* expr);

    /// Promotes a string literal to a heap DragonString via dragon_string_dup: true for a
    /// compile-time StringLiteral or StrLiteral-kind NameExpr, else returns val as-is. An f-string is also a StringLiteral node but its value is already an owned +1; dup'ing it here orphaned the original +1, leaking one string per list/dict store or subscript-assign of an f-string.
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

    // DragonCleanupKind mirror (must match runtime_internal.h). Selects which
    // decref the unwind path calls for a registered owned heap local.
    static constexpr int DCLEAN_STR      = 1;
    static constexpr int DCLEAN_CALLABLE = 2;
    static constexpr int DCLEAN_OBJ      = 3;
    static constexpr int DCLEAN_UNION    = 4;
    // A pending defer's call entry: val is the void(i64*) thunk, tag is the arg count. The
    // unwinder invokes it over the `tag` entries pushed directly below (each with its own DCLEAN kind for post-call release), then keeps popping so they drain normally.
    static constexpr int DCLEAN_DEFER_CALL = 5;

    /// Maps an owned-heap VarKind to its DragonCleanupKind, mirroring emitScopeCleanupFor's
    /// per-kind decref dispatch. Returns 0 for non-heap (caller must not push); Union is handled separately (carries a box tag).
    int cleanupKindFor(VarKind k) {
        switch (k) {
            case VarKind::Str:     return DCLEAN_STR;
            case VarKind::Closure: return DCLEAN_CALLABLE;
            case VarKind::Union:   return DCLEAN_UNION;
            default:               return isHeapKind(k) ? DCLEAN_OBJ : 0;
        }
    }

    /// Coerce an owned heap value (ptr) or union payload (i64) to the i64 the
    /// cleanup stack snapshots.
    llvm::Value* cleanupValToI64(llvm::Value* v) {
        if (v->getType() == i64Type) return v;
        if (v->getType()->isPointerTy())
            return builder->CreatePtrToInt(v, i64Type, "clean.v");
        return builder->CreateZExtOrBitCast(v, i64Type);
    }

    /// The thread-local frame-count global (`__dragon_active_frames`), declared lazily
    /// with initial-exec TLS (call-free GOT-relative access, valid since the runtime is statically linked). The only inline TLS read codegen emits.
    llvm::GlobalVariable* activeFramesGlobal = nullptr;
    llvm::GlobalVariable* getActiveFramesGlobal() {
        if (!activeFramesGlobal) {
            activeFramesGlobal = new llvm::GlobalVariable(
                *module, llvm::Type::getInt32Ty(*context), /*isConstant=*/false,
                llvm::GlobalValue::ExternalLinkage, /*init=*/nullptr,
                "__dragon_active_frames", /*insertBefore=*/nullptr,
                llvm::GlobalValue::InitialExecTLSModel);
        }
        return activeFramesGlobal;
    }

    /// Emits the inline cleanup gate `__dragon_active_frames != 0`: a heap local declared
    /// with no live exception frame can never be longjmp-unwound (uncaught raise exits), so registration is skipped; the hot path pays only a predicted-untaken branch.
    llvm::Value* emitActiveFramesNonZero() {
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* af = builder->CreateLoad(i32Ty, getActiveFramesGlobal(), "active.frames");
        return builder->CreateICmpNE(af, llvm::ConstantInt::get(i32Ty, 0), "frame.live");
    }

    /// Create an i32 alloca in the entry block, initialized once at function
    /// entry. Used for the cleanup slot/base sentinels (-1 = "not pushed").
    llvm::AllocaInst* createEntryAllocaI32(llvm::Function* func,
                                           const std::string& name, int initVal) {
        llvm::IRBuilder<> tmp(&func->getEntryBlock(), func->getEntryBlock().begin());
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* a = tmp.CreateAlloca(i32Ty, nullptr, name);
        tmp.CreateStore(llvm::ConstantInt::get(i32Ty, initVal), a);
        return a;
    }

    /// Finds the i32 alloca holding a cleanup-registered local's runtime slot index,
    /// searching the scope chain (mirrors setVar's owning-scope resolution). Null if never registered.
    llvm::AllocaInst* findCleanupSlot(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->cleanupSlots.find(name);
            if (found != it->cleanupSlots.end()) return found->second;
            if (it->vars.count(name)) return nullptr;  // owning scope, not registered
        }
        return nullptr;
    }

    /// Registers a freshly-declared owned heap local on the unwind cleanup stack and
    /// remembers its slot for reassignment refresh. `tagVal` is the box value-tag for Union locals; no-op outside RC mode or a terminated block.
    void emitCleanupPush(const std::string& name, llvm::Value* value,
                         int cleanupKind, llvm::Value* tagVal = nullptr);

    /// Refreshes a registered local's cleanup snapshot after reassignment (old value
    /// already decref'd by storeWithRCOverwrite/the union path). No-op if never registered.
    void emitCleanupUpdate(const std::string& name, llvm::Value* value,
                           llvm::Value* tagVal = nullptr);

    /// Pushes an anonymous for-loop temp (generator/iterator) onto the unwind cleanup
    /// stack so a raise unwinding the loop's frame frees it (these aren't named scope locals, so emitScopeCleanupFor never sees them and they'd leak). Pair with emitCleanupPopTemp.
    llvm::Value* emitCleanupPushTemp(llvm::Value* ptr, int cleanupKind);

    /// Registers owned arg temps on the runtime cleanup stack for the call's duration, so
    /// a longjmp out of the callee frees them (else e.g. the bytes literal in `assertRaises(..., lambda: f(b"x"))` leaks when the callee raises). Only tag-independent kinds (Str/Callable/Obj); Union needs a box tag this path doesn't carry.
    std::vector<llvm::Value*> pushArgTempCleanups(
        const std::vector<std::pair<llvm::Value*, VarKind>>& argTemps);

    /// Rewind (does NOT free) the cleanup entries pushed by pushArgTempCleanups,
    /// in reverse push order, on the normal-return path before the decref.
    void popArgTempCleanups(const std::vector<llvm::Value*>& bases);

    /// Rewinds the cleanup stack past a temp pushed by emitCleanupPushTemp; call at the
    /// loop's normal-exit decref site so a later unwind doesn't double-free the stale snapshot.
    void emitCleanupPopTemp(llvm::Value* baseAlloca);

    /// Emits dragon_decref calls for all heap-typed, non-borrowed locals in the current
    /// scope, before the terminator and before popScope(). Str uses dragon_decref_str; ClassInstance uses dragon_decref (pointer is the header, like containers).
    void emitScopeCleanupFor(Scope& scope);

    /// Clean up the innermost scope only (used at normal scope exit points
    /// like end-of-loop-body, end-of-handler, etc.).
    void emitScopeCleanup() {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        emitScopeCleanupFor(scopes.back());
    }

    /// Cleans up all scopes innermost to outermost; used by return statements exiting the
    /// whole function.
    void emitAllScopeCleanup() {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            emitScopeCleanupFor(*it);
        }
    }

    /// Cleans up scopes from innermost down to (and including) targetDepth; used by
    /// break/continue to clean loop-interior scopes without touching the enclosing function scope.
    void emitScopeCleanupToDepth(size_t targetDepth) {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (size_t i = scopes.size(); i > targetDepth; --i) {
            emitScopeCleanupFor(scopes[i - 1]);
        }
    }

    /// Replay one exit-cleanup entry (a finally body or a with's __exit__ /
    /// lock-release set), used by the depth-interleaved early-exit walk below.
    void replayExitCleanup(CodeGen& cg, ExitCleanup& e) {
        if (!e.isWith) {
            for (auto* stmt : e.finallyBody) stmt->accept(cg);
        } else {
            // Forward order, matching the with-statement's normal cleanup path.
            for (auto& it : e.withItems) {
                if (it.isClassCtx) {
                    if (it.exitFn) emitDunderCall(it.exitFn, "__exit__", it.val);
                    else callDunder(it.className, "__exit__", it.val);
                    if (options.gcMode == GCMode::RC) {   // release the CM object (#8)
                        if (it.subjectOwned)
                            builder->CreateCall(runtimeFuncs["dragon_decref"], {it.val});
                        if (it.enterResult && it.enterResult->getType()->isPointerTy())
                            builder->CreateCall(runtimeFuncs["dragon_decref"], {it.enterResult});
                    }
                } else if (it.isLock) {
                    builder->CreateCall(runtimeFuncs["dragon_lock_release"], {it.val});
                    if (it.isLockTemp)  // anonymous `with Lock()` - free the mutex
                        builder->CreateCall(runtimeFuncs["dragon_lock_destroy"], {it.val});
                }
            }
        }
    }

    /// Early-exit (return/break/continue) cleanup: walks scopes and exit-cleanup entries
    /// down to targetScopeDepth/ecDownTo, interleaved by nesting depth so each scope's defers+decrefs run before the finally/__exit__ enclosing it, matching normal fall-through. Replaces an old two-flat-pass order that ran an enclosing finally before an inner scope's defers.
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

    /// Check if a function body contains any YieldExpr nodes (making it a generator).
    static bool containsYield(const std::vector<std::unique_ptr<Stmt>>& body) {
        struct YieldFinder : public DefaultASTVisitor {
            bool found = false;
            void visit(YieldExpr&) override { found = true; }
            // Don't recurse into nested function definitions
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

    /// True if the body has a value-returning `return <expr>` (not bare). An unannotated
    /// function with none is a procedure needing `void` return (a bare `return` lowers to `ret void`, mismatching the i64 default and failing LLVM verification). Stops at nested function/class bodies.
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

    /// LLVM return type for an unannotated function/method: void for a procedure, else the
    /// historical int default. Centralizes the rule so forward-declaration and body emission agree (a divergence fails LLVM verification).
    llvm::Type* unannotatedReturnType(const std::vector<std::unique_ptr<Stmt>>& body) {
        return bodyReturnsValue(body) ? i64Type : voidType;
    }

    /// Infers the yielded value's VarKind from a generator body's first YieldExpr, used
    /// to bind `for x in gen()` with the right kind so heap-typed yields round-trip instead of printing as raw i64. Returns Int if none found or unresolved.
    VarKind inferYieldKind(const std::vector<std::unique_ptr<Stmt>>& body);

    /// generator function name -> VarKind of values it yields; populated when compiled,
    /// consulted by the for-in-over-generator path to type the loop variable.
    std::unordered_map<std::string, VarKind> generatorYieldKinds;

    /// variable (storing a Generator) -> VarKind of yielded values, populated on
    /// `g = some_gen_fn(...)` so later `for x in g { ... }` loops know how to type x.
    std::unordered_map<std::string, VarKind> varGenYieldKinds;

    // C9-B shared spread expansion: expands `node`'s positional (`*tuple`/`*list`) and
    // kwargs (`**dict`) spreads into the fully-coerced `args` vector against `func`'s signature, registering owned temps in `argTemps` (spread elements are borrowed, never listed). Doesn't fill defaults or emit the call. Defined in CallExpr.cpp.
    bool expandSpreadCallArgs(
        CodeGen& cg, llvm::Function* func, CallExpr& node,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName);

    // Packs a variadic method call's surplus positionals into `*args` and surplus keywords
    // into `**kwargs`, given `self` already at args[0]. Method-path twin of emitVarArgCall (differs only by the leading-self offset). Packed list/dict are owned temps registered in `argTemps` for the shared call tail to drain. Defined in CallMethods.cpp.
    bool packVarArgMethodArgs(
        CodeGen& cg, CallExpr& node, const std::string& methodFuncName,
        llvm::FunctionType* methodFuncType,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName);

    // Binds the regular-param slots in `bindIdx` by name from a `**dict` spread source:
    // a required param raises TypeError if absent, an optional PHIs between the dict value and its default. Bound heap values are borrowed. Shared by expandSpreadCallArgs and emitVarArgCall.
    void bindParamSlotsFromDict(
        CodeGen& cg, llvm::Function* func, llvm::Value* d,
        std::vector<llvm::Value*>& args, const std::vector<size_t>& bindIdx,
        const std::vector<std::string>& paramNames, const std::string& dispName);

    /// Evaluates default Expr* nodes for params not already supplied. D040: scans [0,
    /// numParams) and fills any slot past args.size() or nullptr (a kwargs-binding hole). `defaultTemps` (optional) collects owned heap temps for omitted-arg defaults (each call mints a fresh +1) so the caller can release them after the call; skip (nullptr) if not draining.
    void fillDefaultArgs(const std::string& funcName, llvm::Function* func,
                         std::vector<llvm::Value*>& args, CodeGen& cg,
                         std::vector<std::pair<llvm::Value*, VarKind>>* defaultTemps = nullptr);

    /// Emits an atomic incref for a pointer crossing a thread boundary (fire fn(args),
    /// async def wrapper), dispatched by VarKind. Also emits dragon_mark_shared_deep/_str first (d018-shared-refcount.md) to propagate SHARED to every reachable child; without it, two vthread bodies tore the refcount on shared Router state and the cycle collector walked freed memory (the original hello_server crash at request ~87).
    void emitAtomicIncref(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        // Ensure ptr type for the call
        if (!val->getType()->isPointerTy()) return;
        if (kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_str"], {val});
            builder->CreateCall(runtimeFuncs["dragon_incref_str_atomic"], {val});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_deep"], {val});
            builder->CreateCall(runtimeFuncs["dragon_incref_atomic"], {val});
        }
    }

    /// Storing a heap value into a shared instance field makes it globally reachable, so it
    /// must be shared-marked like list/dict store barriers already do for elements (previously masked only by the fire path re-marking `self` per connection). An unshared instance pays one byte load + a predicted-untaken branch (SHARED bit at header offset 9).
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
            // tag gated: a Callable field val may be a bare fn ptr.
            auto* asI64 = builder->CreatePtrToInt(val, i64Type, "fieldshr.clos.i64");
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_boxed"],
                {llvm::ConstantInt::get(i64Type, 10), asI64});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_deep"], {val});
        }
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
    }

    /// A value stored into a module global is reachable from every vthread by name,
    /// never crossing a `fire` boundary, so emitAtomicIncref's fire-site mark misses it; two vthreads then tore the refcount on the same object (the copy-a-global-to-a-local server crash). Mark the stored graph SHARED here instead. Cold path: globals store rarely.
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

    // Conservative borrowed-reference detector for assignment/consume-site RHS: name,
    // attribute, and subscript reads are existing references owned by their enclosing slot/container (need an incref on store); fresh-ref expressions (literals, calls, constructors) already own a +1 and aren't borrowed.
    static bool isBorrowedHeapExpr(Expr* expr) {
        // ADR 054 - a conformance cast compiles to nothing: ownership-wise it
        // IS its operand (same pointer), so classification must see through.
        if (auto* cast = dynamic_cast<AsCastExpr*>(expr))
            return isBorrowedHeapExpr(cast->operand.get());
        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            // A slice (s[1:4]) calls dragon_str_slice/_list_slice/_bytes_slice, all
            // returning a fresh +1 the consumer owns, never a borrow; misclassifying it added an extra incref and skipped the arg-temp decref, leaking one object per evaluation.
            if (dynamic_cast<SliceExpr*>(sub->index.get()) != nullptr)
                return false;
            // A string element read (s[i]) is owned too: strings are immutable, so
            // dragon_str_index mallocs a fresh 1-char string with no interior ref to hand back; misclassifying it leaked one string per evaluation (ord(s[i]), len(s[i]), binascii.hexlify's hot loop). list/dict/set element reads (xs[i], d[k]) do borrow and stay borrowed.
            if (sub->object && sub->object->type &&
                sub->object->type->kind() == Type::Kind::Str)
                return false;
            // A dict element read off a call receiver (`r.info()["k"]`): the receiver is
            // an owned temp the read consumes, and retainElemThenReleaseRecv (Attributes.cpp) retains the element before releasing it, handing an owned +1 (mirrors the f().attr rule below). Only concrete-heap element branches retain; Any/Union/closure elements keep the borrow story.
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
            // `own x` (docs/002 2.8) transfers the binding's +1, so it's owned, never a
            // borrow (consumer adopts, move-out nulls the source slot). `dub x` (2.7) likewise hands a fresh owned +1 (deep copy/identity retain), never a borrow.
            return !nm->isMoveMarked && !nm->isDubMarked;
        }
        if (auto* at = dynamic_cast<AttributeExpr*>(expr)) {
            // `f().attr`: the receiver is an owned temp the read consumes; Attributes.cpp
            // retains the field by kind and releases the receiver, handing an owned +1, so attr-on-a-call is not a borrow. A field read off a named object stays borrowed.
            return !dynamic_cast<CallExpr*>(at->object.get());
        }
        return false;
    }

    /// docs/002 moves: after a call consumes `f(own x)` args, nulls each moved-out slot
    /// so the caller's scope-exit release sees nothing (decref of null no-ops; Lock destroy is null-gated). E9-at-join guarantees every path agrees; pure bookkeeping.
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

    /// Move-out for one value expression (an own-field store RHS): if it is
    /// `own x`, null x's slot - the field adopted the +1.
    void emitMoveOutIfMarked(Expr* value) {
        if (options.gcMode != GCMode::RC || !value) return;
        auto* nm = dynamic_cast<NameExpr*>(value);
        if (!nm || !nm->isMoveMarked) return;
        if (auto* alloca = lookupVar(nm->name)) {
            emitNullSlot(alloca);
            // Same unwind-snapshot neutralization as emitMoveOutSlots: the
            // adopter owns the +1 now, the cleanup stack must not re-free it.
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
                // The unwind cleanup stack snapshots the value at declaration; the callee
                // adopted that +1, so a later longjmp unwind must see null, not re-free what the callee now owns.
                emitCleanupUpdate(
                    nm->name,
                    llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(i8PtrType)),
                    nullptr);
            }
        }
    }

    // setdefault key ownership (#20a): a new str-keyed insert adopts the key pointer,
    // so incref a borrowed heap str key here (literals/owned temps need nothing); no-op for int keys/non-RC. The runtime releases the key on its present branch, so an already-existing key never leaks. Shared by heap-valued/scalar/syncdict setdefault sites.
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

    // Convert any pointer-typed value to i8* for runtime RC calls.
    llvm::Value* toI8Ptr(llvm::Value* val) {
        if (!val || !val->getType()->isPointerTy()) return nullptr;
        if (val->getType() == i8PtrType) return val;
        return builder->CreateBitCast(val, i8PtrType);
    }

    // Emits non-atomic incref/decref for a value based on VarKind: Str uses string-specific
    // RC entrypoints, all other heap kinds use generic object RC entrypoints.
    void emitIncrefByKind(llvm::Value* val, VarKind kind);

    void emitDecrefByKind(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (kind == VarKind::Union) {
            // A Union is a {tag, payload} box value, not a pointer: extract and release by
            // runtime tag (no-op for scalar tags and the zero {0,0} box). Mirrors emitIncrefByKind.
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
            // tag-gated drop - frees a real closure (cascading to its env)
            // and no-ops on a bare fn ptr / null. See emitIncrefByKind.
            builder->CreateCall(runtimeFuncs["dragon_decref_callable"], {ptr});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_decref"], {ptr});
        }
    }

    /// Owned heap-temp call args carry a +1 the callee borrows but never consumes (increfs
    /// on retain), so the caller must release it after the call or leak (binary-trees/object-tree leak). Returns the decref kind, or Other to skip: borrowed exprs never decref; Str only when an owned result; Union/box managed by tag.
    VarKind argTempDecrefKind(Expr* argExpr, VarKind paramKind, llvm::Value* rawVal) {
        if (options.gcMode != GCMode::RC) return VarKind::Other;
        if (paramKind == VarKind::Union) {
            // Union/Any params share the callee-borrows contract: the callee increfs on
            // retain (via storeWithRCOverwrite), so the caller's owned temp is always the caller's to drain. If an Any field store ever adopted the temp's +1 without increfing, this drain would double-free it (pinned by test_rc_any_field.dr). Monomorphizing spurious-Any into generics [T] is the better fix where the type is knowable; this covers the dynamic remainder.
            // A provably-owned box result (dragon_box_subscript/box_binop/Any-returning call)
            // must drain even when the source reads as borrowed (isBorrowedHeapExpr classifies every subscript borrowed), since isOwnedBoxResult is value-based and precise; borrowed-box returners (dict_get_box etc.) stay undrained. Checked before isBorrowedHeapExpr or leaked one payload per `f(anyVal[k])`.
            if (rawVal && rawVal->getType() == boxType)
                return isOwnedBoxResult(rawVal) ? VarKind::Union : VarKind::Other;
            if (isBorrowedHeapExpr(argExpr)) return VarKind::Other;
            // Owned native heap temp boxed at the boundary (concat/ctor/slice into `x: Any`):
            // the box borrows the payload, so the native +1 drains by the temp's own static type.
            return ownedTempDrainKind(argExpr, rawVal);
        }
        if (!isHeapKind(paramKind))
            return VarKind::Other;
        // A box arg unboxed into a native heap param (coerceArg unboxes it): an owned box
        // temp releases after the call as a Union (same drain as the Any-param branch); a borrowed box (dict_get_box etc.) belongs to its container, never drained. Checked before isBorrowedHeapExpr, which reads false for a ternary source and would otherwise mis-drain the box as the param's kind.
        if (rawVal && rawVal->getType() == boxType)
            return isOwnedBoxResult(rawVal) ? VarKind::Union : VarKind::Other;
        if (isBorrowedHeapExpr(argExpr)) return VarKind::Other;
        if (paramKind == VarKind::Str && !isOwnedStrResult(rawVal))
            return VarKind::Other;
        return paramKind;
    }

    // Classifies one pre-coerce call argument and, if an owned heap temp the callee borrows,
    // records it in `out` for post-call release. Skips ptr-returning extern-C callees (FFI interior-pointer hazard) and anything argTempDecrefKind rejects. The single place direct-call sites route owned-temp tracking through (#3, class A).
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
        // An own param ADOPTS the +1 (fresh-temp exemption): no caller drain.
        if (paramIsOwn(funcName, paramIdx)) return;
        VarKind dk = argTempDecrefKind(srcExpr, it->second[paramIdx], rawArg);
        if (dk != VarKind::Other) out.emplace_back(rawArg, dk);
    }

    // Classifies an owned heap temp passed to a borrow callee (str.split, dict.get, int(s),
    // len(x): reads transiently, produces a fresh result). Needs no param-kind table since a borrow callee never consumes the +1; returns Other for borrowed exprs/literals/scalars. Not for transfer callees (list.append, dict set adopt the +1) - those skip this.
    VarKind ownedTempDrainKind(Expr* e, llvm::Value* v) {
        if (options.gcMode != GCMode::RC) return VarKind::Other;
        if (!v || !e || !e->type || isBorrowedHeapExpr(e)) return VarKind::Other;
        // Gate on the argument's static type, not just the LLVM value: at LLVM level
        // str/list/dict/set/bytes are all i8*, so isOwnedStrResult would misclassify a set/list temp as Str and free it via dragon_decref_str (a header walk past the container struct: heap overflow).
        switch (e->type->kind()) {
            case Type::Kind::Str:
                return isOwnedStrResult(v) ? VarKind::Str : VarKind::Other;
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
            case Type::Kind::Contract:  // ADR 054 - an instance behind a contract
                // All release via dragon_decref (VarKind::List is a representative
                // non-Str / non-Closure heap kind in emitDecrefByKind).
                return (v->getType()->isPointerTy() && isOwnedPtrResult(v))
                           ? VarKind::List : VarKind::Other;
            case Type::Kind::Unknown:
                // An untyped owned call result would skip its release silently;
                // never legitimate once the signature pre-passes have run.
                if (v->getType()->isPointerTy() && dynamic_cast<CallExpr*>(e) &&
                    isOwnedPtrResult(v))
                    addError("internal error: owned call result has no static "
                             "type; its release would be silently skipped "
                             "(typechecker signature gap)", e->location());
                return VarKind::Other;
            default:
                return VarKind::Other;  // int/float/bool/Any/Function: no owned heap +1
        }
    }

    // Wraps an already-evaluated borrow-callee argument: if an owned heap temp, records it
    // in `sink` for post-call release and returns the value unchanged (`trackBorrowTemp(expr, lastValue, sink)` replacing a bare `lastValue`). `sink` is block-local, nesting-safe.
    llvm::Value* trackBorrowTemp(Expr* e, llvm::Value* v,
                                 std::vector<std::pair<llvm::Value*, VarKind>>& sink) {
        VarKind k = ownedTempDrainKind(e, v);
        if (k != VarKind::Other) sink.emplace_back(v, k);
        return v;
    }

    // Post-call release of every owned temp recorded by trackBorrowTemp. One
    // definition so no call path can hand-roll (and forget) the drain.
    void drainBorrowTemps(const std::vector<std::pair<llvm::Value*, VarKind>>& temps) {
        for (auto& [v, k] : temps) emitDecrefByKind(v, k);
    }

    // Cleanup-stack registration for one owned temp so a raising callee frees
    // it during longjmp; pop `bases` after the call (popArgTempCleanups). Same
    // kind filter as pushArgTempCleanups: Union stays on the normal-path drain.
    void pushTempCleanupByKind(llvm::Value* v, VarKind k,
                               std::vector<llvm::Value*>& bases) {
        int ck = cleanupKindFor(k);
        if (ck == DCLEAN_STR || ck == DCLEAN_CALLABLE || ck == DCLEAN_OBJ)
            bases.push_back(emitCleanupPushTemp(v, ck));
    }

    // trackBorrowTemp plus the cleanup-stack registration above; pop `bases`
    // after the call, then drainBorrowTemps.
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

    // D027.1 heap-boxed cell read/write helpers. Cells store values as i64; native types
    // round-trip through bitcast/zext/ptrtoint. Heap kinds obey "incref new before set, decref old after" so RC stays balanced across overwrite even when old==new.

    /// Cast a native LLVM value at `kind`'s natural type to i64 for cell storage.
    llvm::Value* nativeToCellI64(llvm::Value* val, VarKind kind) {
        auto* ty = val->getType();
        if (ty == i64Type) return val;
        if (ty == i1Type) return builder->CreateZExt(val, i64Type, "cell.zext");
        if (ty == f64Type) return builder->CreateBitCast(val, i64Type, "cell.fbits");
        if (ty->isPointerTy()) return builder->CreatePtrToInt(val, i64Type, "cell.ptoi");
        // Default: bitcast scalar -> i64 if same width, else truncate / extend.
        if (ty->isIntegerTy()) {
            unsigned bits = ty->getIntegerBitWidth();
            if (bits < 64) return builder->CreateZExt(val, i64Type, "cell.zext");
            if (bits > 64) return builder->CreateTrunc(val, i64Type, "cell.trunc");
            return val;
        }
        return val;
    }

    /// Cast a cell-stored i64 back to the native LLVM type for `kind`.
    llvm::Value* cellI64ToNative(llvm::Value* i64Val, VarKind kind);

    /// Allocates a fresh cell for a `nonlocal`-promoted local; the caller owns the +1 of
    /// any heap value in `valueI64` (no auto-incref). D030 §5: prefers the source Type::Kind for tag derivation when available, so bytes-typed cells keep TAG_BYTES after VarKind::Bytes collapses into the generic-heap cohort.
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

    /// Read a cell-backed local. Returns the value at the var's native LLVM
    /// type (the same shape NameExpr would have produced for a non-cell var).
    llvm::Value* emitCellRead(llvm::AllocaInst* alloca, VarKind kind,
                              const std::string& name) {
        auto* cellPtr = builder->CreateLoad(i8PtrType, alloca, name + ".cell");
        auto* raw = builder->CreateCall(
            runtimeFuncs["dragon_cell_get"], {cellPtr}, name + ".raw");
        return cellI64ToNative(raw, kind);
    }

    /// Writes to a cell-backed local. Borrowed values (name/field/element read) get an
    /// incref; owned fresh values (concat/call result) already carry the +1. Increfing an owned value too leaked one string per mutation (534KB/1000 iterations under LSan, a `nonlocal` accumulator). Decrefs the prior contents so the cell stays balanced; `newIsBorrowed` defaults true (safe for self-aliasing `s = s`).
    void emitCellWrite(llvm::AllocaInst* alloca, VarKind kind,
                       llvm::Value* newVal, const std::string& name,
                       bool newIsBorrowed = true) {
        auto* cellPtr = builder->CreateLoad(i8PtrType, alloca, name + ".cell.w");
        // Incref borrowed new before swap-in so a self-aliasing write (s = s)
        // doesn't momentarily drop refcount to zero.
        if (newIsBorrowed) emitIncrefByKind(newVal, kind);
        auto* newI64 = nativeToCellI64(newVal, kind);
        auto* oldI64 = builder->CreateCall(
            runtimeFuncs["dragon_cell_set"], {cellPtr, newI64}, name + ".old");
        if (isHeapKind(kind) && kind != VarKind::Union) {
            // Decref via the kind's native pointer type, same dispatch as emitDecrefByKind
            // reconstructed from i64. The first write sees old == 0 (fresh cell); decref(NULL) is a no-op.
            auto* oldPtr = builder->CreateIntToPtr(oldI64, i8PtrType, name + ".old.p");
            emitDecrefByKind(oldPtr, kind);
        }
    }

    // Emit conditional decref for a union-typed variable based on its runtime tag.
    // Only decrefs if the tag indicates a heap type (str, list, dict, bytes, etc.).
    void emitUnionDecref(llvm::Value* val, llvm::Value* tag);

    // Emit conditional incref for a union-typed variable based on its runtime tag.
    void emitUnionIncref(llvm::Value* val, llvm::Value* tag);

    // RC-aware store with overwrite cleanup for heap-typed slots: (1) incref new if the
    // RHS is borrowed, (2) decref old occupant (guarded against self-assignment), (3) store new value.
    void storeWithRCOverwrite(llvm::Value* slotPtr, llvm::Type* slotValueType,
                              llvm::Value* newVal,
                              VarKind oldKind, VarKind newKind,
                              bool newIsBorrowed,
                              const std::string& name = "");

    // If `name` denotes a borrowed slot (param, loop var, capture, self), clears the
    // borrowed mark and returns true, so storeWithRCOverwrite's first reassignment skips the old-value decref (caller owns that ref) but the slot cleans up at scope exit thereafter. Walks scopes like setVar.
    bool consumeBorrowedSlot(const std::string& name);

    // Non-mutating peek of the mark consumeBorrowedSlot clears: is `name`'s innermost
    // binding currently borrowed? Gates the owned-str->StrLiteral downgrade guard: a literal store must keep an owned slot's cleanup kind Str, but never promote a borrowed slot (decref on a not-taken branch would UAF).
    bool isBorrowedSlot(const std::string& name) {
        if (name.empty()) return false;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            bool hasVar = it->vars.count(name) != 0;
            bool isBorrowed = it->borrowed.count(name) != 0;
            if (hasVar || isBorrowed)
                return isBorrowed;  // owned binding shadows any outer borrowed mark
        }
        return false;
    }

    // Emits an amortized in-place string append for `slot = slot + rhs` / `slot += rhs`.
    // The runtime entry point consumes the slot's old ref and returns the new value, so it must be plain-stored, not routed through storeWithRCOverwrite (which would double-consume `cur`). `rhs` is only borrowed; decref it here iff an owned intermediate (mirrors dragon_str_concat/Expressions.cpp).
    void emitStrAppendInplace(llvm::Value* slotPtr, llvm::Value* cur,
                              llvm::Value* rhs, const std::string& name);

    // Lowers Python `%`/`//` (floor semantics, tracking the divisor's sign, unlike C's
    // truncated srem/sdiv) inline instead of per-use runtime calls, whose overhead dominated tight loops. Strategy by divisor shape: nonzero constant -> inline + branchless floor-select; variable -> branch on b==0 (fallback path prints ZeroDivisionError+exit(1), nonzero path inline, phi merge); literal 0 -> keep the call.
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

        // Zero path: the runtime fallback prints ZeroDivisionError + exit(1) and never
        // returns, but isn't marked noreturn, so feed its dead result into the phi instead of a noreturn-lying `unreachable`.
        builder->SetInsertPoint(zeroBB);
        llvm::Value* zres = builder->CreateCall(runtimeFuncs[fallback], {a, b}, label);
        builder->CreateBr(contBB);
        auto* zeroExit = builder->GetInsertBlock();

        // Nonzero path: fully inline.
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
                // floor correction: if (r != 0 && (r ^ d) < 0) r += d
                llvm::Value* nz = builder->CreateICmpNE(r, zero, "mod.nz");
                llvm::Value* neg = builder->CreateICmpSLT(
                    builder->CreateXor(r, d, "mod.xor"), zero, "mod.neg");
                llvm::Value* fix = builder->CreateAnd(nz, neg, "mod.fix");
                llvm::Value* radj = builder->CreateAdd(r, d, "mod.adj");
                return builder->CreateSelect(fix, radj, r, "mod");
            });
    }

    // Floor division: q=a/b, corrected toward -inf when operands' signs differ and the
    // division was inexact (if (a^b)<0 && a%b!=0, q-=1). LLVM fuses an adjacent sdiv+srem into one idiv, so the srem check is near-free.
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

    // Computes `cur OP rhs` for an integer augmented-assignment token (i64 operands),
    // mirroring NameExpr's aug-assign path (reuses emitIntMod for `%=`). Returns nullptr for non-int ops (e.g. `/=`) so the caller skips. Used by list/dict element aug-assign.
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

    // Coerce a value to f64 for float arithmetic: i1->i64->f64, i64->f64, f64 as-is.
    // Returns nullptr if the value isn't a numeric type we can widen.
    llvm::Value* coerceToF64(llvm::Value* v) {
        if (v->getType() == i1Type) v = builder->CreateZExt(v, i64Type);
        if (v->getType() == i64Type) return builder->CreateSIToFP(v, f64Type);
        if (v->getType() == f64Type) return v;
        return nullptr;
    }

    // Computes `cur OP rhs` for a float augmented-assignment token (f64 operands), mirroring
    // emitIntAugOp; shared by dict/list/field aug-assign targets (NameExpr inlines directly). `//=`/`%=` use Python float floor/mod semantics; returns nullptr for bitwise/shift ops.
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

    // Python float floor-division `a // b` = floor(a / b). Uses the llvm.floor
    // intrinsic so it lowers to a single rounding instruction (roundsd) on x86.
    llvm::Value* emitFloatFloorDiv(llvm::Value* a, llvm::Value* b) {
        llvm::Value* q = builder->CreateFDiv(a, b, "ffdiv.q");
        llvm::Function* floorFn = llvm::Intrinsic::getOrInsertDeclaration(
            module.get(), llvm::Intrinsic::floor, {f64Type});
        return builder->CreateCall(floorFn, {q}, "ffdiv");
    }

    // Python float modulo: result takes the divisor's sign (unlike C fmod/LLVM frem, which
    // take the dividend's). Starts from frem, then adds b when the remainder is nonzero and disagrees in sign, matching CPython's float_mod.
    llvm::Value* emitFloatMod(llvm::Value* a, llvm::Value* b) {
        llvm::Value* zero = llvm::ConstantFP::get(f64Type, 0.0);
        llvm::Value* r = builder->CreateFRem(a, b, "fmod.r");
        llvm::Value* nz = builder->CreateFCmpONE(r, zero, "fmod.nz");
        // sign(r) != sign(b): (r < 0) xor (b < 0)
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

    // Like lookupVar but restricted to the innermost scope. A `:`-declaration uses this
    // to decide reuse-vs-shadow: a name resolving only in an enclosing scope must be shadowed with a fresh slot, not aliased onto the outer binding (risking a wrong-LLVM-type reinterpretation on a type change).
    llvm::AllocaInst* lookupVarInCurrentScope(const std::string& name) {
        if (scopes.empty()) return nullptr;
        auto found = scopes.back().vars.find(name);
        return found != scopes.back().vars.end() ? found->second : nullptr;
    }

    void setVar(const std::string& name, llvm::AllocaInst* alloca,
                 VarKind kind = VarKind::Other);

    // D027.1: walks the scope chain to check whether this name's alloca holds a DragonCell
    // pointer (not the value directly); reads/writes route through dragon_cell_get/set when true.
    bool isCellBacked(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->cellBacked.count(name)) return true;
            // Stop at the scope that actually defines the name, so a same-name shadow in a
            // deeper scope isn't confused with an outer cell-backed binding.
            if (it->vars.count(name)) return false;
        }
        return false;
    }

    // D027.1: marks a name in the innermost scope as cell-backed. Used at the cell-promoted
    // definition site (outer fn) and at the env-load site (inner fn holding a cell pointer for this capture).
    void markCellBacked(const std::string& name) {
        if (!scopes.empty()) scopes.back().cellBacked.insert(name);
    }

    // B Phase 1: marks a freshly-bound local as stack-allocated in its owning scope, so
    // block-exit cleanup skips the decref. Mirrors setVar's owning-scope search.
    void markStackAllocated(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->vars.count(name)) { it->stackAllocated.insert(name); return; }
        }
        if (!scopes.empty()) scopes.back().stackAllocated.insert(name);
    }

    // Set by the CallExpr constructor fork when it lowers a class construction to a
    // stack alloca (a no-escape site of a scalar-only class); the binding site reads+clears it to mark the local stack-allocated.
    bool lastWasStackInstance = false;

    // B Phase 1: ctor CallExpr* sites bound to a non-escaping local, populated by
    // computeStackAllocSites before the entry body emits. Keyed by AST node identity.
    std::unordered_set<const CallExpr*> stackAllocSites;

    // Task-detach tail: `t: Task[...] = fire ...` declarations whose local provably never
    // escapes the block (any use, including join/await/is_alive, counts as escape), capturing only the genuinely-unused fire-and-forget case that leaks the handle. Consulted at the binding site to arm scope.detachOnExit.
    std::unordered_set<const AnnAssignStmt*> detachableTaskDecls;

    // B Phase 1: classes eligible for stack construction (scalar-only fields, exactly one
    // non-self-escaping constructor, no per-instance field defaults, so memset+__init__ reproduces _new). Computed during class codegen, consulted at the CallExpr fork with stackAllocSites.
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
        // Per-module resolution: a foreign same-named global must not retype this read.
        std::string key = resolveGlobalKey(name);
        if (!key.empty()) {
            auto mgIt = moduleGlobalKinds.find(key);
            if (mgIt != moduleGlobalKinds.end()) return mgIt->second;
        }
        return VarKind::Other;
    }

    // Determines if an expression produces a bytes value. D030 §5: prefers the typechecker's
    // static type, falling back to AST node shape and source-level VarKind where a Type hasn't propagated yet (or for legacy VarKind::Bytes-tagged slots).
    bool exprIsBytes(Expr* expr);

    // Bare name -> moduleGlobals key from the current module's view: alias, then
    // own binding, else "". No cross-module fallthrough (that WAS the aliasing bug).
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

    // Write key for a global: the resolved key when visible, else the current
    // module's own key (new declaration).
    std::string globalKeyOrOwn(const std::string& name) const {
        std::string k = resolveGlobalKey(name);
        return k.empty() ? mangleGlobal(currentModuleName, name) : k;
    }

    // Look up a module global by bare name from the current module's view.
    llvm::GlobalVariable* lookupModuleGlobal(const std::string& name) {
        std::string key = resolveGlobalKey(name);
        if (key.empty()) return nullptr;
        return moduleGlobals.find(key)->second;
    }

    // Scoped class binding for an unshadowed class-typed global, else null.
    const GlobalClassBinding* globalClassBindingFor(const std::string& name) {
        if (lookupVar(name)) return nullptr;
        std::string key = resolveGlobalKey(name);
        if (key.empty()) return nullptr;
        auto it = moduleGlobalClassNames.find(key);
        if (it != moduleGlobalClassNames.end()) return &it->second;
        return nullptr;
    }

    // Bind a class-typed global under its mangled key; dual-writes the legacy
    // flat maps until every reader resolves through the scoped one.
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

    // Whether to use a module global for this name: always in .dr mode (scope chain
    // resolution); in .py mode, always at module level, or inside a function only if `global x` was declared.
    bool shouldUseModuleGlobal(const std::string& name) {
        // Mode-independent (.dr/.py parity): a function may read a module global with no
        // keyword in either mode, and this only runs after lookupVar found no shadowing local. Writes require `global`, enforced uniformly in Sema, so no mode gate or globalDeclaredVars check is needed here.
        (void)name;
        return true;
    }

    // Create alloca in function entry block (for stable stack)
    llvm::AllocaInst* createEntryAlloca(llvm::Function* func,
                                         const std::string& name,
                                         llvm::Type* type);

    // Resolves a (possibly dotted) NamedTypeExpr name to a class name in the flat classNames
    // table. Same-module names match directly; cross-module `mod.Foo` matches the trailing segment, since all linked modules share one LLVM symbol space.
    std::string resolveAnnotationClassName(const std::string& name) const {
        if (classNames.count(name)) return name;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            std::string leaf = name.substr(dot + 1);
            if (classNames.count(leaf)) return leaf;
        }
        return "";
    }

    // Binds a class-typed variable's class name AND owning module together (both maps are
    // program-wide, never cleared, so setting only the name let a stale owning module from another function survive and misdirect dispatch). Handles dotted `x: mod.Class` via resolveAnnotationClassName; no-op for non-class types.
    void bindClassVar(const std::string& varName, TypeExpr* typeExpr) {
        auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr);
        if (!named) return;
        std::string cn = resolveAnnotationClassName(named->name);
        if (cn.empty()) return;
        varClassNames[varName] = cn;
        varClassOwningModule[varName] = resolveClassOwningModule(cn);
    }

    // D044: Type::toString-equivalent canonical name for a TypeExpr, recovering the stamped
    // class name of a generic instantiation (`Box[int]`). Must match TypeChecker::mangleInstantiation (top-level args joined by ',') and Type::toString (builtin containers use ', ').
    std::string typeExprCanonicalName(TypeExpr* t) const;

    // If `t` is a generic-class instantiation annotation whose class was stamped
    // (`Box[int]` -> the stamped class "Box[int]"), return that class name; else "".
    std::string genericInstanceClassName(TypeExpr* t) const {
        if (!dynamic_cast<GenericTypeExpr*>(t)) return "";
        std::string c = typeExprCanonicalName(t);
        return classNames.count(c) ? c : "";
    }

    /// D030 §5: source-of-truth Type::Kind from a TypeExpr annotation, used wherever the
    /// static type must survive past the VarKind layer (e.g. typedDictFieldKindsBySym's bytes-vs-list tag dispatch). Mirrors typeExprToKind but returns Type::Kind directly.
    Type::Kind typeExprToTypeKind(TypeExpr* typeExpr);

    VarKind typeExprToKind(TypeExpr* typeExpr);

    // Extract member VarKinds from a union type annotation
    std::vector<VarKind> typeExprToUnionMembers(TypeExpr* typeExpr) {
        std::vector<VarKind> members;
        if (auto* ut = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
            for (auto& t : ut->types) {
                members.push_back(typeExprToKind(t.get()));
            }
        }
        return members;
    }

    // Extracts the class-name member of a union type, if any, to recover the concrete
    // class for narrowing `Foo | None` -> Foo so `x.field` finds the right struct layout.
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

    // Niche-pointer optimization: when a Union is exactly `T | None` and T is pointer-shaped,
    // lower as a bare nullable pointer instead of a {i64,i64} box (null = None, `r != none` is a 1-cycle compare, `r.field` a plain load). Boxed Union stays for non-niche unions like `int | str`. Returns the non-None member's TypeExpr*, or nullptr.
    TypeExpr* unionNicheMember(TypeExpr* typeExpr);

    // D030 Phase 4: box helpers ({i64 tag, i64 payload}).

    /// Coerces a native-typed value to the box's i64 payload slot: floats bitcast, pointers
    /// PtrToInt, bools ZExt, i64/int pass through. Caller decides the tag separately.
    llvm::Value* nativeToPayloadI64(llvm::Value* val) {
        auto* ty = val->getType();
        if (ty == i64Type) return val;
        if (ty == f64Type) return builder->CreateBitCast(val, i64Type, "box.payload.f");
        if (ty == i1Type)  return builder->CreateZExt(val, i64Type, "box.payload.b");
        if (ty->isPointerTy()) return builder->CreatePtrToInt(val, i64Type, "box.payload.p");
        if (ty->isIntegerTy()) return builder->CreateSExt(val, i64Type, "box.payload.i");
        return val;
    }

    /// Build a `{i64, i64}` box value from a tag and a native-typed payload.
    /// The payload is converted to i64 storage shape via nativeToPayloadI64.
    llvm::Value* makeBox(llvm::Value* tag, llvm::Value* payloadNative) {
        llvm::Value* payloadI64 = nativeToPayloadI64(payloadNative);
        llvm::Value* box = llvm::UndefValue::get(boxType);
        box = builder->CreateInsertValue(box, tag, 0, "box.t");
        box = builder->CreateInsertValue(box, payloadI64, 1, "box");
        return box;
    }

    /// Build a box from a constant tag (TAG_INT, TAG_STR, ...) and a native payload.
    llvm::Value* makeBoxConstTag(int64_t tagConst, llvm::Value* payloadNative) {
        return makeBox(llvm::ConstantInt::get(i64Type, tagConst), payloadNative);
    }

    /// Extract the tag (i64) from a box value.
    llvm::Value* boxTag(llvm::Value* box, const std::string& name = "tag") {
        return builder->CreateExtractValue(box, 0, name);
    }

    /// Extract the raw payload (i64) from a box value. Caller narrows to native.
    llvm::Value* boxPayloadI64(llvm::Value* box, const std::string& name = "payload") {
        return builder->CreateExtractValue(box, 1, name);
    }

    /// Extracts the payload as a native LLVM type matching `kind`. Used by isinstance
    /// narrowing: once `box.tag == tag(T)` is verified, gives the value at T's native type.
    llvm::Value* boxPayloadAsKind(llvm::Value* box, VarKind k);

    // D039 Phase 11: arithmetic op token -> dragon_box_binop opcode, handling both the
    // BinaryExpr (PLUS) and AugAssign (PLUS_EQUAL) forms. Returns -1 for a non-box-arithmetic token.
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

    // Boxes a native arithmetic operand for dragon_box_binop: a numeric LLVM type maps
    // directly to its value-tag, a pointer's tag comes from the expr's static type. Already-boxed values pass through unchanged. Borrow semantics: reads, never owns.
    llvm::Value* boxNativeOperand(CodeGen& cg, Expr* e, llvm::Value* v) {
        if (v->getType() == boxType) return v;
        llvm::Value* tag;
        if (v->getType() == i64Type)
            tag = llvm::ConstantInt::get(i64Type, TAG_INT);   // TAG_INT
        else if (v->getType() == f64Type)
            tag = llvm::ConstantInt::get(i64Type, TAG_FLOAT);   // TAG_FLOAT
        else if (v->getType() == i1Type)
            tag = llvm::ConstantInt::get(i64Type, TAG_BOOL);   // TAG_BOOL
        else
            tag = emitTagForExpr(e, cg);                // ptr: str/list/bytes/...
        return makeBox(tag, v);
    }

    // Emit dragon_box_binop(boxA, boxB, op) for two operands where at least one
    // is a box, boxing the native side(s). Returns the result box.
    llvm::Value* emitBoxBinop(CodeGen& cg, Expr* lExpr, llvm::Value* lhs,
                              Expr* rExpr, llvm::Value* rhs, int64_t opcode) {
        llvm::Value* boxA = boxNativeOperand(cg, lExpr, lhs);
        llvm::Value* boxB = boxNativeOperand(cg, rExpr, rhs);
        llvm::Value* res = builder->CreateCall(runtimeFuncs["dragon_box_binop"],
            {boxA, boxB, llvm::ConstantInt::get(i64Type, opcode)}, "box.binop");
        drainOwnedNativeBoxOperands(lhs, rhs);
        return res;
    }

    // boxNativeOperand borrows (the box only reads the payload), so a native ptr operand
    // that's itself an owned temp is orphaned once the call returns; drain it here (names/field reads are loads, screened out by isOwnedPtrResult).
    void drainOwnedNativeBoxOperands(llvm::Value* lhs, llvm::Value* rhs) {
        if (options.gcMode != GCMode::RC) return;
        for (llvm::Value* v : {lhs, rhs}) {
            if (v->getType() != boxType && v->getType()->isPointerTy() &&
                isOwnedPtrResult(v))
                builder->CreateCall(runtimeFuncs["dragon_decref"], {v});
        }
    }

    // Emits dragon_box_cmp for an ordering operator where at least one operand is a box,
    // returning a three-way i64 (<0/0/>0) the caller compares to 0. cmpOp (0=</1=<=/2=>/3=>=) is used only for the TypeError message on incomparable operands.
    llvm::Value* emitBoxCmp(CodeGen& cg, Expr* lExpr, llvm::Value* lhs,
                            Expr* rExpr, llvm::Value* rhs, int64_t cmpOp) {
        llvm::Value* boxA = boxNativeOperand(cg, lExpr, lhs);
        llvm::Value* boxB = boxNativeOperand(cg, rExpr, rhs);
        llvm::Value* res = builder->CreateCall(runtimeFuncs["dragon_box_cmp"],
            {boxA, boxB, llvm::ConstantInt::get(i64Type, cmpOp)}, "box.cmp");
        drainOwnedNativeBoxOperands(lhs, rhs);
        return res;
    }

    // Sentinel for `wantListElemTag`: skip the list representation check.
    static constexpr int64_t kNoListElemCheck =
        std::numeric_limits<int64_t>::min();

    // Unboxes a dragon_box_binop result into `targetType`, emitting a runtime tag-check
    // that raises TypeError (80) on mismatch (mirrors AnnAssign's D039 Phase-7a inline unbox); targetType == box type returns unchanged (Any slot). `wantListElemTag` additionally emits dragon_list_view_check, since the box tag alone can't distinguish DragonList from DragonListBox (silent corruption otherwise): -1 for list[Any]/union, >=0 for concrete list[T].
    // `staticKind` disambiguates targets whose VarKind collapses onto a generic heap
    // kind (bytes -> VarKind::List per D030 §5), else the tag check misreports them.
    llvm::Value* unboxBoxResultChecked(llvm::Value* box, llvm::Type* targetType,
                                       VarKind vk,
                                       int64_t wantListElemTag = kNoListElemCheck,
                                       Type::Kind staticKind = Type::Kind::Unknown);

    /// The dragon_list_view_check argument for a list-typed slot, from its annotation: -1
    /// for list[Any]/union (box representation), a concrete tag for monomorphized elements, kNoListElemCheck for a non-checkable shape (bare `list`, `list[type]`, type variables).
    int64_t listViewWantElemTag(TypeExpr* ann);

    /// Converts a raw i64 container slot (dragon_tuple_get/list_get) to the native LLVM
    /// value for `elemType`, mirroring Assign.cpp's tuple-unpack coercion. Used by C9-B `*tuple`/`*list` spread to feed container elements into typed parameter slots.
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
                return raw;  // Int (and Any-as-int fallback)
        }
    }

    // emitTagForExpr: runtime tag for an expr passed as a union arg (i64 constant for known
    // types, or extracted from a boxed union var). boxArgTagPayload: {tag,payload} pair for an Any/box value passed to a box-list op; `takesOwnership` increfs borrowed heap payloads (false = inspection only, e.g. remove's equality search).
    std::pair<llvm::Value*, llvm::Value*> boxArgTagPayload(
            Expr* argExpr, llvm::Value* val, bool takesOwnership);

    llvm::Value* emitTagForExpr(Expr* expr, CodeGen& cg);

    // Convert Dragon type annotation to LLVM type
    llvm::Type* typeExprToLLVM(TypeExpr* typeExpr);

    // coerceArg (below): D030 Phase 5 audit - legitimate widenings are int<->float,
    // bool<->int/float, intc bridges (FFI); ptr<->int is kept because some class fields still store ptr-returning RHS as i64 (upstream fix tracked, deleting here regresses io/re/sqlite/threading tests).
    /// coerceArgFromExpr: D030 Phase 4 call-boundary - crossing into an Any/Union param
    /// (paramType == boxType) emits a `%dragon.box` here, the inverse of D039 Phase 7a's unbox at the store site. Uses emitTagForExpr for ptr-shaped TAG_* derivation; else defers to coerceArg.
    llvm::Value* coerceArgFromExpr(Expr* expr,
                                    llvm::Value* arg,
                                    llvm::Type* paramType) {
        if (paramType == boxType) {
            // Boxes an arg into an Any/Union param via the shared boxing path.
            // takesOwnership=false: an Any param is borrowed today (no +1); completing the "donate" contract (incref here, callee frees at scope exit) needs an ownership-flow analysis the escape pass can't yet provide.
            auto tp = boxArgTagPayload(expr, arg, /*takesOwnership=*/false);
            return makeBox(tp.first, tp.second);
        }
        return coerceArg(arg, paramType);
    }

    /// Same tag derivation as `emitTagForExpr` but without the unused CodeGen& parameter,
    /// so `coerceArgFromExpr` (no CodeGen reference) can call it. Existing `cg`-passing call sites keep using emitTagForExpr.
    llvm::Value* emitTagForExprNoCG(Expr* expr);

    llvm::Value* coerceArg(llvm::Value* arg, llvm::Type* paramType);

    // Normalize a value to i64 if it's intc - used after function calls
    // so that the rest of Dragon codegen always works with i64 integers.
    llvm::Value* normalizeIntC(llvm::Value* val) {
        if (val->getType() == intcType)
            return builder->CreateSExt(val, i64Type, "intc_ext");
        return val;
    }

    // D030: per-callsite spawn trampolines (fire/async/generator).

    /// Casts a return value of arbitrary type to i64 for transit through the vthread
    /// result slot. Mirrors the inverse of coerceArg's widenings.
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

    // Inverse of resultToI64: reinterprets the i64 result from dragon_vthread_join at
    // the task's native type T. A float was bit-packed, so bitcast back (not coerceArg's SIToFP, which would corrupt the bits); ptr inttoptr, bool truncates. Union/Any (a 16-byte box) can't fit the i64 slot: a pre-existing vthread-ABI limit, left as raw i64.
    llvm::Value* taskResultFromI64(llvm::Value* rawI64, Type* resultType);

    /// Builds the per-callsite typed args struct: { ptr handle, <native_arg_types...> }.
    /// Field 0 is reserved for the runtime to patch (DragonVThread*/DragonGenerator*) so the trampoline can address its result/self slot; the rest are user args at native types.
    llvm::StructType* makeSpawnArgsStructType(
        const std::vector<llvm::Type*>& argTypes,
        const std::string& name) {
        std::vector<llvm::Type*> fields;
        fields.push_back(i8PtrType);  // field 0: handle (vthread or generator)
        for (auto* t : argTypes) fields.push_back(t);
        return llvm::StructType::create(*context, fields, name);
    }

    /// Coerces a value loaded from/stored to a struct field of a native LLVM type. Used
    /// at spawn-site populate where the caller's value type may not exactly match the field.
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

    /// Builds a per-callsite fire/async trampoline: pulls the args struct from user_data,
    /// loads the vthread handle + native args, calls targetFn, sets the vthread result, atomically decrefs heap args (balancing the spawn-site incref), frees the buffer. Caller must atomic-incref heap args before spawn; targetFn may be a function or method.
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

    /// Builds a per-defer-site thunk `void __dragon_defer_<site>(i64* args)` that loads
    /// targetFn's args from the i64 snapshot array and calls it, discarding the result. One thunk serves both exit paths (inline normal exit, DCLEAN_DEFER_CALL during longjmp unwind). vtableIndex >= 0 dispatches through the vtable for D026 override parity.
    llvm::Function* buildDeferThunk(llvm::Function* targetFn,
                                    const std::string& siteName,
                                    int vtableIndex = -1);

    /// Builds a per-callsite generator trampoline: pulls the args struct from user_data,
    /// loads the generator handle + native args, calls the body fn, sets exhausted. The args buffer is owned by the generator and freed at destroy via buildGeneratorDecrefFn.
    llvm::Function* buildGeneratorTrampoline(
        llvm::Function* bodyFn,
        llvm::StructType* argsStructType,
        const std::string& siteName);

    /// Builds a per-callsite decref fn for a generator's args buffer, called by
    /// dragon_generator_destroy; walks heap-typed slots and atomic-decrefs each (destroy can race with a worker thread). Returns NULL if there are no heap args.
    llvm::Function* buildGeneratorDecrefFn(
        llvm::StructType* argsStructType,
        const std::vector<VarKind>& argKinds,
        const std::string& siteName);

    /// Populates a stack-allocated spawn args struct with user args. Field 0 is left zero
    /// (runtime patches it to the vthread/generator); user args go in fields 1..N at their native types.
    void populateSpawnArgs(
        llvm::Value* argsAlloca,
        llvm::StructType* argsStructType,
        const std::vector<llvm::Value*>& userArgs);

    // D030 Phase 3.B: binds a list element to a native-typed alloca.

    /// Issues the matching typed get for the loop variable's kind and binds a fresh alloca
    /// of the native type (for-loops/comprehensions over typed lists; caller handles setVar/borrowed-insertion/varClassNames). Float->list_get_f64, heap kinds->list_get_ptr, Bool->list_get truncated, else->list_get as i64.
    llvm::AllocaInst* bindListElemTyped(
        llvm::Function* func,
        llvm::Value* listVal,
        llvm::Value* idx,
        const std::string& varName,
        VarKind loopKind);

    /// D030 §5: Type::Kind-driven loop-var binder. Sizes the alloca and picks the matching
    /// `dragon_list_get_*` call directly from the element Type::Kind, bypassing the legacy VarKind hop. Single source of truth for a list[T] loop var's LLVM shape.
    llvm::AllocaInst* bindListElemByTypeKind(
        llvm::Function* func,
        llvm::Value* listVal,
        llvm::Value* idx,
        const std::string& varName,
        Type::Kind elemKind);

    // Emits a string-literal byte sequence as an LLVM i8* pointer. ASCII uses a fast global
    // C-string (kind=1 borrowed buffer); bytes >= 0x80 can't use a raw C-string (dragon_str_concat would read one byte as one cp, mangling UTF-8 into double-encoded Latin-1 mojibake), so each distinct sequence gets a module-level i8* slot interned once via dragon_str_intern. Single point of truth for lowering literal text (StringLiteral, template/f-string segments).
    llvm::Value* emitStringLiteralBytes(const std::string& bytes,
                                        const llvm::Twine& twine = "");

    // Process escape sequences in string/bytes literals at compile time
    std::string processEscapes(const std::string& raw, bool isRaw);

    // Get or declare a runtime function
    llvm::Function* getOrDeclareRuntime(const std::string& name,
                                         llvm::FunctionType* funcType);

    void declareRuntimeFunctions(); // defined in codegen/ImplInit.cpp

    void addError(const std::string& msg, SourceLocation loc = {}) {
        diagnostics.push_back({CodeGenDiagnostic::Level::Error, loc, msg});
    }

    void runOptimizationPasses();

    // Determine what type an expression will produce as LLVM Value
    llvm::Type* inferExprLLVMType(Expr* expr);

    // Forward-declare all top-level functions in a module
    void forwardDeclareFunctions(dragon::Module& mod); // defined in codegen/ImplInit.cpp

    // When true, visit(ClassDecl) registers field-layout metadata only and returns before
    // emitting bodies/globals: the pre-pass over all classes before any method body, so cross-class field references to later-defined classes resolve.
    bool classLayoutPass = false;

    // Forward-declare class constructors and methods in a module.
    void forwardDeclareClasses(dragon::Module& mod); // defined in codegen/ImplInit.cpp
    // 6.18: AST-level synthesis of __init__/__eq__/__repr__ for @dataclass-decorated classes
    // and NamedTuple subclasses. Defined in codegen/Classes.cpp; mutates the class body in place.
    void synthesizeDataclassMethods(ClassDecl& node);

    // AST-level synthesis for class-based enums (`class C(Enum)`): rewrites members into
    // singleton instances with name/value fields, __init__, __str__/__repr__, __members__, and a value-lookup helper. Mutates the class body in place; must run before synthesizeDataclassMethods.
    void synthesizeEnumMethods(ClassDecl& node);
};

} // namespace dragon

#endif // DRAGON_CODEGEN_IMPL_H
