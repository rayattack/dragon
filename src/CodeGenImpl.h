#ifndef DRAGON_CODEGEN_IMPL_H
#define DRAGON_CODEGEN_IMPL_H

/// Dragon CodeGen - Private Implementation Header
/// Contains CodeGen::Impl struct definition shared across all codegen TUs.

#include <execinfo.h>
#include <limits>
#include "dragon/CodeGen.h"
#include "dragon/TypeChecker.h"
#include "dragon/StdlibRegistry.h"
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

//===----------------------------------------------------------------------===//
// Impl
//===----------------------------------------------------------------------===//

struct CodeGen::Impl {
    CodeGenOptions options;
    std::vector<CodeGenDiagnostic> diagnostics;

    // LLVM core
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Value stack: visitor methods push results here
    llvm::Value* lastValue = nullptr;

    // Variable storage: name -> alloca
    // Variable type tags for distinguishing ptr-typed variables (list vs str)
    // VarKind::Str = dynamic string from runtime (has DragonObjectHeader, decref with dragon_decref_str)
    // VarKind::StrLiteral = compile-time string literal (no header, never decref)
    // VarKind::ClassInstance = user-defined class instance (has GC header prepended to struct, decref with dragon_decref)
    // D030 §5: VarKind::Bytes deleted - bytes-typed slots use VarKind::List
    // (generic-heap dispatch); the bytes-vs-list distinction now flows
    // through Type::Kind / typeKindToTag at every consumer.
    enum class VarKind { Int, Float, Bool, Str, StrLiteral, List, Dict, Tuple, Set, File, ClassInstance, Generator, Type, Closure, Union, Deque, Other };

    struct Scope {
        std::unordered_map<std::string, llvm::AllocaInst*> vars;
        std::unordered_map<std::string, VarKind> varKinds;
        std::unordered_set<std::string> borrowed;  // params - don't decref at scope exit
        // D027.1: heap-boxed via DragonCell. The alloca holds the cell ptr;
        // reads route through dragon_cell_get, writes through dragon_cell_set.
        // Set both at the cell-promoted definition site (the owning function)
        // and at the env-load site in any nested fn that captured the cell.
        std::unordered_set<std::string> cellBacked;
        // B Phase 1 (escape analysis): class instances allocated in an entry
        // alloca rather than the heap. The var is an ordinary ClassInstance for
        // field access / method dispatch, but it is never malloc'd / gc_tracked,
        // so scope cleanup must NOT decref it (the storage is reclaimed when the
        // stack frame unwinds). Only non-escaping locals of scalar-only classes
        // land here, so there are no heap children to tear down.
        std::unordered_set<std::string> stackAllocated;
        // Bound-Task tail: Task locals bound to a `fire ...` that
        // PROVABLY never escape and are never joined/awaited (a bound
        // fire-and-forget) - dragon_vthread_detach them at scope exit so the
        // handle ref isn't leaked. Populated only at the binding site for decls
        // in `detachableTaskDecls`; detach is idempotent with join (the `joined`
        // CAS), so it is safe even if a later edit adds a join.
        std::unordered_set<std::string> detachOnExit;
        // docs/002 ADR 2.10: bare Lock LOCALS are owned by their scope and
        // destroyed at scope exit (null-gated: a `del` nulls the slot, so a
        // deleted lock is skipped). Statically sound because OwnershipCheck
        // forbids every second-owner path for a raw resource: E8 refuses
        // borrow stores into own fields, E15 makes resource fields own-only,
        // E16 bans container elements, and plain call args only borrow.
        // Module-level Locks are NOT armed (2.10: globals live for the
        // process).
        std::unordered_set<std::string> lockDestroyOnExit;
        // Exception-unwind cleanup (see DragonCleanupStack). cleanupSlots maps an
        // owned heap local's name -> the i32 alloca holding its runtime cleanup
        // slot index, so a reassignment can refresh the snapshot. cleanupBaseAlloca
        // holds the cleanup depth captured at this scope's FIRST push; normal scope
        // exit rewinds the cleanup stack to it (so a later sibling exception cannot
        // re-free locals already decref'd here).
        std::unordered_map<std::string, llvm::AllocaInst*> cleanupSlots;
        llvm::AllocaInst* cleanupBaseAlloca = nullptr;
        // defer f(x) snapshots (defer.md): appended by visit(DeferStmt) in
        // source order; emitScopeCleanupFor calls them in REVERSE (LIFO)
        // ahead of the RC decref pass on every exit edge, so borrowed
        // snapshots are alive at call time. argSlots is an [argc x i64]
        // entry alloca holding the snapshot values (written at the defer
        // statement). drainKinds[i] != VarKind::Other means slot i owns a +1
        // released after the deferred call runs; a value an own param adopts
        // carries VarKind::Other (the callee consumed it). The straight-line
        // append order also gives exit edges emitted mid-scope exactly the
        // defers whose statements precede them - registration is lexical.
        struct DeferEntry {
            llvm::Function* thunk = nullptr;      // void(i64*) per-site
            llvm::AllocaInst* argSlots = nullptr; // [max(argc,1) x i64]
            unsigned argc = 0;
            std::vector<VarKind> drainKinds;
        };
        std::vector<DeferEntry> deferred;
    };
    std::vector<Scope> scopes;

    // D027.1: walk a function body collecting `mutatedCapturedVars` from
    // every nested FunctionDecl and LambdaExpr. Used to compute which of
    // the OUTER function's locals must be cell-promoted (their addresses
    // are taken by inner mutations). Recurses through statement bodies
    // so a `nonlocal` declared two levels deep still surfaces here.
    void collectNestedMutatedCaptures(const std::vector<std::unique_ptr<Stmt>>& body,
                                      std::unordered_set<std::string>& out);
    void collectNestedMutatedCaptures(Stmt* s,
                                      std::unordered_set<std::string>& out);
    void collectNestedMutatedCaptures(Expr* e,
                                      std::unordered_set<std::string>& out);

    // B Phase 1 escape analysis (src/codegen/EscapeAnalysis.cpp). Walks the
    // entry module's top-level statements + function bodies; for each
    // `v: T = T(args)` declaration of a class instance whose binding `v`
    // provably does not escape its declaring block, records the ctor CallExpr*
    // in `stackAllocSites`. Conservative: any use of `v` other than a plain
    // `v.field` read disqualifies (default-escapes). The CallExpr fork applies
    // the remaining gates (scalar-only class, single trivial non-self-escaping
    // ctor) using authoritative class metadata.
    void computeStackAllocSites(Module& entryModule);
    // isModuleTopLevel: direct children of the module body are module globals
    // (whole-program visibility - an earlier-defined function may reference
    // them), so they are NEVER stack-allocated; only their nested blocks are
    // analyzed. Function bodies and nested blocks get full candidate detection.
    void analyzeBlockForStackAlloc(const std::vector<std::unique_ptr<Stmt>>& stmts,
                                   bool isModuleTopLevel = false);
    // True if `name` is used in a way that lets the instance escape its
    // declaring block (or is rebound). Default-escapes for unhandled nodes.
    bool exprEscapes(Expr* e, const std::string& name);
    bool stmtEscapes(Stmt* s, const std::string& name);
    // Exhaustive (DefaultASTVisitor-based) "does `name` appear anywhere in this
    // subtree" - the sound fallback for nodes exprEscapes/stmtEscapes don't
    // special-case, and for capture sites (lambda/fire/thread/nested def).
    bool nodeMentionsName(Expr* e, const std::string& name);
    bool nodeMentionsName(Stmt* s, const std::string& name);

    // Task-detach tail (refined): does Task local `name` TRANSFER out of its
    // scope - returned, stored, passed as an argument, captured by a
    // lambda/fire/thread/nested def, or rebound - as OPPOSED to merely being
    // CONSUMED (`await t` / `t.join()`) or READ (`t.is_alive()`)? A consume/read
    // is safe to ALSO detach at scope exit (idempotent with join via the runtime
    // `joined` CAS); a transfer must NOT be detached (the new owner still needs
    // the handle -> detaching early = UAF). DISTINCT from exprEscapes/stmtEscapes
    // (deliberately NOT shared with stack-alloc, so it can never weaken that
    // analysis): the carve-out for await/join/is_alive applies ONLY at the
    // current scope's top level - inside a capture body ANY mention is a transfer
    // (the closure took the variable). Conservative: any unrecognized mention is
    // a transfer (leak > UAF).
    bool taskLocalTransferEscapes(Stmt* s, const std::string& name);
    // A nested def / lambda / fire / thread body referencing `name` is a
    // capture ⇒ escape. Also covers ctor self-escape via target="self".
    std::unordered_set<std::string> cellPromotedLocals;

    // Current function being generated
    llvm::Function* currentFunction = nullptr;

    // Loop control: break/continue targets
    struct LoopInfo {
        llvm::BasicBlock* breakBlock;
        llvm::BasicBlock* continueBlock;
        size_t scopeDepth;  // scopes.size() when loop was entered (before body push)
        size_t tryFrameDepth = 0;  // tryFrameFuncs.size() at loop entry - break/
                                   // continue pop the try/with frames opened
                                   // inside the loop body (those above this).
        size_t exitCleanupDepth = 0;  // exitCleanupStack.size() at loop entry -
                                      // break/continue replay only the finally /
                                      // with __exit__ cleanups opened inside the
                                      // loop body, not ones enclosing the loop.
    };
    std::stack<LoopInfo> loopStack;
    std::vector<llvm::Function*> tryFrameFuncs;

    // Count of the current function's live try/with exception frames (the
    // trailing run of tryFrameFuncs equal to currentFunction). A `return`
    // escapes all of them.
    size_t currentFnTryFrames() {
        size_t n = 0;
        for (auto it = tryFrameFuncs.rbegin(); it != tryFrameFuncs.rend(); ++it) {
            if (*it != currentFunction) break;
            ++n;
        }
        return n;
    }

    // Emit `n` dragon_exc_pop_frame calls at the current insertion point (no-op
    // if it is already terminated). Used by return/break/continue to unwind the
    // exception frames their jump bypasses.
    void emitExcFramePops(size_t n) {
        if (n == 0) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (size_t i = 0; i < n; ++i)
            builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
    }

    // One escaped-scope cleanup action attached to a `with` context manager:
    // call __exit__ (class CM) or release a lock. Carries the SSA context handle
    // (defined at with-entry, which dominates every early-exit point in the body).
    struct WithCleanupItem {
        bool isClassCtx;
        bool isLock;
        std::string className;
        llvm::Value* val;
        llvm::Value* enterResult = nullptr;  // __enter__ result (class CMs); may == val
        bool isLockTemp = false;  // `with Lock()` - an anonymous lock the `with`
                                  // OWNS: destroy (not just release) it on exit.
        bool subjectOwned = true;  // false when the subject expression is a
                                   // BORROW (bound local / attribute / walrus):
                                   // its slot owns the manager, so with-exit
                                   // must NOT decref `val` (A/B-proven UAF,
                                   // test_rc_with_subject.dr). The __enter__
                                   // result's +1 is always the with's to drop.
    };

    // Unified exit-cleanup stack: a try/finally body OR a with-statement's
    // __exit__/lock-release set, pushed in lexical nesting order (innermost
    // last). return/break/continue replay the escaped frames innermost-first
    // BEFORE jumping, so finally bodies AND `with` __exit__/lock-release run on
    // every exit edge - not just the normal fall-through and the longjmp path
    // (an early `return` skipped __exit__/release; break/continue
    // additionally produced invalid IR). One stack rather than two so a `with`
    // nested in a try-finally (and vice-versa) interleave in the correct order.
    struct ExitCleanup {
        bool isWith = false;
        std::vector<Stmt*> finallyBody;          // isWith == false (owned by TryStmt)
        std::vector<WithCleanupItem> withItems;  // isWith == true
        llvm::Function* func = nullptr;          // owning function (depth isolation)
        // scopes.size() when this entry was pushed. Early exits interleave
        // exit-cleanup replays with per-scope cleanup by nesting depth
        // (emitEarlyExitCleanups), so a defer registered INSIDE a try body
        // runs before that try's finally, matching the normal-exit order.
        size_t scopeDepth = 0;
    };
    std::vector<ExitCleanup> exitCleanupStack;

    // Index below which exitCleanupStack entries belong to ENCLOSING functions
    // (a nested fn/lambda/comprehension emitted inline keeps its parent's frames
    // on the stack); a `return` replays only the trailing run owned by
    // currentFunction. Mirrors currentFnTryFrames.
    size_t currentFnExitCleanupBase() {
        size_t i = exitCleanupStack.size();
        while (i > 0 && exitCleanupStack[i - 1].func == currentFunction) --i;
        return i;
    }

    // Names of the exception variables bound by the except handler bodies
    // currently being emitted (lexically enclosing, innermost last). Lets
    // RaiseStmt recognize `raise e` as a re-raise of the in-flight exception
    // (the bound var only holds the message string, so the type must come from
    // dragon_exc_get_type, not from `e`).
    std::vector<std::string> handlerExcVars;

    // Generator state: when compiling a generator body, this holds the gen pointer alloca
    llvm::AllocaInst* generatorPtr = nullptr;
    // Set of function names that are generators (contain yield)
    std::unordered_set<std::string> generatorFunctions;

    // D025 (post ADR-025 removal): Set of function names whose declared return
    // type is `type` (they return a class value). Callers set VarKind::Type on
    // the receiving variable. Because classes are now compile-time entities,
    // such a value's class is not known statically, so constructing through it
    // (or isinstance against it) is a compile error - there is no runtime
    // class-descriptor dispatch. Populated at FunctionDecl emission.
    std::unordered_set<std::string> funcReturnsType;

    // D025: Functions whose declared return type is `ptr` - callers can call
    // the returned value as a function pointer (the historical higher-order
    // pattern: `dbl = get_doubler(); dbl(x)`). Tracked so the indirect-call
    // fallback can fire safely without conflating with class-descriptor vars.
    std::unordered_set<std::string> funcReturnsPtr;

    // D027: Functions that return a CLOSURE value (a capturing nested def or
    // capturing lambda - a heap DragonClosure carrying an env), as opposed to a
    // bare function pointer. Both are typed `-> Callable[...]`, so the type
    // alone can't distinguish them, but the call-site dispatch differs (a
    // closure must be unpacked into fn+env). Populated at FunctionDecl emission
    // ONLY when every value-return is provably a closure, so a bare-fn-returning
    // function is never mis-marked (which would crash the closure dispatch).
    // Lets `g = make_closure(); g()` mark g VarKind::Closure (escaping-closure
    // fix). Consumed in the AnnAssign Callable paths in Assign.cpp.
    std::unordered_set<std::string> funcReturnsClosure;

    // Predicate backing funcReturnsClosure: true iff `node` is typed
    // `-> Callable[...]` and EVERY value-return is provably a closure (capturing
    // nested def / capturing lambda). Defined in Functions.cpp (uses file-local
    // AST walkers). Must be run from the forward-declaration pre-pass so it
    // precedes all body emission - class method bodies are emitted before a free
    // function's visit(FunctionDecl) runs, so populating it there alone is too
    // late for a method that calls a closure factory.
    bool functionReturnsClosure(FunctionDecl& node);

    // D025: Variable / parameter names whose declared type is `ptr`. The
    // bare-fn-pointer indirect-call fallback in CallExpr.cpp needs this
    // signal to safely emit an indirect call without conflating with
    // unannotated parameters that may carry class descriptors.
    std::unordered_set<std::string> varIsPtrCallable;

    // D024, post ADR-025: classes with user-defined (runtime) decorators.
    // Class decorators are DROPPED - a decorated class would require runtime
    // descriptor construction, which ADR-025 removed.
    // Constructing a class in this set is a compile error (CallExpr.cpp).
    // @dataclass / @staticmethod / @classmethod / @property / NamedTuple are
    // compile-time synthesis and are NOT tracked here.
    std::unordered_set<std::string> decoratedClasses;
    // Per-class decorator AST expressions (raw pointers; AST owns them).
    std::unordered_map<std::string, std::vector<Expr*>> classDecoratorExprs;

    // 6.18: @dataclass / NamedTuple synthesis tracking. classNames in this set
    // had __init__ / __eq__ / __repr__ auto-generated from field declarations.
    // dataclassFieldNames holds the ordered field names per class for use by
    // synthesized __eq__ and __repr__.
    std::unordered_set<std::string> dataclassClassNames;
    std::unordered_map<std::string, std::vector<std::string>> dataclassFieldNames;

    // Enum synthesis tracking (class-based `from enum import Enum`). A class
    // deriving Enum/IntEnum/StrEnum is rewritten by synthesizeEnumMethods into
    // a class of singleton member instances. enumKind selects equality
    // semantics: Plain -> pointer identity (default); Int/Str -> value-compare
    // emitted in Expressions.cpp. enumMemberNames holds member order (used by
    // value-lookup and iteration over the class object).
    enum class EnumKind { Plain, Int, Str };
    std::unordered_map<std::string, EnumKind> enumKind;          // className -> kind
    std::unordered_map<std::string, std::vector<std::string>> enumMemberNames;

    // Map a VarKind for a list/dict element annotation to the Type::Kind used
    // by varListElemKinds / varDictValueKinds. Mirrors the per-VarKind switch
    // in Assign.cpp for local list[T]/dict[K,V] annotations.
    static Type::Kind elemVarKindToTypeKind(VarKind ek);

    // D025: Mark `paramName` as ptr-typed if its annotation is `ptr`. Param
    // setup sites call this after typeExprToKind so the bare-fn-pointer
    // indirect-call fallback can safely fire only for ptr-annotated names.
    //
    // Also handles `Callable[[A, B], R]`: derives the LLVM FunctionType from
    // the type expression and registers it so calls go through with the
    // proper signature (no i64-default fallback).
    //
    // Also populates the list[T]/dict[K,V] element-kind tables so for-in,
    // subscript, and iteration over a parameter name dispatch at the right
    // native type - without this, `for part in parts` on a `list[str]` param
    // treats the loop var as int and prints raw addresses.
    void trackPtrParam(const std::string& paramName, TypeExpr* typeExpr);

    /// Allocate a monomorphized list matching `elemTag` (and `isAny` for the
    /// box list). Mirrors the variant selection in visit(ListExpr) so list
    /// literals and *args packing share one source of truth. Defined in
    /// Collections.cpp. (elemTag 0 + !isAny = legacy i64 DragonList.)
    llvm::Value* emitNewTypedList(int64_t elemTag, bool isAny, llvm::Value* capVal);

    /// Append one already-evaluated `val` to a list built by emitNewTypedList.
    /// `elemExpr` drives tag inference (box list) and the borrow/incref +
    /// ensureHeapString discipline. Defined in Collections.cpp.
    void emitTypedListAppend(llvm::Value* list, llvm::Value* val, Expr* elemExpr,
                             int64_t elemTag, bool isAny, CodeGen& cg);

    /// Build an llvm::FunctionType from a Callable[[A, B], R] AST node.
    /// Used by trackPtrParam (for params/locals) and by the for-loop site
    /// (for list[Callable[...]] element propagation).
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

    // D030 Phase 4: %dragon.box = type { i64 tag, i64 payload }.
    //  tag: DragonValueTag (TAG_INT=0, TAG_STR=1, TAG_FLOAT=2, ...).
    //  payload: i64-shaped storage; codegen extracts at the narrowed
    //  native type at consumption sites (no value flows as raw
    //  i64 through user code - the i64 here is purely 8-byte
    //  opaque storage backing for any 8-byte value).
    // {i64,i64} (not the doc's original {i8,i64}) is the locked-in shape:
    // sysv ABI passes it cleanly in two registers, every field is on a
    // natural-alignment boundary, and the tag word can later carry richer
    // metadata (class id, narrowing hints) without touching the layout.
    llvm::StructType* boxType = nullptr;

    // Runtime function cache
    std::unordered_map<std::string, llvm::Function*> runtimeFuncs;

    // Lambda counter for unique names
    int lambdaCounter = 0;

    // Exception handling counter for unique block naming
    int excCounter = 0;

    // Per-for-loop counter so each loop's owned iterable temp gets a UNIQUE
    // scope-cleanup name. Two container-iterating for-loops in one scope both
    // used the name "__iter"; the second setVar clobbered the first in the
    // scope map, so scope cleanup only decref'd the last and leaked the rest
    // (one keys()/items()/comprehension temp per extra loop).
    int forIterCounter = 0;

    // Map exception class name to hierarchical type code.
    // Codes are assigned so that all children of a parent are contiguous,
    // enabling range-based subtype matching (e.g., except ArithmeticError
    // catches ZeroDivisionError, OverflowError, FloatingPointError).
    int64_t excTypeCode(const std::string& name);

    // Check if a name is a built-in exception type
    bool isBuiltinExcName(const std::string& name);

    // Check if a name is any exception type (built-in or user-defined)
    bool isExcType(const std::string& name) {
        return isBuiltinExcName(name) || userExcCodes.count(name) > 0;
    }

    // User-defined exception tracking
    int64_t userExcNextCode = 1000;
    std::unordered_map<std::string, int64_t> userExcCodes;        // className -> code
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

    // Stdlib import tracking
    std::map<std::string, std::string> symbolAliases;
    std::set<std::string> importedModules;
    std::set<std::string> fileResolvedModules;

    // Class support
    // Dragon class layout: each class creates an LLVM StructType with fields
    // extracted from __init__ body (self.x = ... assignments).
    // Constructor: ClassName_new(params...) -> ptr (malloc + __init__)
    // Init: ClassName___init__(ptr self, params...) -> void
    // Methods: ClassName_methodName(ptr self, params...) -> retType
    std::set<std::string> classNames;         // Known class names for constructor dispatch
    std::string currentClassName;              // Set when emitting class methods
    std::unordered_map<std::string, llvm::StructType*> classStructTypes;
    // TypedDict: class name -> {field name -> Type::Kind}. Variables of TypedDict
    // type are VarKind::Dict at runtime but access uses checked get with known
    // tags derived via typeKindToTag - the source-of-truth tag derivation.
    // Stored as Type::Kind (not VarKind) so per-field bytes-ness survives the
    // VarKind::Bytes deletion (D030 §5).
    std::set<std::string> typedDictClasses;
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> typedDictFieldKinds;
    // Variable name -> TypedDict class name (so we know which schema to use)
    std::unordered_map<std::string, std::string> varTypedDictClass;
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classFieldIndices;
    std::unordered_map<std::string, std::unordered_map<std::string, llvm::Type*>> classFieldTypes;
    std::unordered_map<std::string, std::unordered_map<std::string, VarKind>> classFieldKinds; // Phase 5: per-field VarKind for dealloc
    // Own (non-inherited) instance-field order per class, from the AST
    // `instanceFieldOrder` helper - the SAME source the TypeChecker fills
    // ClassType::fieldOrder from. Drives positional `match` class-pattern
    // destructuring (`case Point(x, y)`); ancestors are prepended via
    // classParentNames at the match site.
    std::unordered_map<std::string, std::vector<std::string>> classFieldOrder;
    std::unordered_map<std::string, llvm::GlobalVariable*> classIdGlobals; // Phase 5: class_id globals
    // Class docstrings: populated by visit(ClassDecl) when ClassDecl.docstring is
    // present. Looked up at dragon_class_descriptor_create call time so the
    // descriptor's `doc` field carries the class docstring (powers `Cls.__doc__`).
    std::unordered_map<std::string, std::string> classDocstrings;
    // Function docstrings: keyed by mangleFunc(modName, funcName). Populated by
    // visit(FunctionDecl) when ClassDecl.docstring is present. Powers `f.__doc__`.
    std::unordered_map<std::string, std::string> functionDocstrings;
    // Cached `.rodata` i8* constants for function docstring bytes. Lazy: built
    // on first attribute-access. Unused docstrings cost zero (the constant is
    // never emitted; the entry stays in `functionDocstrings` only).
    std::unordered_map<std::string, llvm::Constant*> functionDocConstants;
    // Method docstrings: className -> methodName -> docstring. Powers
    // `MyClass.method.__doc__` and `instance.method.__doc__`. Pattern-match
    // happens in Attributes.cpp on the AttrExpr(AttrExpr(...), "__doc__")
    // chain - methods aren't first-class values in Dragon, so this is the
    // only access shape we support.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> methodDocstrings;
    // Cached `.rodata` constants for method docstrings: className -> methodName.
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::Constant*>> methodDocConstants;
    // Module docstrings: keyed by module name (entry module key is empty
    // string, matching `currentModuleName = ""`). Populated upfront in
    // generate() from each Module.docstring.
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
                                                // last-wins on classDescriptorGlobals)
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
    // dict[K, V] value Type::Kind tracking. Used by `for k, v in d.items()`
    // to set v's VarKind so subsequent uses (print, comparisons, etc.) dispatch
    // correctly. Mirrors varListElemKinds for lists.
    std::unordered_map<std::string, Type::Kind> varDictValueKinds;
    // D030 Phase 3.G: dict[K, V] key Type::Kind tracking. Codegen branches on
    // this to route subscript/`in`/print/iteration at int-keyed dicts to the
    // dragon_dict_int_* family instead of the str-keyed defaults. Default
    // (absent entry) means str-keyed, preserving the existing behaviour.
    std::unordered_map<std::string, Type::Kind> varDictKeyKinds;
    // className -> fieldName -> list element Type::Kind (for self.field list iterations)
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldListElemKinds;
    // className -> fieldName -> dict value Type::Kind (for self.field dict subscript)
    // Mirrors varDictValueKinds for class-field dicts so `obj.field["k"]` routes
    // to the typed runtime op (dragon_dict_get_str_ptr / _str_f64) at the dict's
    // native value type instead of the polymorphic i64-returning op.
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldDictValueKinds;
    // D030 Phase 3.G: class-field dict key Type::Kind. Mirrors varDictKeyKinds
    // for self.<field> dicts.
    std::unordered_map<std::string, std::unordered_map<std::string, Type::Kind>> classFieldDictKeyKinds;
    // className -> fieldName -> element class name (for list[ClassName] field iterations)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldListElemClassName;

    // Class name of a class-instance field. Populated when extractFields sees
    // `self.x = Foo(...)` or `self.x = param: Foo`. Read by
    // resolveExprClassName(AttributeExpr) so chained `obj.x.field` resolves
    // to Foo's struct layout instead of returning ConstantInt 0.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldClassName;
    // varName -> element class name (for local list[ClassName] iterations)
    std::unordered_map<std::string, std::string> varListElemClassName;
    // Callable[[...], R] element typing for list iterations:
    //  varName -> element FunctionType (local list[Callable[...]])
    //  className.F -> element FunctionType (self.field: list[Callable[...]])
    // Loop var of `for f in xs` picks up callableTypes[f] from these maps so the
    // callsite has a known signature without falling back to i64-default.
    std::unordered_map<std::string, llvm::FunctionType*> varListElemCallableType;
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::FunctionType*>>
            classFieldListElemCallableType;
    // Direct Callable[[A,B,...], R] field types - `class C { handler:
    // Callable[[Req,Res,Ctx], None] }`. Recorded so the call site
    // `obj.handler(args)` can build the real FunctionType (param types
    // and return type) instead of a synthetic all-i64 signature, and so
    // it can append the trailing env arg when the field at runtime holds
    // a DragonClosure rather than a bare fn pointer.
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::FunctionType*>>
            classFieldCallableType;
    std::unordered_map<std::string, std::string> classParentNames; // className -> parentClassName
    // className -> (field, per-instance default-expr). Persisted from the layout
    // pre-pass (which visits every class in source order, before any _new body is
    // emitted) so emitNewBody can walk the parent chain and apply inherited
    // defaults regardless of source order. Expr* is borrowed from the ClassDecl
    // body (owned by the AST) and stays valid for the CodeGen instance's lifetime.
    std::unordered_map<std::string, std::vector<std::pair<std::string, Expr*>>> classPerInstanceDefaults;
    std::unordered_map<std::string, std::string> methodReturnClassNames; // "Class_method" -> returnClassName
    std::unordered_map<std::string, std::string> funcReturnClassNames;   // top-level funcName -> returnClassName
    // "Class_method" -> declared return Type::Kind. Needed alongside the LLVM
    // function's return type because `ptr` is overloaded (str / list / dict /
    // bytes / instance all lower to ptr) - the AST kind disambiguates so
    // downstream call sites can pick the right VarKind for the bound value.
    // Drives `for x in iter` binding `x` with the correct VarKind so method
    // dispatch on `x` (e.g. x.strip() when __next__() -> str) reaches the
    // right runtime path.
    std::unordered_map<std::string, Type::Kind> methodReturnKinds;       // "Class_method" -> Type::Kind

    // Decision 025: First-class class descriptors
    std::unordered_map<std::string, llvm::GlobalVariable*> classDescriptorGlobals; // className -> @ClassName__descriptor
    bool resolvingCallTarget = false; // true when visiting callee of a CallExpr (suppresses descriptor load)

    // Decision 026: Vtable support
    // className -> methodName -> vtable index (0-based)
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classMethodVtableIndices;
    // className -> ordered list of method names in vtable order
    std::unordered_map<std::string, std::vector<std::string>> classVtableMethodOrder;
    // className -> llvm::GlobalVariable* for @ClassName__vtable
    std::unordered_map<std::string, llvm::GlobalVariable*> classVtables;

    // D033: Method-name reflection. Each class's OWN (non-inherited) method
    // names, in declaration order, and parallel kind bytes
    // (0 = instance, 1 = static, 2 = classmethod). Populated alongside the
    // vtable order in ImplInit's class-body scan; consumed by CodeGen
    // main-init to emit @ClassName__method_names / __method_fn_ptrs /
    // __method_kinds globals and wire them via dragon_class_descriptor_set_methods.
    std::unordered_map<std::string, std::vector<std::string>> classOwnMethods;
    std::unordered_map<std::string, std::unordered_map<std::string, uint8_t>> classMethodKinds;
    // D033 Phase 3: per-(class, method) bound-thunk function. Codegen emits
    // one alongside the method body (signature = user args minus self + env).
    // Indexed by className -> methodName -> thunk fn. NULL entries are valid
    // for static methods (which skip the bind path in dragon_getattr).
    std::unordered_map<std::string,
                       std::unordered_map<std::string, llvm::Function*>> classMethodBoundThunks;

    // 4.1 @property: per-class set of property names whose getter is the same name.
    // Populated in ImplInit when scanning class bodies for FunctionDecl with isProperty=true.
    std::unordered_map<std::string, std::unordered_set<std::string>> classProperties;
    // className -> propertyName -> mangled setter func name ("<propName>__setter")
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classPropertySetters;

    /// Mangle user function names that collide with codegen-reserved symbols.
    /// "main" collides with the C entry point that codegen owns; user
    /// `def main()` becomes the LLVM symbol `_dragon_user_main`. All call
    /// sites that resolve a user function by name route through this helper
    /// so the rename is invisible at the Dragon source level.
    static std::string userFuncName(const std::string& name) {
        if (name == "main") return "_dragon_user_main";
        return name;
    }

    // Per-module symbol mangling for top-level function names. Dragon
    // links all imports into a single LLVM module, which
    // means every module's `def open` would land on the same `@open`
    // symbol - `gzip.open`, `zstandard.open`, and `tarfile.open` together
    // would collapse onto one body. Mangling by module path gives each
    // module its own namespace at the LLVM symbol level while keeping the
    // user-visible name unchanged at the Dragon source level.
    //
    // `_dragon_user_main` stays unique across the program (only the entry
    // module is allowed to define `main`). Modules with no name (the entry
    // file's body before imports are resolved) keep the bare name so the
    // entry program's `def helper(...)` call remains `@helper`.
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

    // The module currently being lowered. Set/restored in CodeGen::generate
    // for each dependency before forwardDeclare / visit fires, then again
    // for the entry module. Read by mangleFunc call sites that need to
    // refer to a same-module symbol (where there's no AttributeExpr base
    // pointing at the owning module).
    std::string currentModuleName;

    // Per-importing-module alias scope: importingModule -> (bareName -> mangled).
    // `from os import listdir` in module A binds A's `listdir` to
    // `os__listdir`; module B doing `from socket import listdir as ld`
    // never sees A's binding. The keys are the IMPORTING module names so
    // same-module reads consult the alias under currentModuleName; the
    // entry program (currentModuleName == "") gets its own bucket.
    //
    // Without this scoping, a single global map silently clobbered when
    // two modules imported the same bare name from different sources.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedFuncAliasesByModule;

    // Resolve an alias under the current module's scope. Returns the
    // mangled symbol name, or empty string when no alias is in effect.
    // Readers (CallExpr / Expressions / Assign function-as-value) all
    // funnel through this so the lookup stays consistent.
    std::string lookupImportedAlias(const std::string& bareName) const {
        auto modIt = importedFuncAliasesByModule.find(currentModuleName);
        if (modIt == importedFuncAliasesByModule.end()) return "";
        auto nameIt = modIt->second.find(bareName);
        if (nameIt == modIt->second.end()) return "";
        return nameIt->second;
    }

    // Resolve a bare callee name to the LLVM symbol it should look up:
    // alias -> mangleFunc(currentModule, name) -> userFuncName(name). Used
    // both for `module->getFunction(...)` lookups AND for indexing the
    // side-channel maps (funcVarArgInfo, funcParamKinds, funcParamDefaults,
    // funcReturnClassNames, generatorFunctions) which are now keyed by
    // the mangled symbol so two stdlib modules with same-named functions
    // (gzip.open vs tarfile.open) don't clobber each other's signatures
    // and defaults. Picks the FIRST candidate that exists in the module
    // so extern-C and entry-module bare names still resolve.
    std::string resolveCalleeSymbol(const std::string& name) const {
        std::string aliasSym = lookupImportedAlias(name);
        if (!aliasSym.empty()) {
            if (module && module->getFunction(aliasSym)) return aliasSym;
        }
        std::string mangled = mangleFunc(currentModuleName, name);
        if (module && module->getFunction(mangled)) return mangled;
        return userFuncName(name);
    }

    // Per-module mangling for class symbols. Same shape as mangleFunc - joins
    // module path and class name with `__`, replacing dots in the module path
    // with underscores. Used as the prefix for ALL class-related LLVM
    // symbols: `%<className>` struct type, `<cls>_new` / `<cls>___init__` /
    // `<cls>_<method>` functions, `<cls>__vtable` / `<cls>__descriptor` /
    // `__class_id_<cls>` globals, and the `__dragon_dealloc_<cls>` /
    // `_traverse_` / `_clear_` / `_mark_shared_` per-class helpers.
    //
    // Modules with no name (the entry file) leave the bare name unchanged so
    // entry-program classes keep their `%Foo` etc. symbols and same-named
    // dep classes get a module-prefixed namespace at the LLVM symbol level.
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

    // Bare class name -> owning module. Populated in forwardDeclareClasses for
    // every dependency module and the entry. With duplicate class names this
    // is last-write-wins; resolveClassOwningModule below prefers the
    // current-module probe over this map so same-module callers always win
    // before falling through to the global owner.
    std::unordered_map<std::string, std::string> classOwningModule;

    // Per-importing-module class alias scope: importingMod -> (bareName ->
    // owningMod). `from b import Conflict` in module A pins A's `Conflict`
    // to module b regardless of any same-named class elsewhere. Mirrors
    // importedFuncAliasesByModule in shape.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> importedClassAliasesByModule;

    std::string lookupImportedClassAlias(const std::string& bareName) const {
        auto modIt = importedClassAliasesByModule.find(currentModuleName);
        if (modIt == importedClassAliasesByModule.end()) return "";
        auto nameIt = modIt->second.find(bareName);
        if (nameIt == modIt->second.end()) return "";
        return nameIt->second;
    }

    // Resolve which module a bare class name belongs to from the current
    // module's perspective. Order:
    //  1. Imported alias (`from b import Conflict` in current module).
    //  2. Same-module probe: if `<currentModule>__<className>_new` exists
    //  in the LLVM module, the current module defines the class.
    //  3. Global owning-module map (last-wins fallback for duplicate names).
    //  4. Current module name (for entry-module direct definitions).
    std::string resolveClassOwningModule(const std::string& bareName) const {
        std::string aliasMod = lookupImportedClassAlias(bareName);
        if (!aliasMod.empty()) return aliasMod;
        if (module) {
            // Same-module probe - robust to last-write-wins on classOwningModule.
            std::string mangled = mangleClass(currentModuleName, bareName);
            if (module->getFunction(mangled + "_new") ||
                module->getFunction(mangled + "_new_0") ||
                module->getFunction(mangled + "___init__") ||
                module->getFunction(mangled + "___init___0")) {
                return currentModuleName;
            }
        }
        auto cmIt = classOwningModule.find(bareName);
        if (cmIt != classOwningModule.end()) return cmIt->second;
        return currentModuleName;
    }

    // Return the LLVM symbol prefix for a bare class name, resolved from
    // the current module's perspective. Use this for every emitter that
    // needs to reach a class-owned LLVM function or global.
    std::string classSymPrefix(const std::string& bareName) const {
        return mangleClass(resolveClassOwningModule(bareName), bareName);
    }

    // D026 devirtualization gate: does any STRICT subclass of `baseClass`
    // directly define (override) `method`? If not, a call on a `baseClass`-
    // typed receiver can be devirtualized to a direct call (C-speed) - the
    // runtime object's method is provably the statically-resolved one. If so,
    // the receiver may be a subclass and the call must dispatch through the
    // object's vtable. Dragon compiles whole-program (all modules in one LLVM
    // module), so this analysis is exact: a subclass only has a
    // `mangleClass(mod, sub) + "_" + method` symbol when it overrides - inherited
    // methods are never re-emitted under the subclass name (resolveMethodFunction
    // walks the MRO precisely because of that).
    bool methodIsOverridden(const std::string& baseClass,
                            const std::string& method) const;

    // Walk the inheritance chain starting at (owningModule, className),
    // trying each level for a method symbol of the form
    // `mangleClass(mod, cls) + "_" + methodName`. Returns the first
    // matching llvm::Function, or nullptr if no level defines the method.
    // On match, *resolvedSymbol (if non-null) is set to the mangled symbol
    // so callers can probe parallel maps (staticMethods, classCtorArities,
    // etc.) without re-mangling.
    //
    // Each parent's owning module is looked up in classOwningModule and
    // falls back to the caller-supplied owningModule if not recorded.
    //
    // This is the SINGLE lookup point: CallMethods, Classes, and Concurrency
    // codegen all resolve through it. Per-caller copies drift (e.g. bare
    // classNames pre-mangling) and a drifted copy miscompiles cross-module
    // dispatch like `fire self._method()`.
    llvm::Function* resolveMethodFunction(
        const std::string& owningModule,
        const std::string& className,
        const std::string& methodName,
        std::string* resolvedSymbol = nullptr) const;

    // Per-instance owning module: var -> owning module of the class instance
    // stored in that var. Mirrors varClassNames so method dispatch (and
    // anything else that needs to reach `<owner>__<className>_<method>`)
    // can pick the right LLVM symbol when two modules define same-named
    // classes. Populated alongside varClassNames at every assignment site.
    std::unordered_map<std::string, std::string> varClassOwningModule;


    // Owning module per class field whose declared type is another class
    // (see classFieldClassName). Same purpose as varClassOwningModule but
    // for `self.x: Foo` / `self.x = Foo(...)` reads.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> classFieldOwningModule;

    /// 6.12(B) Non-negative variable tracking. A variable name is in this
    /// set when its current value is provably ≥ 0 - assigned from an int
    /// literal ≥ 0, a `len()` / `abs()` call, or a `+`/`*`/`**` of two
    /// non-negative operands. Used by the inline list-subscript paths in
    /// Attributes.cpp / Assign.cpp to skip the `idx + (idx<0 ? size : 0)`
    /// correction (3 instructions per access). Conservatively cleared on any
    /// assignment whose RHS is not provably non-negative.
    std::unordered_set<std::string> knownNonNeg;

    /// Returns true when `e` is provably ≥ 0 from its AST shape. Recursive,
    /// purely structural - no flow-sensitive reasoning beyond the tracked
    /// `knownNonNeg` set.
    bool isExprDefinitelyNonNeg(Expr* e) const;

    // 4.7 PEP 393-lite: non-ASCII string literals.
    // Each unique UTF-8 byte sequence gets a module-level i8* global, lazily
    // initialized at the top of main() via dragon_str_intern (one-shot
    // alloc + immortal). Use sites just emit a load - zero per-access cost.
    std::unordered_map<std::string, llvm::GlobalVariable*> utf8LiteralGlobals;
    std::vector<std::string> utf8LiteralOrder;

    // ASCII string literals: emitted as IMMORTAL DragonString CONSTANTS (real
    // DragonObjectHeader + len/kind/cap + NUL-terminated bytes) in the binary,
    // deduped by byte sequence. A pointer to the `data` field is returned, so
    // dragon_is_heap_string / _len / _eq / decref read an in-bounds header
    // (no OOB read off a bare C global), and the immortal refcount makes
    // incref/decref no-ops. Zero per-access and zero startup cost (writable
    // .data, like interned non-ASCII immortals, so immortal-flag writes never fault).
    std::unordered_map<std::string, llvm::GlobalVariable*> asciiLiteralGlobals;

    // D017 Phase 4.B - template content-type context stack. Pushed on
    // entry to `visit(TemplateExpr)` / `visit(TemplateFileExpr)`, popped on
    // exit. Used so `:{ ... }` content fragments inside a `!{}` block
    // inherit their parent template's content type for auto-escape and
    // instance wrapping. Empty stack = no enclosing template (top-level
    // `template { ... }` stays untyped str).
    std::vector<std::string> templateContextStack;

    // D032 - interned canonical `$$N` query texts for `template[SQL]` sites.
    // One ASCII global per unique canonical, so structurally identical sites
    // share a pointer and a driver's prepared-statement cache can hit by
    // pointer-compare. Non-ASCII canonicals (rare) fall back to the UTF-8 path
    // and aren't interned (content+hash keying still works).
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

    // 64-bit FNV-1a over `s`'s bytes. MUST match dragon_str_fnv1a in
    // runtime_sqltemplate.cpp so the compiler's folded constant and any
    // runtime-built canonical with the same text share a cache bucket.
    uint64_t sqlCanonicalHash(const std::string& s) const {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) { h ^= (uint64_t)c; h *= 0x100000001b3ULL; }
        return h;
    }

    // D017 Phase 4.B - template block-interp buffer stack. Each `!{...}`
    // that falls into block mode (parseExpression failed -> parseBlock)
    // allocates a runtime DragonListPtr* (list[str]) and pushes the alloca
    // here. `:{}` ExprStmts inside the block append their rendered string
    // to the top buffer via dragon_list_append_ptr. After the block runs,
    // CodeGen pops the buffer and calls dragon_str_join_ptr to flatten -
    // that joined string is the !{}'s value.
    std::vector<llvm::Value*> templateBlockBufferStack;

    // Static member support
    // staticFieldGlobals[className][fieldName] -> LLVM GlobalVariable for that static field
    std::unordered_map<std::string, std::unordered_map<std::string, llvm::GlobalVariable*>> staticFieldGlobals;
    // staticMethods tracks "ClassName_methodName" entries that are static (no self param)
    std::unordered_set<std::string> staticMethods;

    // Multi-constructor support: className -> number of __init__ overloads
    std::unordered_map<std::string, size_t> classCtorCount;
    // className -> vector of (arity, constructorIndex) pairs for dispatch
    std::unordered_map<std::string, std::vector<std::pair<size_t, int>>> classCtorArities;

    // Module-level globals: variables declared at top level become LLVM GlobalVariables
    // so functions can access them without needing Python-style `global` declarations (.dr mode)
    std::unordered_map<std::string, llvm::GlobalVariable*> moduleGlobals;
    std::unordered_map<std::string, VarKind> moduleGlobalKinds;
    // Entry-module globals that were forward-declared (so entry-module method
    // bodies can resolve them) but not yet initialized. Their first
    // module-level assignment is a definition, not an overwrite, so it must
    // not decref the null initializer. Erased on first init; genuine
    // reassignments thereafter take the normal RC-overwrite path.
    std::unordered_set<std::string> entryGlobalsAwaitingInit;
    llvm::Function* mainFunction = nullptr;  // pointer to main() for detecting module level
    // Scope-stack depth at which the entry module's top-level body executes.
    // A declaration in main() is only a module global when at this depth; any
    // deeper means it sits inside a block scope and must be a block-local.
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
    // docs/002 2.8: aligned with funcParamKinds (methods include self at 0) -
    // true for `own p: T` params. The CALLER must not drain an owned temp
    // bound to an own param: ownership TRANSFERRED, the callee's scope exit
    // releases it (caller-drain + callee-release double-freed, A/B-proven by
    // the fresh-temp-exemption probe).
    std::unordered_map<std::string, std::vector<bool>> funcParamOwns;
    bool paramIsOwn(const std::string& funcName, unsigned idx) {
        auto it = funcParamOwns.find(funcName);
        return it != funcParamOwns.end() && idx < it->second.size() &&
               it->second[idx];
    }
    // D027: per-function flags - param i is a `Callable[...]`. When a BARE
    // function is passed to such a param, the call site wraps it as
    // DragonClosure(fn, null) so the param always holds a real DragonClosure
    // (reliable closure dispatch - no .text tag guess, no crash). Keyed by the
    // LLVM symbol, like funcParamKinds; the wrapped closure is freed after the
    // call via argTemps (VarKind::Closure decref). Indexed by AST param position
    // (== arg index for the free-function direct-call path that consumes it).
    std::unordered_map<std::string, std::vector<bool>> funcCallableParam;

    // `extern "C"` function names (keyed like funcParamKinds). Extern callees do
    // NOT follow Dragon's borrow-and-incref RC convention - their args may be
    // borrowed interior/non-owned pointers (e.g. dragon_bytes_data -> a raw
    // buffer ptr) - so the call site must never release owned-temp arguments
    // passed to them.
    std::unordered_set<std::string> externFuncNames;
    // FFI v0 ownership contract: extern "C" args are
    // BORROWED for the duration of the call (an extern must not retain a
    // Dragon reference past return - an adopting extern would already corrupt
    // named-local args, which are never increfed for externs), and a managed
    // return (str/bytes/list/dict/set) is a FRESH +1 allocation, never an
    // alias of an argument. Under that contract an owned heap temp passed to
    // a managed-typed extern param drains after the call exactly like any
    // borrow callee (stdlib http's nested dragon_str_concat calls leaked one
    // string per header per response without this). A declared `ptr` RETURN
    // opts the whole call out: the result may point INTO an argument
    // (dragon_bytes_data), so its arg temps must outlive the call site
    // (leak-over-UAF at the FFI edge). Members: externs whose declared return
    // is not `ptr` - the ones whose owned arg temps are safe to drain.
    std::unordered_set<std::string> externDrainableFuncs;

    // Default parameter values: funcName -> vector of Expr* (one per LLVM param,
    // nullptr for params without defaults). Used at call sites to fill missing args.
    std::unordered_map<std::string, std::vector<Expr*>> funcParamDefaults;

    // Defining module name per LLVM function symbol. Recorded alongside
    // funcParamDefaults at registration time. fillDefaultArgs swaps
    // `currentModuleName` to this value while evaluating each default's AST
    // so NameExpr / CallExpr lookups against module-private symbols
    // (functions, classes, aliases) resolve in the defining module's scope
    // instead of the call site's. Without it, a default like
    // `Cls.method = _helper` in module M is unreachable from a call site in
    // module N (M is private; N has no alias for _helper).
    std::unordered_map<std::string, std::string> funcDefiningModule;

    // D040: Declared parameter names (one per LLVM param), populated alongside
    // funcParamDefaults at every emission site. The non-vararg call path in
    // CallExpr.cpp uses this to bind call-site keyword arguments to their
    // matching parameter positions before fillDefaultArgs fills the remainder.
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames;

    // Union type support: member VarKinds per union-typed variable.
    // Used by isinstance narrowing to compute the "else" type for 2-member
    // unions and to validate `isinstance(x, T)` against declared members.
    std::unordered_map<std::string, std::vector<VarKind>> unionMemberKinds;
    // (D030 Phase 4: unionTagAllocas and funcUnionTagMask deleted -
    //  tag is now structural in the {i64, i64} box value.)

    // First-class function support: track the LLVM FunctionType of callable variables.
    // Populated when a lambda is assigned to a variable, or when a named function
    // reference is assigned (e.g., fn = double). Used in visit(CallExpr) to emit
    // indirect calls when module->getFunction(name) fails.
    std::unordered_map<std::string, llvm::FunctionType*> callableTypes;

    // Nested `def` aliases: the LLVM function of a nested def is mangled
    // (e.g. `__dragon_nested_3__inner`) so it can't collide across siblings,
    // but the user source refers to it by the bare user name. While emitting
    // the nested def's own body - where the enclosing scope has been replaced
    // and its locals are no longer visible - direct calls to `inner(...)`
    // resolve through this map so self-recursion compiles to a direct LLVM
    // call (with the env arg auto-appended for capturing variants).
    struct NestedAliasInfo {
        llvm::Function* fn;            // mangled LLVM function (params + optional trailing env)
        llvm::FunctionType* userFnType;// user-visible signature (no trailing env)
        llvm::Value* envValue;         // null for non-capturing; else the body's __env arg
    };
    std::unordered_map<std::string, NestedAliasInfo> nestedFunctionAliases;

    // D027: After LambdaExpr codegen, if non-null, the last value was a closure.
    // Holds the user-facing function type (without trailing env param).
    // Assignment path checks this to set VarKind::Closure and callableTypes.
    llvm::FunctionType* lastClosureCallableType = nullptr;

    // D025 Phase 4: set to true when type() returns a class descriptor (i64).
    // Assignment path checks this to set VarKind::Type.
    bool lastValueIsType = false;

    // D024: Functions that have been wrapped by user-defined decorators.
    // Maps original function name -> module global holding the decorated callable.
    // Call dispatch checks this before direct function calls.
    std::unordered_map<std::string, llvm::GlobalVariable*> decoratedFunctions;

    // Pre-register a decorated top-level function's indirect-dispatch global and
    // callable type BEFORE class method bodies are emitted. Class bodies lower
    // in an early pass (CodeGen.cpp), before visit(FunctionDecl) runs the
    // decorator-application block - so a method that calls a decorated free
    // function would otherwise miss the decoratedFunctions entry and bind the
    // UNdecorated original. Mirrors the class-layout / module-global pre-passes.
    // Idempotent; visit(FunctionDecl) reuses the global this creates.
    void preregisterDecoratedFunction(FunctionDecl& node);

    // *args/**kwargs tracking: function name -> vararg info
    struct VarArgInfo {
        size_t numRegularParams = 0; // params before *args
        bool hasVarArg = false;
        bool hasKwArg = false;
        std::string varArgName;      // name of *args param
        std::string kwArgName;       // name of **kwargs param
        // Element representation for *args, derived from the declared element
        // annotation (`*args: T` -> T). Lets the call site pack into the
        // monomorphized list variant instead of the type-erasing i64 path.
        //  tag 0 + !isAny -> legacy DragonList (int/bool/bare *args, unchanged)
        //  tag 2 -> DragonListF64 (*args: float)
        //  tag 1/5/6/7 -> DragonListPtr (*args: str/list/dict/bytes/...)
        //  isAny -> DragonListBox (*args: Any or a union element)
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

    // mbedTLS: set to true when the dragon_tls_* TLS shim OR the mbedTLS-backed
    // crypto digests/HMAC (dragon_sha*/dragon_md5*/dragon_hmac, ADR 038 Phase 7)
    // are referenced - both pull mbedtls_* symbols from libdragon_mbedtls.a.
    bool needsMbedtls = false;

    // System libz / libzstd: set when dragon_zlib_* / dragon_zstd_* extern
    // declarations show up. Used by linkExecutable to decide whether to
    // pass -lz / -lzstd. Programs that never touch compression don't pay.
    bool needsZ = false;
    bool needsZstd = false;

    // Dunder method tracking: className -> set of dunder names (e.g. "__str__", "__eq__")
    std::unordered_map<std::string, std::set<std::string>> classDunderMethods;

    // Resolve the class name of an expression (for dunder dispatch and field
    // access). Returns "" if the expression is not a known class instance.
    std::string resolveExprClassName(Expr* expr);

    /// Does this module level annotated declaration bind a DEQUE? True when
    /// the annotation names deque (`X: deque[T]`) or the RHS is a deque(...)
    /// ctor call.
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

    /// Is `e` a receiver expression denoting the intrinsic Lock? Covers a
    /// tagged local/global (`lock.acquire()`, `with glock`) via varClassNames,
    /// AND a Lock-typed instance field (`self._lock.acquire()`,
    /// `with app._storage_lock`) via classFieldClassName - the NameExpr-only
    /// check silently missed fields: the acquire/release/with lowering fell
    /// through to generic paths that DROPPED the calls, so the "lock" never
    /// locked (found by the concurrent-mutation detector on
    /// Router._storage_lock). Defined in codegen/ImplMethods.cpp.
    bool isLockExpr(Expr* e);

    // Resolve the VarKind of an arbitrary expression. Used by print() and
    // other dispatch sites that need to know if a non-NameExpr argument is
    // a string / bool / float / etc. - without this, a subscript like
    // `obj.names[0]` would fall through to default-int print.
    // Returns VarKind::Other if unknown.
    VarKind resolveExprVarKind(Expr* expr);

    // Check if a class has a specific dunder method (walks inheritance chain)
    bool hasDunder(const std::string& className, const std::string& dunder) {
        std::string cls = className;
        while (!cls.empty()) {
            auto it = classDunderMethods.find(cls);
            if (it != classDunderMethods.end() && it->second.count(dunder))
                return true;
            auto pit = classParentNames.find(cls);
            cls = (pit != classParentNames.end()) ? pit->second : "";
        }
        return false;
    }

    // Find the class that actually defines the dunder (for MRO)
    std::string findDunderClass(const std::string& className, const std::string& dunder) {
        std::string cls = className;
        while (!cls.empty()) {
            auto it = classDunderMethods.find(cls);
            if (it != classDunderMethods.end() && it->second.count(dunder))
                return cls;
            auto pit = classParentNames.find(cls);
            cls = (pit != classParentNames.end()) ? pit->second : "";
        }
        return "";
    }

    // Call a dunder method on a class instance. Returns the result or nullptr if not found.
    llvm::Value* callDunder(const std::string& className, const std::string& dunder,
                            llvm::Value* self, const std::vector<llvm::Value*>& extraArgs = {});

    // Convert a value to i1 (boolean) for use in conditions.
    // For class instances, calls __bool__ if available (defaults to true).
    // exprNode is optional - used to resolve class name for dunder dispatch.
    llvm::Value* toBool(llvm::Value* val, Expr* exprNode = nullptr);

    void init(); // defined in codegen/ImplInit.cpp

    void pushScope() { scopes.push_back({}); }
    void popScope() { if (!scopes.empty()) scopes.pop_back(); }

    /// Return true if a VarKind represents a heap-allocated type
    /// that has a DragonObjectHeader and can be safely decref'd.
    /// Phase 2: Str (dynamic) is included. Uses dragon_decref_str which
    /// navigates from the data pointer back to the DragonString header.
    /// StrLiteral is NOT included - literals have no header.
    static bool isHeapKind(VarKind k) {
        return k == VarKind::Str || k == VarKind::List || k == VarKind::Dict ||
               k == VarKind::Tuple || k == VarKind::Set ||
               k == VarKind::File || k == VarKind::ClassInstance || k == VarKind::Generator ||
               k == VarKind::Deque ||
               k == VarKind::Closure ||  // D027: closure wrapper is refcounted
               k == VarKind::Union;  // conservative: union may hold heap types
    }

    // a closure env capture that can be a NODE in a reference
    // cycle - a heap object the env holds a +1 to AND that can transitively
    // point back at the env (instance/list -> closure -> env -> ...). Strings
    // are heap LEAVES (no children, never gc_tracked) and unions are boxed (not
    // a single tracked ptr), so neither makes the env cyclic; cells + the
    // pointer-shaped heap kinds do. Drives BOTH the env's gc-track gate (only a
    // cyclic env joins gc_tracked - #1: scalar/str-only envs pay no GC cost) AND
    // which captures the env gc_fn's TRAVERSE op visits (must match exactly, so
    // trial-deletion subtracts each internal ref once and no more).
    static bool envCaptureIsCyclic(VarKind kind, bool isCellRelay) {
        if (isCellRelay) return true;
        return kind == VarKind::List || kind == VarKind::Dict ||
               kind == VarKind::Tuple || kind == VarKind::Set ||
               kind == VarKind::ClassInstance || kind == VarKind::Generator ||
               kind == VarKind::Deque || kind == VarKind::Closure;
    }

    // Per-capture descriptor for the shared env-GC-hook emitter. One per env
    // field (field i+1 of the env struct); `kind`/`isCellRelay` mirror the
    // closure-site CaptureInfo.
    struct EnvCaptureDesc { VarKind kind; bool isCellRelay; };

    // emit the per-closure-site MULTI-OP env GC hook (DEALLOC /
    // TRAVERSE / CLEAR over the env's heap captures). Replaces the old
    // single-purpose dealloc fn; both closure sites (LambdaExpr + nested def)
    // route through this one emitter so they cannot drift. Returns the fn;
    // the caller bitcasts it into dragon_env_alloc's gc_fn slot.
    llvm::Function* emitEnvGcFn(const std::string& baseName,
                                llvm::StructType* envStructType,
                                const std::vector<EnvCaptureDesc>& caps);

    /// True if `v` is an owned (+1 refcount, heap) intermediate string that a
    /// consuming op (concat, repeat, ...) must decref or it leaks. By the
    /// ownership convention, ALL dragon_* i8*-returning
    /// functions return owned heap strings EXCEPT the borrowed-returners
    /// listed here, so this uses a blocklist. Non-CallInst values (GlobalString
    /// literals, alloca loads of named vars) are not owned intermediates.
    bool isOwnedStrResult(llvm::Value* v);

    /// True when a NAMED callee returns a BORROWED string (TLS slots,
    /// container element reads, foreign C pointers) that must never be
    /// decref'd by the consumer. Shared by isOwnedStrResult and the
    /// mixed-shape comparison drain in Expressions.cpp.
    bool isBorrowedStrReturnerName(const std::string& name);

    /// Generic-pointer analog of isOwnedStrResult: true if `v` is an owned
    /// (+1, fresh) heap object pointer - list/dict/set/tuple/instance/bytes -
    /// that a consumer must release or take ownership of. Same blocklist
    /// shape: every ptr-returning call yields an owned result EXCEPT the
    /// container element reads below, which return a BORROW (the container
    /// keeps the +1). Non-CallInst values (alloca loads of locals, IntToPtr
    /// of i64 payloads, GEP field loads) are conservatively borrows.
    bool isOwnedPtrResult(llvm::Value* v);

    /// True if `v` is an OWNED box temporary (a `{tag,payload}` value whose
    /// refcounted payload carries a +1 the consumer must release / take). The
    /// box equivalent of isOwnedStrResult, and a BLOCKLIST for the same reason:
    /// every box-returning call yields an owned result EXCEPT the container
    /// element-read helpers below, which return a BORROW (the container keeps
    /// the +1). User functions returning Any are owned too - the ReturnStmt path
    /// increfs a borrowed box payload before returning, so a box flowing out of
    /// a call always owns its payload. A non-CallInst (a box loaded from a slot)
    /// or an indirect call (no known callee) is conservatively a borrow.
    /// Used to free box temporaries after a transient use (print / discarded
    /// statement) and to take ownership on store instead of double-counting.
    bool isOwnedBoxResult(llvm::Value* v);

    /// D030 §5: Type::Kind -> DragonValueTag - the source-of-truth tag
    /// derivation. Replaces varKindToTag at every site that has access to
    /// the source expression's static type. Critical for Bytes-typed
    /// values stored in box/union/typedDict slots: the runtime tag must
    /// reflect the actual element type even when the slot's VarKind has
    /// been collapsed into a generic-heap kind.
    static int64_t typeKindToTag(Type::Kind k);

    /// Map VarKind to DragonValueTag. Returns -1 if no specific tag (e.g. Other/Any).
    static int64_t varKindToTag(VarKind vk);

    /// When >= 0, the next dict subscript/dot-access emits dragon_dict_get_checked
    /// with this tag, then resets to -1. Set by AnnAssignStmt when RHS is dict access.
    int64_t pendingDictCheckTag = -1;

    /// Companion to pendingDictCheckTag for LIST-annotated LHS: the
    /// dragon_list_view_check argument (see listViewWantElemTag). The dict
    /// checked-get verifies the stored TAG is "list" (5) but cannot tell a
    /// monomorphized DragonList from a DragonListBox - the consuming get site
    /// emits the view check on the returned pointer so `xs: list[int] =
    /// anyDict["k"]` raises TypeError instead of misreading a list[str]'s
    /// pointers as ints. Captured and cleared together with
    /// pendingDictCheckTag at the get sites.
    int64_t pendingListViewElemTag = kNoListElemCheck;

    /// D030 Phase 3.G: resolve the static key Type::Kind of a dict that
    /// `expr` evaluates to. Used by subscript/`in`/print sites to branch
    /// between str-keyed (default) and int-keyed dispatch. Returns
    /// Type::Kind::Unknown when no annotation reached this site.
    Type::Kind resolveDictKeyKind(Expr* expr);

    /// True iff `expr` denotes a dict[int, V].
    bool dictKeyIsInt(Expr* expr) {
        return resolveDictKeyKind(expr) == Type::Kind::Int;
    }

    /// Resolve the VALUE kind of a dict expression (the V in dict[K, V]).
    /// Mirrors resolveDictKeyKind but reads the value-kind maps. Used to pick
    /// the typed augmented-assignment path for `d[k] OP= v`.
    Type::Kind resolveDictValueKind(Expr* expr);

    /// Phase 5: Map a Type::Kind to a DragonValueTag integer for container elem_tag.
    /// Returns 0 (TAG_INT) for non-heap types we don't pack specially.
    /// 6.12: TAG_BOOL = 3 unlocks 1-byte packed storage in the runtime -
    /// `list[bool]` of 1M elements drops from 8MB to 1MB and fits in L2.
    static int64_t typeKindToElemTag(dragon::Type::Kind k);

    /// If `e` is a container expression (list/dict/set/tuple), return the
    /// runtime function that renders it to a DragonString; otherwise "".
    /// Used by str() and f-string interpolation so a container is rendered as
    /// its repr (e.g. "[1, 2, 3]") instead of being misread as a string
    /// pointer (which produced empty output). Sets are typed as ListType, so we
    /// disambiguate list vs set via VarKind / AST node before the type kind.
    std::string containerReprFn(Expr* e);

    /// Phase 5: Get elem_tag for a list expression from its resolved type.
    int64_t getListElemTag(Expr* listExpr) {
        if (listExpr && listExpr->type) {
            if (auto* lt = dynamic_cast<ListType*>(listExpr->type.get())) {
                if (lt->elementType) return typeKindToElemTag(lt->elementType->kind());
            }
        }
        return 0; // TAG_INT (unknown)
    }

    /// Map a Type::Kind to the matching VarKind. Used when binding a
    /// comprehension/for-loop variable to the element type derived from the
    /// iterable. Returns Int for primitives we don't track as heap kinds.
    static VarKind typeKindToVarKind(Type::Kind k);

    /// D030 §5: native-LLVM-type derivation from Type::Kind. The single
    /// source of truth for "what shape does a value of this type have at
    /// the LLVM ABI level". Replaces ad-hoc Type::Kind -> VarKind translation
    /// switches scattered across codegen call sites - drives loop-var
    /// allocas, list-get return shapes, dict-value bindings, and any other
    /// place that needs to size a slot from the static type.
    llvm::Type* typeKindToLLVM(Type::Kind k) const;

    /// Type::Kind-based heap classification. Replaces `isHeapKind(VarKind)`
    /// at refcount-on-iteration sites so the heap test is driven from the
    /// static type, not the source-level VarKind hop. Mirrors `isHeapKind`
    /// minus the Other/Union/File branches that VarKind tracked for
    /// non-Type-shaped slots.
    static bool isHeapTypeKind(Type::Kind k);

    /// Determine the element Type::Kind of an iterable expression. Looks at
    /// `varListElemKinds` for plain NameExpr iterables (matches ForLoop.cpp's
    /// iterable handling) and falls back to the resolved AST type otherwise.
    /// Returns Type::Kind::Int when no element type can be determined.
    Type::Kind getIterableElementKind(Expr* iterable) {
        // The receiver's OWN resolved container type is authoritative when its
        // element type is concrete. Prefer it over the varListElemKinds tracking
        // map: that map is program-wide, keyed by BARE variable name, and never
        // cleared between functions/modules, so a same-named list elsewhere
        // (e.g. an `out: list[SomeClass]` param) leaves a stale entry that would
        // otherwise mark THIS local `out: list[int]` as list[Instance] - routing
        // its append through dragon_list_append_ptr + a generic dragon_incref on
        // a raw i64 element (SEGV / UAF). The map is only a fallback for a list
        // whose static element type is not yet pinned - an unannotated `out = []`
        // whose element kind is learned from what gets appended (Unknown here).
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

    /// True when iterating `expr` directly means iterating a dict's KEYS - i.e.
    /// `expr` is a bare dict (variable, class field, or dict-typed expression),
    /// NOT a `.keys()`/`.items()`/`.values()` call (those already yield a list).
    /// Comprehensions and the for-loop must convert the evaluated DragonDict*
    /// to its keys list via dragon_dict_keys before indexing it as a list;
    /// indexing the dict pointer directly walks its raw bytes (-> SIGSEGV).
    bool isBareDictIterable(Expr* expr);

    /// Determine the correct DragonValueTag for a pointer-typed expression.
    /// Used by DictExpr to tag values properly (not blindly TAG_STR for all pointers).
    int64_t inferPtrValueTag(Expr* expr);

    /// Promote a string literal to a heap-allocated DragonString via dragon_string_dup.
    /// If the expression is a TRUE compile-time StringLiteral (headerless rodata
    /// pointer) or a NameExpr with VarKind::StrLiteral, calls dragon_string_dup to
    /// create a refcounted copy. Otherwise returns val as-is.
    ///
    /// An f-string parses as a StringLiteral node too (isFString=true), but its
    /// VALUE is already an owned +1 heap string built at runtime by the concat
    /// chain. It must NOT be dup'd here: the container setter then adopts the
    /// dup while the original +1 is orphaned - one leaked string per
    /// list-literal element, list.append, dict-literal value, d[k]=v (key and
    /// value), and xs[i]=v, every time the stored value is an f-string.
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
    // A pending defer's call entry: val is the void(i64*) thunk, tag is the
    // arg count. The unwinder invokes the thunk over the `tag` entries pushed
    // directly below it (each carrying its own DCLEAN kind for the post-call
    // release), then keeps popping so those entries drain normally.
    static constexpr int DCLEAN_DEFER_CALL = 5;

    /// Map an owned-heap VarKind to its DragonCleanupKind, mirroring
    /// emitScopeCleanupFor's per-kind decref dispatch. Returns 0 for non-heap
    /// (caller must not push). Union is handled separately (carries a box tag).
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

    /// The thread-local frame-count global (`__dragon_active_frames`), declared
    /// lazily. Initial-exec TLS model: call-free access (a GOT-relative TLS
    /// offset), valid because the runtime is statically linked into the
    /// executable. This is the ONLY inline TLS read codegen emits.
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

    /// Emit the inline cleanup gate: `__dragon_active_frames != 0`. A heap local
    /// declared with NO exception frame live can never be longjmp-unwound (any
    /// raise is uncaught -> exit), so its cleanup registration is skipped - the
    /// hot path pays only this predicted-untaken branch, no runtime call.
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

    /// Find the i32 alloca holding a cleanup-registered local's runtime slot
    /// index, searching the scope chain (mirrors setVar's owning-scope
    /// resolution). Null if the name was never registered.
    llvm::AllocaInst* findCleanupSlot(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->cleanupSlots.find(name);
            if (found != it->cleanupSlots.end()) return found->second;
            if (it->vars.count(name)) return nullptr;  // owning scope, not registered
        }
        return nullptr;
    }

    /// Register a freshly-declared owned heap local on the unwind cleanup stack
    /// and remember its slot (so a later reassignment can refresh the snapshot).
    /// `tagVal` is the box value-tag for Union locals (DCLEAN_UNION), else null.
    /// No-op outside RC mode or when the block already has a terminator.
    void emitCleanupPush(const std::string& name, llvm::Value* value,
                         int cleanupKind, llvm::Value* tagVal = nullptr);

    /// Refresh a registered local's cleanup snapshot after reassignment (its old
    /// value was already decref'd by storeWithRCOverwrite / the union path). No-op
    /// if the name was never registered.
    void emitCleanupUpdate(const std::string& name, llvm::Value* value,
                           llvm::Value* tagVal = nullptr);

    /// Push an anonymous for-loop temp (generator/iterator object) onto the
    /// unwind cleanup stack so a raise that unwinds the loop's frame frees it.
    /// These are NOT named scope locals, so emitScopeCleanupFor never sees them
    /// and they'd leak when an exception skips the loop's normal-exit decref.
    /// Returns an i32 alloca holding the depth to rewind to at that decref (or
    /// null in non-RC / terminated block). Pair with emitCleanupPopTemp.
    llvm::Value* emitCleanupPushTemp(llvm::Value* ptr, int cleanupKind);

    /// Register owned arg temps on the runtime cleanup stack for the duration
    /// of a call, so a raise that longjmps out of the callee frees them (the
    /// post-call decref only runs on normal return; without this an owned temp
    /// like the bytes literal in `assertRaises(..., lambda: f(b"x"))` leaks
    /// whenever the callee raises). Returns the per-temp
    /// rewind bases for popArgTempCleanups on the normal-return path. Only
    /// tag-independent kinds (Str/Callable/Obj) are registered - a Union temp
    /// needs a box value-tag the temp-cleanup path doesn't carry, so it stays
    /// with the existing normal-path decref.
    std::vector<llvm::Value*> pushArgTempCleanups(
        const std::vector<std::pair<llvm::Value*, VarKind>>& argTemps);

    /// Rewind (does NOT free) the cleanup entries pushed by pushArgTempCleanups,
    /// in reverse push order, on the normal-return path before the decref.
    void popArgTempCleanups(const std::vector<llvm::Value*>& bases);

    /// Rewind the cleanup stack past a temp pushed by emitCleanupPushTemp - call
    /// at the loop's normal-exit decref site, where codegen already freed the
    /// temp, so a later unwind doesn't double-free the now-stale snapshot.
    void emitCleanupPopTemp(llvm::Value* baseAlloca);

    /// Emit dragon_decref calls for all heap-typed, non-borrowed locals in
    /// the current (innermost) scope. Must be called BEFORE the scope's
    /// terminator is emitted and BEFORE popScope().
    /// Phase 2: Str uses dragon_decref_str (navigates from data ptr to header).
    /// Phase 3: ClassInstance uses dragon_decref (pointer IS the header, like containers).
    /// Emit decrefs for all heap-typed, non-borrowed locals in a single scope.
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

    /// Clean up ALL scopes from innermost to outermost.
    /// Used by return statements which exit the entire function.
    void emitAllScopeCleanup() {
        if (scopes.empty()) return;
        if (options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            emitScopeCleanupFor(*it);
        }
    }

    /// Clean up scopes from innermost down to (and including) targetDepth.
    /// Used by break/continue to clean loop-interior scopes without
    /// touching the enclosing function scope.
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
                    callDunder(it.className, "__exit__", it.val);
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

    /// Early-exit (return/break/continue) cleanup: walk scopes innermost to
    /// `targetScopeDepth` and exit-cleanup entries down to `ecDownTo`,
    /// INTERLEAVED by nesting depth - each scope's defers + decrefs run
    /// before the finally/__exit__ that encloses that scope, exactly as they
    /// would on the normal fall-through path. Replaces the old two-flat-pass
    /// order (all finallys, then all scopes), which ran an enclosing finally
    /// before an inner scope's deferred calls.
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

    /// True if the body contains a value-returning `return <expr>` (not a bare
    /// `return`). An unannotated function with NO value-returning return is a
    /// procedure and must get a `void` LLVM return type: a bare `return` lowers
    /// to `ret void`, which would mismatch the historical i64 no-annotation
    /// default and fail LLVM verification. Stops at nested function/class
    /// bodies - their returns belong to them, not the enclosing function.
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

    /// LLVM return type for an unannotated function/method: a procedure (no
    /// value-returning return) is `void`; everything else keeps the historical
    /// `int` default. Centralizes the rule so forward-declaration and body
    /// emission agree (a divergence would fail LLVM verification).
    llvm::Type* unannotatedReturnType(const std::vector<std::unique_ptr<Stmt>>& body) {
        return bodyReturnsValue(body) ? i64Type : voidType;
    }

    /// Infer the yielded value's VarKind from the first YieldExpr in a
    /// generator body. Used to bind `for x in gen()` loop var with the
    /// correct kind so heap-typed yields (str/list/dict/instance) round-trip
    /// through the consumer instead of being printed as raw i64. Returns Int
    /// if no yields are found or the value's type is unresolved.
    VarKind inferYieldKind(const std::vector<std::unique_ptr<Stmt>>& body);

    /// Map: generator function name -> VarKind of values it yields.
    /// Populated when a generator function is compiled; consulted by the
    /// for-in-over-generator path to type the loop variable.
    std::unordered_map<std::string, VarKind> generatorYieldKinds;

    /// Map: variable name (storing a Generator) -> VarKind of yielded values.
    /// Populated when `g = some_gen_fn(...)` is assigned, so subsequent
    /// `for x in g { ... }` loops know how to type x.
    std::unordered_map<std::string, VarKind> varGenYieldKinds;

    /// Fill missing arguments with default values at a call site.
    // C9-B shared spread expansion. Expand `node`'s positional args (with
    // `*tuple`/`*list` spread) and kwargs (with `**dict` spread) into the
    // fully-coerced `args` vector against `func`'s signature, registering owned
    // heap temporaries in `argTemps` (spread elements are BORROWED - never
    // listed). `args` may already hold prefix values (e.g. `self`); expansion
    // continues from args.size(). Does NOT fill defaults or emit the call - the
    // caller owns dispatch (so a method call routes through its vtable). Returns
    // false on a diagnosed error (lastValue poisoned). Defined in CallExpr.cpp.
    bool expandSpreadCallArgs(
        CodeGen& cg, llvm::Function* func, CallExpr& node,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName);

    // Pack a variadic method call's surplus positionals into a `*args` list and
    // surplus keywords into a `**kwargs` dict, given `self` already pushed as
    // args[0] (empty for a static variadic method). The method-path twin of the
    // free-function emitVarArgCall, differing only by the leading-self offset:
    // funcParamNames includes "self" at index 0 for an instance method, while
    // VarArgInfo.numRegularParams counts only the user params before `*args`.
    // The packed list/dict are call-site-owned temporaries registered in
    // `argTemps` so the shared call tail drains them (the callee borrows).
    // Returns true when `args` is fully built (caller emits the call), false
    // after emitting a diagnostic (caller returns, lastValue poisoned). Defined
    // in CallMethods.cpp.
    bool packVarArgMethodArgs(
        CodeGen& cg, CallExpr& node, const std::string& methodFuncName,
        llvm::FunctionType* methodFuncType,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName);

    // Bind the regular-param slots listed in `bindIdx` by NAME from a `**dict`
    // spread source `d`: a required param (no default) raises TypeError when
    // its key is absent, an optional param PHIs between the dict value and its
    // default expr. Bound heap values are BORROWED from the dict. Shared by
    // the fixed-arity spread path (expandSpreadCallArgs) and the variadic one
    // (emitVarArgCall). Defined in CallExpr.cpp.
    void bindParamSlotsFromDict(
        CodeGen& cg, llvm::Function* func, llvm::Value* d,
        std::vector<llvm::Value*>& args, const std::vector<size_t>& bindIdx,
        const std::vector<std::string>& paramNames, const std::string& dispName);

    /// Evaluates default Expr* AST nodes for params not already supplied.
    ///
    /// D040: scans the full [0, numParams) range and fills any slot that is
    /// either past args.size() OR currently nullptr (the latter happens when
    /// kwargs binding leaves a hole between positional and keyword-filled
    /// positions - see CallExpr.cpp non-vararg path).
    // `defaultTemps` (optional): owned heap temporaries synthesized for omitted
    // args (Dragon evaluates a default per-call, so `def f(x: list = [])` mints a
    // fresh +1 each time the arg is omitted). The callee borrows it like any arg,
    // so the call site must release it after the call - pass the same argTemps
    // vector the regular args drain through. Skipped (nullptr) by callers that
    // don't drain. (#3 class A, default-value temps.)
    void fillDefaultArgs(const std::string& funcName, llvm::Function* func,
                         std::vector<llvm::Value*>& args, CodeGen& cg,
                         std::vector<std::pair<llvm::Value*, VarKind>>* defaultTemps = nullptr);

    /// Emit an atomic incref for a pointer value being passed across a thread
    /// boundary (fire fn(args), async def wrapper). Dispatches to the correct
    /// atomic incref variant based on VarKind.
    ///
    /// Also emits `dragon_mark_shared_deep` (or `_str`) on the value first -
    /// see d018-shared-refcount.md. This propagates SHARED to every reachable
    /// child so plain dragon_incref/decref calls inside the vthread body
    /// route to the atomic path. Without this, two vthread bodies on
    /// different OS threads tear the refcount on shared Router state and
    /// the cycle collector then walks freed memory (the original
    /// hello_server crash at request ~87).
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

    /// Storing a heap value into a shared isntance field makes it globally reachable
    /// through that instance so itm must be shared-marked like the list/sdict store
    /// barriers already do for elements. Previously a gap masked only by fire path
    /// re-marking `self` per connection.
    /// UNSHARED instance pays one byte load + predicted-untaken branch (the
    /// SHARED bit lives at header offset 9); only genuinely shared instances
    /// reach the runtime call
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

    /// A value stored into a MODULE GLOBAL is reachable from
    /// every vthread BY NAME - it never crosses a `fire` boundary, so the
    /// fire-site mark (emitAtomicIncref above) never sees it. Two handler
    /// vthreads on different OS workers then run plain non-atomic
    /// incref/decref on the same object: torn refcount, premature free, heap
    /// use-after-free (the /copy-a-global-to-a-local server crash). Mark the
    /// stored graph SHARED at the global-store site instead; the SHARED store
    /// barriers in list/dict keep the invariant for values inserted later.
    /// Cold path: module globals are stored once at init / rarely reassigned.
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

    // Conservative borrowed-reference detector for assignment / consume-site
    // RHS expressions. Name, attribute, and subscript reads are existing
    // references owned by their enclosing slot/container; storing them into
    // another owning slot requires an incref. Fresh-ref expressions
    // (literals, calls, container constructors) already own a +1 and are NOT
    // borrowed.
    static bool isBorrowedHeapExpr(Expr* expr) {
        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            // A SLICE (s[1:4], xs[a:b]) calls dragon_str_slice /
            // dragon_list_slice / dragon_bytes_slice, all of which return a
            // FRESH +1 object the consumer owns - never a borrow. Treating it as
            // a borrow adds an extra incref on store (and skips the arg-temp
            // decref), leaking one object per evaluation.
            if (dynamic_cast<SliceExpr*>(sub->index.get()) != nullptr)
                return false;
            // A STRING element read (s[i]) is likewise OWNED, not borrowed:
            // strings are immutable, so dragon_str_index mallocs a fresh 1-char
            // string - there is no interior reference to hand back. Same fresh
            // +1 as a slice, so the same rule applies; misclassifying it as a
            // borrow leaked one 1-char string per evaluation (ord(s[i]),
            // c = s[i], len(s[i]), and binascii.hexlify's ord(HEX[n]) hot loop).
            // list / dict / set element reads (xs[i], d[k]) DO borrow the
            // container's stored reference, so they stay borrowed.
            if (sub->object && sub->object->type &&
                sub->object->type->kind() == Type::Kind::Str)
                return false;
            // A DICT element read whose receiver is itself a CALL
            // (`r.info()["k"]`, `cfg.get("db")["host"]`): the receiver is an
            // owned temp the read consumes, and the lowering
            // (retainElemThenReleaseRecv in Attributes.cpp) retains the
            // element by kind before releasing it - handing the consumer an
            // owned +1, the mirror of the f().attr rule below. Only the
            // concrete-heap element branches retain (Any/Union box reads and
            // closure elements keep today's borrow story), so the owned
            // classification is gated the same way.
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
        // A walrus target slot ADOPTS its value's +1 (the store site passes
        // rhsBorrowed=false and skips the incref), so the expression hands its
        // consumer a BORROW of the slot's value - exactly like reading the
        // name. Classifying it owned made a call site drain `takes(x := ...)`
        // while x still held the same pointer (A/B-proven use-after-free,
        // test_rc_walrus.dr).
        if (dynamic_cast<WalrusExpr*>(expr) != nullptr) return true;
        if (auto* nm = dynamic_cast<NameExpr*>(expr)) {
            // `own x` at a consuming position (docs/002 2.8): the binding's
            // +1 TRANSFERS - an owned value by definition, never a borrow.
            // The consumer adopts (no incref) and the move-out nulls the
            // source slot, so the single reference stays single.
            // `dub x` (2.7) likewise hands its consumer a fresh owned +1
            // (deep copy / identity retain) - never a borrow.
            return !nm->isMoveMarked && !nm->isDubMarked;
        }
        if (auto* at = dynamic_cast<AttributeExpr*>(expr)) {
            // `f().attr`: the receiver is an owned temp the read consumes;
            // the lowering (Attributes.cpp) RETAINS the field by kind and
            // releases the receiver, handing the consumer an owned +1 - so
            // an attr-on-a-CALL is NOT a borrow. A field read off a named
            // object stays borrowed as ever.
            return !dynamic_cast<CallExpr*>(at->object.get());
        }
        return false;
    }

    /// docs/002 moves: after a call consumed `f(own x)` arguments, null each
    /// moved-out binding's slot. The callee adopted the caller's +1, so the
    /// caller's scope-exit release must see nothing (decref of null no-ops;
    /// the Lock scope-destroy is null-gated). E9-at-join guarantees every
    /// path agrees, so this is bookkeeping, not a runtime drop flag.
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
                // The unwind cleanup stack snapshots the value at declaration;
                // the callee adopted that +1, so a later longjmp unwind must
                // see null here, not re-free what the callee now owns.
                emitCleanupUpdate(
                    nm->name,
                    llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(i8PtrType)),
                    nullptr);
            }
        }
    }

    // setdefault key ownership (#20a): a NEW str-keyed insert stores the key by
    // POINTER (adopt), so the dict must own a real ref. Incref a BORROWED heap
    // str key here; literals need nothing (dragon_dict_release_key no-ops on
    // them - no promotion/alloc, faster than the d[k]=v store path) and owned
    // temps already carry +1. No-op for int keys (i64, not a pointer) and non-RC.
    // The runtime setdefault releases this key on its present branch, so a
    // borrowed key that turns out to already exist never leaks. Shared by the
    // heap-valued, scalar, and syncdict setdefault call sites (one definition so
    // the three can't drift).
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

    // Emit non-atomic incref/decref for a value based on VarKind.
    // Str uses string-specific RC entrypoints; all other heap kinds use
    // generic object RC entrypoints.
    void emitIncrefByKind(llvm::Value* val, VarKind kind);

    void emitDecrefByKind(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (kind == VarKind::Union) {
            // A Union value is a {tag, payload} box VALUE, not a pointer:
            // extract and release by runtime tag (no-op for scalar tags and
            // the zero-initialized {0, 0} box). Mirrors emitIncrefByKind so
            // the two by-kind entry points share one ownership story.
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

    /// Owned heap-temporary call arguments carry a +1 the callee borrows but
    /// never consumes (a callee increfs when it retains - e.g. a ctor's
    /// `self.f = param` - so the caller's reference always returns to the
    /// caller). After the call the caller must release it or it leaks
    /// (the binary-trees / object-tree leak). Given an argument expression and
    /// the callee's formal param kind, return the kind to decref the raw arg
    /// value with after the call, or VarKind::Other to skip.
    ///  - Borrowed exprs (Name/Attribute/Subscript) keep their owner's
    ///  reference - never decref them here.
    ///  - Str args are only released when they are owned heap-string *results*
    ///  (concat / f-string / str() / call); literals and borrows have no
    ///  +1 to drop and (for literals) no header to navigate.
    ///  - Union/box params manage RC by tag; left to the box machinery.
    VarKind argTempDecrefKind(Expr* argExpr, VarKind paramKind, llvm::Value* rawVal) {
        if (options.gcMode != GCMode::RC) return VarKind::Other;
        if (paramKind == VarKind::Union) {
            // Union/Any params take the SAME callee-borrows contract as
            // every typed param: the callee retains by increfing (the Union
            // arm of emitIncrefByKind fires on field stores / return aliases
            // via storeWithRCOverwrite), so the caller's owned temp is always
            // the caller's to drain. That incref-on-retain is the load-bearing
            // half of the contract: if an Any FIELD store ever adopts the
            // temp's +1 without increfing again, this drain double-frees the
            // retain case (heap-use-after-free in __dragon_dealloc_<Class>,
            // pinned by test_rc_any_field.dr). Monomorphizing
            // spurious-Any params into generics [T] remains the better fix
            // where the concrete type is knowable (zen: types are honest);
            // this drain covers the genuinely dynamic remainder.
            // A PROVABLY-OWNED box result (dragon_box_subscript / dragon_box_binop
            // / an Any-returning call) carries a +1 the callee borrows, so it must
            // be drained even when the SOURCE EXPRESSION reads as borrowed: a
            // subscript on an Any value lowers to dragon_box_subscript (OWNED +1),
            // but isBorrowedHeapExpr classifies every list/dict subscript borrowed
            // for the typed-container element-read case. isOwnedBoxResult is
            // value-based and precise - the borrowed-box returners (dict_get_box /
            // dict_int_get_box / list_box_get) are isOwnedBoxResult=false, so a
            // BORROWED element read (dict[str,Any] / list[Any], the hot path in a
            // parsed-JSON server) is still NOT drained (the container keeps the +1;
            // draining it would double-free). Ordered BEFORE the isBorrowedHeapExpr
            // gate so the owned-subscript +1 is not lost to the borrowed-subscript
            // classification (was: leaked one payload per `f(anyVal[k])`).
            if (rawVal && rawVal->getType() == boxType)
                return isOwnedBoxResult(rawVal) ? VarKind::Union : VarKind::Other;
            if (isBorrowedHeapExpr(argExpr)) return VarKind::Other;
            // Owned NATIVE heap temp boxed at the boundary (concat, ctor,
            // slice, ... into `x: Any`): the box borrows the payload, so the
            // native +1 is drained by the temp's own static type.
            return ownedTempDrainKind(argExpr, rawVal);
        }
        if (!isHeapKind(paramKind))
            return VarKind::Other;
        // A box arg unboxed into a native heap param (coerceArg unboxes it): an
        // OWNED box temporary (Any-returning call / box_subscript) carries a +1
        // the callee borrows, so release it after the call as a UNION
        // (emitUnionDecref extracts and drops the payload by tag - the same drain
        // the Any-param branch above uses). A BORROWED box (dict_get_box /
        // list_box_get, or a ternary over one) belongs to its container and is
        // never drained. Ordered before the isBorrowedHeapExpr gate, which reads
        // false for a ternary source and would otherwise mis-drain the box by the
        // param's kind (decref of a box struct as a bare str ptr).
        if (rawVal && rawVal->getType() == boxType)
            return isOwnedBoxResult(rawVal) ? VarKind::Union : VarKind::Other;
        if (isBorrowedHeapExpr(argExpr)) return VarKind::Other;
        if (paramKind == VarKind::Str && !isOwnedStrResult(rawVal))
            return VarKind::Other;
        return paramKind;
    }

    // Classify one PRE-coerce call argument and, if it is an owned heap temporary
    // the callee borrows, record it in `out` for release after the call. Skips
    // ptr-returning extern-C callees (interior-pointer hazard at the FFI edge;
    // see externDrainableFuncs) and anything argTempDecrefKind rejects
    // (borrowed expr / str literal / scalar).
    // The single place direct-call sites (cross-module fn, module-attr ctor,
    // static methods, ...) route owned-temp tracking through (#3, class A).
    void collectArgTemp(const std::string& funcName, Expr* srcExpr,
                        llvm::Value* rawArg, unsigned paramIdx,
                        std::vector<std::pair<llvm::Value*, VarKind>>& out) {
        if (options.gcMode != GCMode::RC) return;
        if (externFuncNames.count(funcName)) {
            // A ptr-returning extern is not drainable (interior-pointer hazard).
            // A drainable extern is classified by the ARG's own static type,
            // never the declared param kind (the same C symbol can carry
            // disagreeing arg types across modules; classifying by param kind
            // drains a borrowed pointer - a use-after-free), so route
            // through ownedTempDrainKind.
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

    // Classify an owned heap temporary passed to a BORROW callee (a builtin
    // method / function that reads its argument transiently and produces a
    // fresh result without storing or returning the argument - str.split,
    // dict.get(key), int(s), len(x), ...). Unlike collectArgTemp this needs no
    // callee param-kind table: a borrow callee never consumes the +1, so any
    // owned heap temp the caller materialized for the arg must be released after
    // the call or it leaks once per call. Returns VarKind::Other (skip) for
    // borrowed exprs (Name/Attribute/element-read keep their owner's ref), str
    // literals, and scalars - none carry a droppable +1. NOT for transfer
    // callees (list.append, dict set: they adopt the +1) - those must skip this.
    VarKind ownedTempDrainKind(Expr* e, llvm::Value* v) {
        if (options.gcMode != GCMode::RC) return VarKind::Other;
        if (!v || !e || !e->type || isBorrowedHeapExpr(e)) return VarKind::Other;
        // Gate on the argument's STATIC type, not just the LLVM value: at LLVM
        // level str / list / dict / set / bytes are all i8*, and isOwnedStrResult
        // treats any non-blocklisted i8*-returning call as an owned STRING - so a
        // set/list temp would be misclassified Str and freed with dragon_decref_str
        // (a string-header walk past the container struct - heap overflow). The
        // type kind picks the correct release entry point.
        switch (e->type->kind()) {
            case Type::Kind::Str:
                return isOwnedStrResult(v) ? VarKind::Str : VarKind::Other;
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
                // All release via dragon_decref (VarKind::List is a representative
                // non-Str / non-Closure heap kind in emitDecrefByKind).
                return (v->getType()->isPointerTy() && isOwnedPtrResult(v))
                           ? VarKind::List : VarKind::Other;
            default:
                return VarKind::Other;  // int/float/bool/Any/Function: no owned heap +1
        }
    }

    // Wrap an already-evaluated borrow-callee argument: if it is an owned heap
    // temporary, record it in `sink` for release after the call, and return the
    // value unchanged so call sites read `trackBorrowTemp(expr, lastValue, sink)`
    // in place of a bare `lastValue`. The drain happens once per dispatch block
    // (str/bytes common tail; per-handler for list/dict/set), so `sink` is a
    // block-local vector - nesting-safe (a nested builtin call has its own).
    llvm::Value* trackBorrowTemp(Expr* e, llvm::Value* v,
                                 std::vector<std::pair<llvm::Value*, VarKind>>& sink) {
        VarKind k = ownedTempDrainKind(e, v);
        if (k != VarKind::Other) sink.emplace_back(v, k);
        return v;
    }

    //===-- D027.1: Heap-boxed cell read / write helpers ------------------===//
    //
    // The cell stores values as i64; native types (float/bool/ptr) round-trip
    // through bitcast / zext / ptrtoint at the boundary so the cell layout
    // stays uniform. Heap kinds also obey the "incref new before set, decref
    // returned-old after" discipline so RC remains balanced across the
    // overwrite even if old==new.

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

    /// Allocate a fresh cell for a `nonlocal`-promoted local. The caller is
    /// responsible for the +1 refcount of any heap value placed in `valueI64`
    /// - the cell does not auto-incref. Returns the cell pointer (i8*).
    /// D030 §5: when the source-level Type::Kind is available, prefer it for
    /// tag derivation so bytes-typed cells round-trip with TAG_BYTES even
    /// after VarKind::Bytes is collapsed into the generic-heap cohort.
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

    /// Write to a cell-backed local. `newVal` is at native type. When the
    /// value is BORROWED (a name/field/element read - some other slot owns
    /// it), the cell increfs to take its own reference; an OWNED fresh value
    /// (a concat / call result) already carries the +1 the cell adopts.
    /// Incref'ing an owned value too left every intermediate at refcount 2:
    /// a `nonlocal` str accumulator (`acc = acc + s`) leaked one string PER
    /// MUTATION (534KB per 1000 loop iterations under LSan). Decrefs the
    /// prior cell contents returned by dragon_cell_set under the same kind,
    /// so the cell holds a balanced +1 ref to the latest value at all times.
    /// `newIsBorrowed` defaults to true (the incref side) - the safe side for
    /// self-aliasing writes (`s = s`) and in-place aug-assign results.
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
            // Decref via the kind's native pointer type - same dispatch as
            // emitDecrefByKind, just reconstructed from i64. The first write
            // sees old == 0 (fresh cell), and decref(NULL) is a no-op.
            auto* oldPtr = builder->CreateIntToPtr(oldI64, i8PtrType, name + ".old.p");
            emitDecrefByKind(oldPtr, kind);
        }
    }

    // Emit conditional decref for a union-typed variable based on its runtime tag.
    // Only decrefs if the tag indicates a heap type (str, list, dict, bytes, etc.).
    void emitUnionDecref(llvm::Value* val, llvm::Value* tag);

    // Emit conditional incref for a union-typed variable based on its runtime tag.
    void emitUnionIncref(llvm::Value* val, llvm::Value* tag);

    // RC-aware store with overwrite cleanup.
    // For heap-typed slots this performs:
    //  1) (optional) incref new when RHS is borrowed from an existing owner
    //  2) decref old occupant (guarded against self-assignment)
    //  3) store new value
    void storeWithRCOverwrite(llvm::Value* slotPtr, llvm::Type* slotValueType,
                              llvm::Value* newVal,
                              VarKind oldKind, VarKind newKind,
                              bool newIsBorrowed,
                              const std::string& name = "");

    // If `name` currently denotes a BORROWED slot (a parameter, loop variable,
    // capture, or `self` - one the callee does not own), clear the borrowed mark
    // in its owning scope and return true. Used by storeWithRCOverwrite: the
    // first reassignment of a borrowed slot must NOT decref its old value (that
    // reference belongs to the caller), and after the store the slot owns its
    // new value, so it must be cleaned up at scope exit like any owned local.
    // Walks scopes outward and consults the same owning-scope rule as setVar.
    bool consumeBorrowedSlot(const std::string& name);

    // Non-mutating peek of the same borrowed mark consumeBorrowedSlot clears:
    // is `name`'s innermmost binding currently a borrowed slot? Used to gate the
    // owned-str -> StrLiteral downgrade guard (a literal sotre must keepn an
    // owned slot's cleanup kind Str, but must NOT promote a BORROWED slot -
    // decref'ing the caller's value on a not-taken branch would be a UAF).
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

    // Emit an amortized in-place string append for `slot = slot + rhs` and
    // `slot += rhs`. The runtime entry point CONSUMES the slot's old
    // reference (in-place realloc reuse, or fallback concat + decref) and
    // returns the new value - so we MUST plain-store it, NOT route through
    // storeWithRCOverwrite, whose old-value decref would double-consume `cur`.
    // `rhs` is only borrowed by the entry point; decref it here iff it is an
    // owned intermediate, exactly as the dragon_str_concat path does
    // (isOwnedStrResult / Expressions.cpp). The slot then holds a dynamic heap
    // Str, so its VarKind is updated for block-exit cleanup.
    void emitStrAppendInplace(llvm::Value* slotPtr, llvm::Value* cur,
                              llvm::Value* rhs, const std::string& name);

    // Lower Python integer modulo `a % b` and floor-division `a // b`. Both use
    // FLOOR semantics - the result tracks the sign of the *divisor*, unlike C's
    // truncated `srem`/`sdiv`. We emit them INLINE rather than as a per-use call
    // to dragon_mod_int / dragon_floordiv_int, whose call overhead dominated tight
    // loops (`x % 256`, `i % k`, `n // 2`, ...): a function call vs a few inline
    // instructions. This shared scaffold picks the strategy by divisor shape:
    //
    //  - Nonzero CONSTANT divisor: no guard needed (it can't be zero) - emit the
    //  inline op + branchless floor-correction `select` directly.
    //  - VARIABLE divisor: branch on `b == 0`. The (predictable, normally
    //  never-taken) zero path calls the runtime fallback, which prints the
    //  ZeroDivisionError message and exit(1)s - preserving existing behavior;
    //  the nonzero path is fully inline. A phi merges the two results.
    //  - Literal `0` divisor: keep the call (compile-time div-by-zero, rare).
    //
    // `a`/`b` are already i64 at every call site (the integer-arithmetic path).
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

        // Zero path: the runtime fallback prints ZeroDivisionError + exit(1). It
        // never returns, but it isn't marked noreturn, so we feed its (dead)
        // result into the phi rather than emit a noreturn-lying `unreachable`.
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

    // Floor division: q = a / b, then correct toward -inf when the operands have
    // opposite signs and the division was inexact:
    //  if ((a ^ b) < 0 && a % b != 0) q -= 1.
    // The `srem` reuses the divide hardware result - LLVM fuses an adjacent
    // sdiv+srem of the same operands into a single idiv - so detecting the
    // remainder is near-free (and matches dragon_floordiv_int's `d*b != a` test:
    // `a % b == 0` exactly when `b` divides `a`).
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

    // Compute `cur OP rhs` for an integer augmented-assignment op token (i64
    // operands). Mirrors the int arithmetic in the NameExpr aug-assign path and
    // reuses emitIntMod for `%=`. Returns nullptr for ops that don't yield an
    // int result (e.g. `/=` true division) so the caller can skip. Used by the
    // list/dict element aug-assign lowering.
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

    // Compute `cur OP rhs` for a float augmented-assignment op token (f64
    // operands). Mirrors emitIntAugOp for the float path; shared by every
    // float aug-assign target (NameExpr already inlines, but dict/list/field
    // element targets route here). `//=` and `%=` use Python float floor/mod
    // semantics. Returns nullptr for ops with no float meaning (bitwise/shift).
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

    // Python float modulo `a % b`: the result takes the sign of the DIVISOR
    // (unlike C `fmod`/LLVM `frem`, which take the sign of the dividend). Start
    // from frem (= fmod) then add b when the remainder is nonzero and its sign
    // disagrees with b - matching CPython's float_mod.
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

    // Like lookupVar but restricted to the innermost (current) scope. A
    // `:`-declaration uses this to decide reuse-vs-shadow: a name that
    // resolves only in an ENCLOSING scope must be shadowed with a fresh slot,
    // not aliased onto the outer binding (which would overwrite it and, on a
    // type change, reinterpret its slot at the wrong LLVM type).
    llvm::AllocaInst* lookupVarInCurrentScope(const std::string& name) {
        if (scopes.empty()) return nullptr;
        auto found = scopes.back().vars.find(name);
        return found != scopes.back().vars.end() ? found->second : nullptr;
    }

    void setVar(const std::string& name, llvm::AllocaInst* alloca,
                 VarKind kind = VarKind::Other);

    // D027.1: walk scope chain to find whether this name's alloca holds a
    // DragonCell pointer (rather than the value directly). Reads/writes
    // route through dragon_cell_get/set when this returns true.
    bool isCellBacked(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->cellBacked.count(name)) return true;
            // Stop at the scope that actually defines the name - a same-name
            // shadow in a deeper scope must not be confused with an outer
            // cell-backed binding.
            if (it->vars.count(name)) return false;
        }
        return false;
    }

    // D027.1: mark a name in the innermost scope as cell-backed. Used both
    // at the cell-promoted definition site (outer fn) and at the env-load
    // site (inner fn whose env carries a cell pointer for this capture).
    void markCellBacked(const std::string& name) {
        if (!scopes.empty()) scopes.back().cellBacked.insert(name);
    }

    // B Phase 1: mark a freshly-bound local as a stack-allocated instance in
    // its owning scope, so block-exit cleanup skips the decref. Mirrors the
    // owning-scope search in setVar (a fresh declaration lands in scopes.back).
    void markStackAllocated(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->vars.count(name)) { it->stackAllocated.insert(name); return; }
        }
        if (!scopes.empty()) scopes.back().stackAllocated.insert(name);
    }

    // Set by the CallExpr constructor fork when it lowers a class construction
    // to a stack alloca (a NoEscape site of a scalar-only class). The binding
    // site reads + clears it to mark the bound local stack-allocated.
    bool lastWasStackInstance = false;

    // B Phase 1: ctor CallExpr* sites whose result is bound to a non-escaping
    // local (populated by computeStackAllocSites before the entry body is
    // emitted). Keyed by AST node identity.
    std::unordered_set<const CallExpr*> stackAllocSites;

    // Bound-Task tail: `t: Task[...] = fire ...` declarations whose
    // bound local PROVABLY does not escape the rest of its block (reusing the
    // same conservative escape walk as stackAllocSites - any use, INCLUDING a
    // join/await/is_alive method call, counts as escape). So this captures ONLY
    // the genuinely-unused bound-fire-and-forget case that leaks the handle ref;
    // a joined/awaited task already drops it, an escaped one keeps it for its new
    // owner. Populated by computeStackAllocSites; consulted at the binding site
    // to arm scope.detachOnExit. Keyed by AST node identity.
    std::unordered_set<const AnnAssignStmt*> detachableTaskDecls;

    // B Phase 1: classes eligible for stack construction - scalar-only fields
    // (no heap children to tear down), exactly one constructor that does not
    // let `self` escape, and no class-body per-instance field defaults (so a
    // memset + __init__ exactly reproduces _new's field initialization).
    // Computed during class codegen; consulted at the CallExpr fork together
    // with stackAllocSites. Keyed by source class name.
    std::unordered_set<std::string> stackEligibleClasses;

    VarKind lookupVarKind(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->varKinds.find(name);
            if (found != it->varKinds.end()) return found->second;
        }
        // Check module globals
        auto mgIt = moduleGlobalKinds.find(name);
        if (mgIt != moduleGlobalKinds.end()) return mgIt->second;
        return VarKind::Other;
    }

    // Determine if an expression produces a bytes value.
    // D030 §5: prefer the typechecker's static type as the source of truth.
    // Falls back to AST node shape and the source-level VarKind for paths
    // where the typechecker hasn't propagated a Type yet (and for legacy
    // VarKind::Bytes-tagged slots that haven't been migrated).
    bool exprIsBytes(Expr* expr);

    // Look up a module-level global variable. Returns nullptr if not found.
    llvm::GlobalVariable* lookupModuleGlobal(const std::string& name) {
        auto it = moduleGlobals.find(name);
        if (it != moduleGlobals.end()) return it->second;
        return nullptr;
    }

    // Check if we should use a module global for this variable name.
    // In .dr mode: always (scope chain resolution).
    // In .py mode: module-level code always accesses globals; inside functions
    // only if `global x` was declared.
    bool shouldUseModuleGlobal(const std::string& name) {
        // Mode-independent (D: .dr/.py parity). A function may READ a module
        // global with no keyword in both modes; this is only consulted after
        // lookupVar found no shadowing local, so using the global is always the
        // correct read. WRITES to a module global from a function require
        // `global` - but that is enforced uniformly in Sema, so by the time we
        // get here any reachable write already carries the declaration. Hence
        // no `.py`-vs-`.dr` gate and no `globalDeclaredVars` check.
        (void)name;
        return true;
    }

    // Create alloca in function entry block (for stable stack)
    llvm::AllocaInst* createEntryAlloca(llvm::Function* func,
                                         const std::string& name,
                                         llvm::Type* type);

    // Determine VarKind from a type annotation
    // Resolve a (possibly dotted) NamedTypeExpr name to a class name in the
    // flat classNames table. Same-module names match directly. Cross-module
    // names like `mod.Foo` (legal after Parser supports dotted type
    // annotations) match the trailing segment, since all linked modules
    // share one LLVM symbol space - the class struct is registered under
    // the bare class name regardless of which module defined it.
    std::string resolveAnnotationClassName(const std::string& name) const {
        if (classNames.count(name)) return name;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            std::string leaf = name.substr(dot + 1);
            if (classNames.count(leaf)) return leaf;
        }
        return "";
    }

    // Bind a class-typed variable's class name AND owning module together from
    // its declared type. varClassNames and varClassOwningModule are program-
    // wide and never cleared, so recording only the name (the old param/loop
    // behavior) let a STALE owning module from an earlier same-named binding in
    // another function survive into this scope and misdirect method dispatch to
    // `<staleModule>__<class>_<method>`. Always set the pair. Also handles a
    // DOTTED annotation (`x: mod.Class`) via resolveAnnotationClassName, which
    // a bare classNames.count() missed. No-op for non-class types, so it never
    // writes a garbage module for `x: str` / `x: tuple[...]`.
    void bindClassVar(const std::string& varName, TypeExpr* typeExpr) {
        auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr);
        if (!named) return;
        std::string cn = resolveAnnotationClassName(named->name);
        if (cn.empty()) return;
        varClassNames[varName] = cn;
        varClassOwningModule[varName] = resolveClassOwningModule(cn);
    }

    // D044 - Type::toString-equivalent canonical name for a TypeExpr, used to
    // recover the stamped class name of a generic-instantiation annotation
    // (`Box[int]`). Must match TypeChecker::mangleInstantiation (a user-generic
    // class's top-level args are joined by ',') AND Type::toString (the built-in
    // dict/tuple containers use ', '). Builtin container args nested inside a
    // user-generic therefore keep ', '; the user-generic level uses ','.
    std::string typeExprCanonicalName(TypeExpr* t) const;

    // If `t` is a generic-class instantiation annotation whose class was stamped
    // (`Box[int]` -> the stamped class "Box[int]"), return that class name; else "".
    std::string genericInstanceClassName(TypeExpr* t) const {
        if (!dynamic_cast<GenericTypeExpr*>(t)) return "";
        std::string c = typeExprCanonicalName(t);
        return classNames.count(c) ? c : "";
    }

    /// D030 §5: source-of-truth Type::Kind from a TypeExpr annotation.
    /// Used wherever the static type needs to survive past the VarKind layer
    /// (e.g. typedDictFieldKinds, where bytes-vs-list disambiguation matters
    /// for runtime tag dispatch). Mirrors typeExprToKind but returns the
    /// type-system Type::Kind directly.
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

    // Extract the class-name member of a union type, if any. Used to recover
    // the concrete class name for narrowing `Foo | None` -> Foo so attribute
    // access (`x.field`) finds the right struct layout.
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

    // Niche-pointer optimization: when a Union is exactly `T | None` and T's
    // LLVM representation is a pointer (class instance, list, dict, str, bytes,
    // tuple, set, ptr), lower the union as a bare nullable pointer instead of a
    // {i64,i64} box. `null` represents None. `r != none` becomes a 1-cycle null
    // compare; `r.field` is a normal field load - no boxing/unboxing/tag check.
    // Boxed Union machinery stays for non-niche unions like `int | str`.
    //
    // Returns the non-None member's TypeExpr* on success, or nullptr otherwise.
    TypeExpr* unionNicheMember(TypeExpr* typeExpr);

    //===------------------------------------------------------------------===//
    // D030 Phase 4 - Box helpers ({i64 tag, i64 payload})
    //===------------------------------------------------------------------===//

    /// Coerce a native-typed value to the box's i64 payload slot.
    /// Floats bitcast (preserve bit pattern). Pointers PtrToInt. Bools ZExt.
    /// i64 / int identity. Caller decides the tag separately.
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

    /// Extract the payload as a native LLVM type matching `kind`. Used by
    /// isinstance narrowing: once `box.tag == tag(T)` is verified, this gives
    /// the value at T's native LLVM type with no further conversion needed.
    llvm::Value* boxPayloadAsKind(llvm::Value* box, VarKind k);

    // D039 Phase 11: arithmetic op token -> dragon_box_binop opcode. Handles both
    // the BinaryExpr form (PLUS) and the AugAssign form (PLUS_EQUAL). Returns -1
    // when the token is not a box-arithmetic operator.
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

    // Box a native arithmetic operand for dragon_box_binop. A numeric LLVM type
    // maps directly to its value-tag; a pointer operand's tag comes from the
    // expr's static type (str/list/bytes/...). A value already of box type is
    // returned unchanged. Borrow semantics - the helpers read, never own.
    llvm::Value* boxNativeOperand(CodeGen& cg, Expr* e, llvm::Value* v) {
        if (v->getType() == boxType) return v;
        llvm::Value* tag;
        if (v->getType() == i64Type)
            tag = llvm::ConstantInt::get(i64Type, 0);   // TAG_INT
        else if (v->getType() == f64Type)
            tag = llvm::ConstantInt::get(i64Type, 2);   // TAG_FLOAT
        else if (v->getType() == i1Type)
            tag = llvm::ConstantInt::get(i64Type, 3);   // TAG_BOOL
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

    // boxNativeOperand borrows - the box only reads the payload. A native ptr
    // operand that is itself an owned temp (fresh call result / bytes literal)
    // is orphaned once the runtime call returns, so drain it here. Names and
    // field reads are loads, not calls - isOwnedPtrResult screens them out.
    void drainOwnedNativeBoxOperands(llvm::Value* lhs, llvm::Value* rhs) {
        if (options.gcMode != GCMode::RC) return;
        for (llvm::Value* v : {lhs, rhs}) {
            if (v->getType() != boxType && v->getType()->isPointerTy() &&
                isOwnedPtrResult(v))
                builder->CreateCall(runtimeFuncs["dragon_decref"], {v});
        }
    }

    // Emit dragon_box_cmp(boxA, boxB, cmpOp) for an ordering operator where at
    // least one operand is a box. Returns the three-way i64 result (<0/0/>0);
    // the caller compares it to 0. cmpOp (0=< 1=<= 2=> 3=>=) is only used for
    // the TypeError message on incomparable operands.
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

    // Unbox a dragon_box_binop result into `targetType` (a native slot type),
    // emitting a runtime tag-check that raises TypeError (code 80) on mismatch.
    // Mirrors the D039 Phase-7a inline unbox in AnnAssign. If targetType IS the
    // box type, returns the box unchanged (Any slot). Leaves the builder at the
    // ok-path continuation block.
    // `wantListElemTag` (when not kNoListElemCheck) additionally emits
    // dragon_list_view_check on a list-tagged payload: the box tag alone says
    // "some list" but cannot distinguish a monomorphized DragonList from a
    // DragonListBox - reading one at the other's stride corrupts silently.
    // Pass -1 for a list[Any]/list[union] target (requires a box list), an
    // element tag >= 0 for a concrete list[T] target.
    llvm::Value* unboxBoxResultChecked(llvm::Value* box, llvm::Type* targetType,
                                       VarKind vk,
                                       int64_t wantListElemTag = kNoListElemCheck);

    /// The dragon_list_view_check argument for a list-typed slot, derived from
    /// its annotation: -1 for list[Any] / list[union] (box representation), a
    /// concrete element tag for monomorphized element types, kNoListElemCheck
    /// when the annotation is not a checkable list shape (bare `list`,
    /// `list[type]` descriptor lists, type variables, ...).
    int64_t listViewWantElemTag(TypeExpr* ann);

    /// Convert a raw i64 container slot (as returned by dragon_tuple_get /
    /// dragon_list_get) to the native LLVM value for `elemType`. Mirrors the
    /// tuple-unpack coercion in Assign.cpp: float bits -> f64, 0/≠0 -> i1, any
    /// heap/ptr kind -> i8*, else the i64 stays. Used by C9-B `*tuple` / `*list`
    /// spread to feed container elements into typed parameter slots.
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

    // Emit the runtime tag value for an expression being passed as a union arg.
    // Returns an i64 constant for known types, or extracts the tag from a
    // boxed union variable's stored {tag, payload} value.
    // Compute the {tag, payload} pair for an `Any`/box value passed to a
    // box-list op (insert/remove/append). `takesOwnership` increfs borrowed
    // heap payloads (Model B - for ops that keep the value); false = the value
    // is only inspected (e.g. remove's value-equality search).
    std::pair<llvm::Value*, llvm::Value*> boxArgTagPayload(
            Expr* argExpr, llvm::Value* val, bool takesOwnership);

    llvm::Value* emitTagForExpr(Expr* expr, CodeGen& cg);

    // Convert Dragon type annotation to LLVM type
    llvm::Type* typeExprToLLVM(TypeExpr* typeExpr);

    // Coerce a function-call argument to match the expected parameter type.
    // D030 Phase 5 audit:
    //  Legitimate widening: int<->float, bool<->int/float, intc bridges (FFI).
    //  ptr<->int: KEPT because class-field type inference still stores some
    //  ptr-returning RHS values (e.g. `self.handle = fopen(...)`) as i64
    //  fields. The proper fix is upstream - class field types should be
    //  inferred as ptr when the RHS is ptr-typed. Tracked as a follow-up
    //  to D030 Phase 5; deleting these here today regresses io / re /
    //  sqlite / threading interop tests.
    /// D030 Phase 4 call-boundary: when a concrete-typed value crosses into
    /// an `Any` / `Union[...]` parameter slot (paramType == boxType), the
    /// compiler MUST emit a `%dragon.box = { i64 tag, i64 payload }` here.
    /// This is the inverse of D039 Phase 7a's box-to-native unbox at the
    /// store site - both directions are first-class boundary handling, not
    /// fallbacks.
    ///
    /// The AST is required to derive the right TAG_* for ptr-shaped values
    /// (str vs list vs dict vs class etc.) - `emitTagForExpr` already does
    /// that lookup. If the param isn't box-shaped, defer to the regular
    /// `coerceArg` (i64/i1/f64/ptr/intc widenings, etc.).
    llvm::Value* coerceArgFromExpr(Expr* expr,
                                    llvm::Value* arg,
                                    llvm::Type* paramType) {
        if (paramType == boxType) {
            // Box an arg into an Any/Union parameter through the single shared
            // boxing path. takesOwnership=FALSE: an Any param is BORROWED today
            // (the box does not take its own +1), matching the long-standing
            // behavior. Completing the "donate" contract (incref a borrowed
            // source here, then have the callee free a non-escaping Any param at
            // scope exit) is deferred - it additionally needs an ownership-FLOW
            // analysis for the free-point that the stack-allocation escape pass
            // can't provide. Flipping this to true is
            // the caller half of that future work.
            auto tp = boxArgTagPayload(expr, arg, /*takesOwnership=*/false);
            return makeBox(tp.first, tp.second);
        }
        return coerceArg(arg, paramType);
    }

    /// Same tag derivation as `emitTagForExpr` but without the unused
    /// CodeGen& parameter so we can call it from `coerceArgFromExpr` (which
    /// doesn't have a CodeGen reference). The original `emitTagForExpr`
    /// stays in place - call sites that already pass `cg` shouldn't change.
    llvm::Value* emitTagForExprNoCG(Expr* expr);

    llvm::Value* coerceArg(llvm::Value* arg, llvm::Type* paramType);

    // Normalize a value to i64 if it's intc - used after function calls
    // so that the rest of Dragon codegen always works with i64 integers.
    llvm::Value* normalizeIntC(llvm::Value* val) {
        if (val->getType() == intcType)
            return builder->CreateSExt(val, i64Type, "intc_ext");
        return val;
    }

    //===------------------------------------------------------------------===//
    // D030 - Per-callsite spawn trampolines (fire / async / generator)
    //===------------------------------------------------------------------===//

    /// Cast a return value of arbitrary type to i64 for transit through the
    /// vthread result slot. Mirrors the inverse of coerceArg's widenings.
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

    // Inverse of resultToI64: reinterpret the i64 result slot from
    // dragon_vthread_join back to the task's native result type T (D030).
    // A float result was bit-PACKED by resultToI64 (CreateBitCast), so we
    // bitcast back - NOT coerceArg, which would SIToFP-CONVERT and corrupt
    // the bits. ptr-shaped Ts inttoptr; bool truncates.
    // Union/Any results are a 16-byte box that cannot fit the i64 slot - a
    // pre-existing vthread-ABI boundary; left as raw i64 here (not worsened).
    llvm::Value* taskResultFromI64(llvm::Value* rawI64, Type* resultType);

    /// Build the per-callsite typed args struct type:
    ///  { ptr handle, <native_arg_types...> }
    /// Field 0 is reserved for the runtime to patch (DragonVThread* or
    /// DragonGenerator*) so the trampoline can address its result/self slot.
    /// Subsequent fields are the user args at native LLVM types.
    llvm::StructType* makeSpawnArgsStructType(
        const std::vector<llvm::Type*>& argTypes,
        const std::string& name) {
        std::vector<llvm::Type*> fields;
        fields.push_back(i8PtrType);  // field 0: handle (vthread or generator)
        for (auto* t : argTypes) fields.push_back(t);
        return llvm::StructType::create(*context, fields, name);
    }

    /// Coerce a value loaded from / stored to a struct field whose type
    /// matches a native LLVM type. Used at spawn-site populate where the
    /// caller's value type may not exactly match the field type.
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

    /// Build a per-callsite fire/async trampoline that:
    ///  1. Pulls the args struct out of the coroutine's user_data
    ///  2. Loads the vthread handle (field 0) and each native arg (fields 1..N)
    ///  3. Calls the target function with native types
    ///  4. dragon_vthread_set_result(vt, i64-coerced result)
    ///  5. Atomically decrefs heap-typed args (balances spawn-site incref)
    ///  6. free(args buffer) and ret void
    /// Caller is responsible for atomic-increfing heap args BEFORE spawn.
    /// targetFn may be a regular Dragon function or a method.
    llvm::Function* buildFireTrampoline(
        llvm::Function* targetFn,
        llvm::StructType* argsStructType,
        const std::vector<VarKind>& argKinds,
        const std::string& siteName);

    /// Build a per-defer-site thunk `void __dragon_defer_<site>(i64* args)`
    /// that loads targetFn's arguments from the i64 snapshot array (inttoptr /
    /// trunc / bitcast per param type), calls it, and discards the result.
    /// One thunk serves BOTH exit paths: emitScopeCleanupFor calls it inline
    /// on normal exits, and dragon_exc_cleanup_unwind calls it through the
    /// DCLEAN_DEFER_CALL entry during a longjmp unwind. Defined next to
    /// buildFireTrampoline in ImplMethods2.cpp.
    /// vtableIndex >= 0 dispatches the call through the receiver's vtable
    /// (D026 parity: an overridden method deferred on a base-typed receiver
    /// must reach the subclass override, exactly like the direct call).
    llvm::Function* buildDeferThunk(llvm::Function* targetFn,
                                    const std::string& siteName,
                                    int vtableIndex = -1);

    /// Build a per-callsite generator trampoline that:
    ///  1. Pulls the args struct out of user_data
    ///  2. Loads the generator handle (field 0) and each native arg (fields 1..N)
    ///  3. Calls the generator body fn with (gen_ptr, native_args...)
    ///  4. dragon_generator_set_exhausted(gen) and ret void
    /// Args buffer is owned by the generator and freed at destroy via the
    /// separately-built decref fn (see buildGeneratorDecrefFn).
    llvm::Function* buildGeneratorTrampoline(
        llvm::Function* bodyFn,
        llvm::StructType* argsStructType,
        const std::string& siteName);

    /// Build a per-callsite decref function for a generator's args buffer.
    /// Called by dragon_generator_destroy. Walks heap-typed arg slots and
    /// atomic-decrefs each (atomic because destroy can race with the worker
    /// thread, e.g. last decref from another vthread).
    /// Returns NULL if no heap args (caller passes NULL to create_typed).
    llvm::Function* buildGeneratorDecrefFn(
        llvm::StructType* argsStructType,
        const std::vector<VarKind>& argKinds,
        const std::string& siteName);

    /// Populate a stack-allocated spawn args struct with user args.
    /// Field 0 is left zero (runtime patches it to the vthread/generator).
    /// User args are stored at fields 1..N at their native types.
    void populateSpawnArgs(
        llvm::Value* argsAlloca,
        llvm::StructType* argsStructType,
        const std::vector<llvm::Value*>& userArgs);

    //===------------------------------------------------------------------===//
    // D030 Phase 3.B - Bind a list element to a native-typed alloca
    //===------------------------------------------------------------------===//

    /// Issue the matching typed get for the loop variable's kind and bind to
    /// a fresh alloca of the native LLVM type. Used by for-loops and
    /// comprehensions over typed lists. Caller is responsible for setVar
    /// (VarKind tracking), borrowed-insertion, and varClassNames bookkeeping.
    ///
    ///  Float -> dragon_list_get_f64 -> double, stored in f64 alloca
    ///  Str / Bytes / List / Dict / Tuple / Set / ClassInstance
    ///  -> dragon_list_get_ptr -> ptr, stored in i8* alloca
    ///  Bool -> dragon_list_get (1-byte packing path), truncated to i1
    ///  else -> dragon_list_get -> i64, stored in i64 alloca (Int / Type / unknown)
    llvm::AllocaInst* bindListElemTyped(
        llvm::Function* func,
        llvm::Value* listVal,
        llvm::Value* idx,
        const std::string& varName,
        VarKind loopKind);

    /// D030 §5 - Type::Kind-driven loop-var binder. Sizes the alloca and
    /// picks the matching typed `dragon_list_get_*` runtime call directly
    /// from the iterable's element Type::Kind, bypassing the legacy
    /// VarKind hop. The single source of truth for "what shape does a
    /// loop var of T-typed list[T] have at the LLVM level."
    llvm::AllocaInst* bindListElemByTypeKind(
        llvm::Function* func,
        llvm::Value* listVal,
        llvm::Value* idx,
        const std::string& varName,
        Type::Kind elemKind);

    // Emit a string-literal byte sequence as an LLVM i8* pointer.
    //
    // For pure-ASCII bytes we keep the existing fast path: a global
    // C-string, zero per-use cost, byte-count == cp-count, full C-FFI
    // compatibility. The runtime treats it as a borrowed kind=1 buffer.
    //
    // For sequences containing any byte >= 0x80 we *cannot* publish a raw
    // C-string - `dragon_str_concat` and friends interpret a kind=1 input
    // (or a literal pointer) as one byte == one cp, which silently turns
    // every multi-byte UTF-8 sequence into Latin-1 code points and then
    // re-encodes them, producing the dreaded double-encoded "Ã¢â‚¬â€".
    // Instead we register one module-level i8* slot per distinct byte
    // sequence; main()'s preamble calls `dragon_str_intern` exactly once
    // (immortal heap DragonString, decoded to its canonical kind=1/4),
    // and use sites just emit a load.
    //
    // This is the single point of truth for "how do we lower a literal
    // byte sequence into an LLVM value" - every place that wants to emit
    // text inline (StringLiteral, TemplateExpr literal segments, f-string
    // literal segments) routes through here so non-ASCII text in any of
    // them survives concatenation byte-accurate.
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

    // When true, visit(ClassDecl) registers field-layout metadata only and
    // returns before emitting any bodies/globals - the layout pre-pass that runs
    // for all classes before any method body, so cross-class field references to
    // later-defined classes resolve correctly.
    bool classLayoutPass = false;

    // Forward-declare class constructors and methods in a module.
    void forwardDeclareClasses(dragon::Module& mod); // defined in codegen/ImplInit.cpp
    // 6.18: AST-level synthesis of __init__ / __eq__ / __repr__ for
    // @dataclass-decorated classes and NamedTuple subclasses. Defined in
    // codegen/Classes.cpp. Mutates the class body in place.
    void synthesizeDataclassMethods(ClassDecl& node);

    // AST-level synthesis for class-based enums (`class C(Enum)`): rewrites
    // members into singleton static instances with name/value fields, __init__,
    // __str__/__repr__, a __members__ list, and a value-lookup helper. Defined
    // in codegen/Classes.cpp. Mutates the class body in place. Must run before
    // synthesizeDataclassMethods sees the (now non-enum) class.
    void synthesizeEnumMethods(ClassDecl& node);
};

} // namespace dragon

#endif // DRAGON_CODEGEN_IMPL_H
