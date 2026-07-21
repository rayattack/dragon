/// Dragon CodeGen - Call Expression Dispatch
/// Routes to emitBuiltinCall (CallBuiltins.cpp) and emitMethodCall (CallMethods.cpp).
/// Contains: constructor delegation, class constructors, TypedDict constructors,
///  decorator dispatch, D025 dynamic constructors, __call__ dunder,
///  indirect/closure calls, stdlib symbol aliases, "Unknown function" error.
// XXX: why does the tagged-dict ctor path need a separate branch here
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(CallExpr& node) {
    // C9-B call-site spread (`*tuple` / `*list` / `**dict`) routing. TypedDict
    // construction `T(**row)` is lowered specially below (a dict copy / tagged
    // set, not a normal call), and method spread `obj.m(*xs)` is handled inside
    // emitMethodCall - both fall through this guard. Every other spread shape
    // routes through emitSpreadDispatch, which resolves the callee and emits the
    // expanded call (or returns false -> diagnosed here). Non-spread calls skip
    // this block entirely, so their IR is byte-identical to before.
    if (callHasSpread(node)) {
        bool isTypedDict = false;
        if (auto* c = dynamic_cast<NameExpr*>(node.callee.get()))
            isTypedDict = impl_->typedDictClasses.count(c->name) > 0;
        bool isMethodCall = false;
        if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get()))
            isMethodCall = !(attr->object && attr->object->type &&
                             attr->object->type->kind() == Type::Kind::Module);
        if (!isTypedDict) {
            // dict(**a, **b) / dict(x=1, **d) - construct a dict, exactly like the
            // `{...}` literal (an honest copy; Python parity). Desugar to a
            // DictExpr so the literal's value-dispatch + spread-merge codegen is
            // reused verbatim instead of duplicated. (No positional args / `*`
            // star args - those are a different dict() form, left to fall through.)
            // Callee is bare `dict` or the subscripted `dict[K, V]` form - the
            // monomorphizer substitutes a `T(**row)` type parameter to the
            // subscript shape `dict[str, Any](**row)`, so both must be recognized.
            NameExpr* dc = dynamic_cast<NameExpr*>(node.callee.get());
            if (!dc)
                if (auto* sub = dynamic_cast<SubscriptExpr*>(node.callee.get()))
                    dc = dynamic_cast<NameExpr*>(sub->object.get());
            {
                if (dc && dc->name == "dict" && node.args.empty() && !node.kwArgs.empty()) {
                    DictExpr synth;
                    synth.setLocation(node.location());
                    for (auto& kw : node.kwArgs) {
                        if (kw.first.empty()) {
                            // `**src` spread entry (empty-name kwArg sentinel)
                            synth.entries.emplace_back(nullptr, std::move(kw.second));
                        } else {
                            auto keyLit = std::make_unique<StringLiteral>();
                            keyLit->value = kw.first;
                            keyLit->setLocation(node.location());
                            synth.entries.emplace_back(std::move(keyLit),
                                                       std::move(kw.second));
                        }
                    }
                    visit(synth);
                    return;
                }
            }
            if (isMethodCall) {
                if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get()))
                    if (emitMethodCall(node, *attr)) return;
            } else if (emitSpreadDispatch(node)) {
                return;
            }
            // Unsupported spread target (decorated fn/class, closure variable,
            // dynamic ctor, builtin, C-alias, __call__) - diagnose rather than
            // miscompile (#2). The implemented paths above already returned.
            impl_->addError(
                "call-site spread (`*` / `**`) into this callable is not "
                "supported", node.location());
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return;
        }
        // isTypedDict: fall through to the TypedDict ctor path below.
    }

    if (auto* callee = dynamic_cast<NameExpr*>(node.callee.get())) {
        const std::string& name = callee->name;

        // .dr keyword-form `super(args)`: delegate to the parent's constructor.
        // The parent ctor is a nameless `def()`, so calling the parent reference
        // *is* calling its ctor - exactly parallel to `Animal(args)`. This is the
        // canonical .dr spelling; `super().__init__(...)` is rejected in .dr (see
        // emitMethodCall). In .py mode `super()` keeps Python's proxy semantics,
        // handled by emitBuiltinCall below, so this branch is .dr-only. Delegation
        // is opt-in: if a child ctor never calls super(...), the parent ctor does
        // not run (Python parity - no implicit base-ctor chaining).
        if (name == "super" && impl_->isDragonFile) {
            if (impl_->currentClassName.empty()) {
                impl_->addError("super(...) is only valid inside a method of a "
                    "class with a parent", node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }
            auto parentIt = impl_->classParentNames.find(impl_->currentClassName);
            if (parentIt == impl_->classParentNames.end()) {
                impl_->addError("super(...): class '" + impl_->currentClassName +
                    "' has no parent class to delegate to", node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }
            const std::string& parentName = parentIt->second;
            std::string initName = impl_->classSymPrefix(parentName) + "___init__";
            auto* initFunc = impl_->module->getFunction(initName);
            if (!initFunc) {
                // Built-in exception parents (Exception, ValueError, ...) have
                // no user-defined __init__ to delegate to. Python parity treats
                // `super().__init__(msg)` here as storing msg on the instance
                // for `str(e)` purposes; in Dragon the runtime msg slot is set
                // at the *raise* site (RaiseStmt's obj-raise path snapshots
                // the first ctor arg), so the call here can be a no-op. Still
                // evaluate the args for any visible side effects, then return.
                // Any other missing parent ctor is a real error.
                if (impl_->isBuiltinExcName(parentName)) {
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                    }
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                    return;
                }
                impl_->addError("super(...): no constructor found for parent class '" +
                    parentName + "'", node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }
            // Pass the current self (param 0 of the enclosing method) plus args.
            llvm::Value* selfVal = &*impl_->currentFunction->arg_begin();
            std::vector<llvm::Value*> args = {selfVal};
            auto initFuncType = initFunc->getFunctionType();
            for (size_t i = 0; i < node.args.size(); ++i) {
                node.args[i]->accept(*this);
                llvm::Value* arg = impl_->lastValue;
                if (i + 1 < initFuncType->getNumParams())
                    arg = impl_->coerceArg(arg, initFuncType->getParamType(i + 1));
                args.push_back(arg);
            }
            impl_->builder->CreateCall(initFunc, args);
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return;
        }

        // User-defined bindings shadow builtins (Python parity). Skip the
        // builtin dispatcher if the name resolves to a binding visible in
        // the CURRENT module's lexical scope:
        //  1. An import alias scoped to this module (`from X import open`
        //  -> importedFuncAliasesByModule[currentModule]["open"]).
        //  2. A local / parameter binding (alloca-backed).
        //  3. A top-level module global (`open: Callable = ...`).
        //  4. A same-module non-extern Dragon `def`. Same-module means the
        //  symbol exists at `mangleFunc(currentModuleName, name)`. Extern-
        //  "C" decls from OTHER modules also occupy bare LLVM symbol
        //  names (math.h's `pow`, libc's `open`) but are NOT in this
        //  module's scope unless explicitly imported - gated through
        //  `externFuncNames` so a dep module's `extern "C" def pow`
        //  doesn't accidentally shadow the builtin `pow` here.
        // Without this gate, `from webfake import open; open(url)` falls
        // into `emitBuiltinCall("open")` which dispatches to
        // `dragon_file_open` - silently calling the wrong function with a
        // string arg.
        bool nameIsUserBound = false;
        {
            std::string aliasSym = impl_->lookupImportedAlias(name);
            if (!aliasSym.empty() && impl_->module &&
                impl_->module->getFunction(aliasSym)) {
                nameIsUserBound = true;
            }
            if (!nameIsUserBound && impl_->lookupVar(name))
                nameIsUserBound = true;
            if (!nameIsUserBound && impl_->lookupModuleGlobal(name))
                nameIsUserBound = true;
            if (!nameIsUserBound && impl_->module) {
                std::string mangled =
                    Impl::mangleFunc(impl_->currentModuleName, name);
                if (impl_->module->getFunction(mangled) &&
                    !impl_->externFuncNames.count(mangled)) {
                    nameIsUserBound = true;
                }
            }
        }

        // Builtin function dispatch (print, len, abs, int, float, str, etc.)
        if (!nameIsUserBound && emitBuiltinCall(node, name)) return;

        // Constructor delegation: ParentClass(args...) inside a child class method
        // calls ParentClass___init__(self, args...) instead of allocating a new instance.
        if (impl_->classNames.count(name) && !impl_->currentClassName.empty()) {
            auto parentIt = impl_->classParentNames.find(impl_->currentClassName);
            if (parentIt != impl_->classParentNames.end() && parentIt->second == name) {
                // Delegate to parent's __init__ with current self. Resolve the
                // parent's owning module for the LLVM symbol - same shape as
                // the function-mangling cross-module path.
                std::string initName = impl_->classSymPrefix(name) + "___init__";
                auto* initFunc = impl_->module->getFunction(initName);
                if (initFunc) {
                    // Load self from the first parameter of the current function
                    llvm::Value* selfVal = &*impl_->currentFunction->arg_begin();
                    std::vector<llvm::Value*> args = {selfVal};
                    auto initFuncType = initFunc->getFunctionType();
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        if (i + 1 < initFuncType->getNumParams())
                            arg = impl_->coerceArg(arg, initFuncType->getParamType(i + 1));
                        args.push_back(arg);
                    }
                    impl_->builder->CreateCall(initFunc, args);
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                    return;
                }
            }
        }

        // TypedDict constructor: ClassName({...}) -> create dict with tagged values
        if (impl_->typedDictClasses.count(name)) {
            // TypedDict(dict_literal) - the single arg should be a DictExpr
            // Just evaluate the dict literal (which already emits tagged set),
            // then mark the result variable as this TypedDict class.
            if (node.args.size() == 1) {
                node.args[0]->accept(*this);
                // lastValue is now the dict pointer
                // The caller (AnnAssignStmt/AssignStmt) will store it and we track
                // the TypedDict class name via varTypedDictClass in those visitors.
                return;
            }
            // TypedDict() with keyword args: ClassName(name="Jon", age=10)
            // Build a new dict and set each kwarg as a tagged entry
            if (!node.kwArgs.empty()) {
                // #1 fast path: `Customer(**row)` - a single ** spread and
                // nothing else (the dominant form, and the D032 gate). Lower to
                // one pre-sized dragon_dict_copy: a single bulk copy, no
                // dragon_dict_new + per-entry insert/rehash. Python **-unpack
                // copy semantics (the result owns its own +1 refs).
                if (node.kwArgs.size() == 1 && node.kwArgs[0].first.empty()) {
                    node.kwArgs[0].second->accept(*this);
                    llvm::Value* src = impl_->lastValue;
                    if (!src->getType()->isPointerTy())
                        src = impl_->builder->CreateIntToPtr(src, impl_->i8PtrType);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_copy"], {src}, "td");
                    return;
                }
                // General path: fresh dict; merge each ** spread, set each named
                // kwarg by schema tag. Handles Customer(**a, **b, x=1).
                auto* cap = llvm::ConstantInt::get(impl_->i64Type, node.kwArgs.size());
                llvm::Value* dict = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_new"], {cap}, "td");
                for (auto& [kwName, kwVal] : node.kwArgs) {
                    kwVal->accept(*this);
                    llvm::Value* val = impl_->lastValue;
                    // `**spread` (empty-name sentinel): merge the source dict's
                    // entries - with their existing tags - into the fresh
                    // TypedDict dict. This is how `Customer(**row)` builds an
                    // owned TypedDict from a runtime dict (Python's **-unpack
                    // copy semantics; the new dict owns its own +1 refs).
                    if (kwName.empty()) {
                        if (!val->getType()->isPointerTy())
                            val = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_dict_update"], {dict, val});
                        continue;
                    }
                    // Determine tag from the TypedDict schema
                    int64_t tag = 0; // TAG_INT default
                    auto schemaIt = impl_->typedDictFieldKinds.find(name);
                    if (schemaIt != impl_->typedDictFieldKinds.end()) {
                        auto fIt = schemaIt->second.find(kwName);
                        if (fIt != schemaIt->second.end())
                            tag = Impl::typeKindToTag(fIt->second);
                        if (tag < 0) tag = 0;
                    }
                    // Coerce value to i64
                    if (val->getType() == impl_->i1Type)
                        val = impl_->builder->CreateZExt(val, impl_->i64Type);
                    else if (val->getType() == impl_->f64Type)
                        val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                    else if (val->getType()->isPointerTy())
                        val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                    auto* keyStr = impl_->builder->CreateGlobalString(kwName);
                    auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_tagged"],
                        {dict, keyStr, val, tagVal});
                }
                impl_->lastValue = dict;
                return;
            }
            // No args: empty TypedDict
            auto* cap = llvm::ConstantInt::get(impl_->i64Type, 0);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_new"], {cap}, "td");
            return;
        }

        // Class-based enum value lookup: `Color(v)` returns the member whose
        // .value == v (NOT a fresh construction). Redirect to the synthesized
        // `Color._lookup(v)` static method (see synthesizeEnumMethods). The
        // internal 2-arg ctor `Color(name, value)` that builds the singletons
        // has arity 2, so it is not intercepted here.
        if (impl_->enumKind.count(name) && node.args.size() == 1) {
            auto call = std::make_unique<CallExpr>();
            auto attr = std::make_unique<AttributeExpr>();
            auto obj = std::make_unique<NameExpr>(); obj->name = name; obj->setLocation(node.location());
            attr->object = std::move(obj);
            attr->attribute = "_lookup";
            attr->setLocation(node.location());
            call->callee = std::move(attr);
            call->args.push_back(std::move(node.args[0]));
            call->setLocation(node.location());
            call->accept(*this);
            return;
        }

        // Class constructor call: ClassName(args...) -> ClassName_new(args...)
        //
        // ADR 025 removal: classes are compile-time entities (D021 /
        // commandment #3). Construction is ALWAYS resolved statically from a
        // literal class name - there are no class values, no aliases, and no
        // runtime descriptor construction. A non-class callee (e.g. a
        // `VarKind::Type` variable) falls through to the error below.
        const std::string ctorClassName =
            impl_->classNames.count(name) ? name : std::string();
        if (impl_->classNames.count(ctorClassName)) {
            // Class decorators (`@dec class C`) are dropped:
            // a decorated class would require runtime descriptor construction,
            // which ADR 025 removed. Function decorators, @dataclass,
            // @staticmethod, @classmethod, @property and NamedTuple are
            // unaffected (compile-time synthesis, handled elsewhere).
            if (impl_->decoratedClasses.count(ctorClassName)) {
                impl_->addError(
                    "class decorators are not supported: classes are "
                    "compile-time entities and cannot be wrapped at runtime "
                    "(class '" + ctorClassName + "')",
                    node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }

            // Rebind so the rest of this block constructs the resolved class
            // (handles both `Dog(...)` and `X(...)` where `X = Dog`).
            const std::string& name = ctorClassName;
            (void)name;

            // B Phase 1: stack-construct a non-escaping instance of an
            // eligible scalar-only class. computeStackAllocSites proved this
            // ctor's result is bound to a local that never escapes its block;
            // stackEligibleClasses confirms the class is scalar-only with a
            // single non-self-escaping ctor and no per-instance defaults. We
            // allocate the struct in the entry block (hoisted, reused across
            // loop iterations), memset it (matching _new's zero-init), set an
            // immortal refcount as defense against any stray decref, then call
            // __init__ directly - NO malloc, NO gc_track, NO decref. LLVM then
            // SROAs the alloca away entirely (see ADR / versus benchmark).
            if (impl_->stackAllocSites.count(&node) &&
                impl_->stackEligibleClasses.count(name)) {
                auto stIt = impl_->classStructTypes.find(name);
                auto* initFn = impl_->module->getFunction(
                    impl_->classSymPrefix(name) + "___init__");
                if (stIt != impl_->classStructTypes.end() && initFn) {
                    llvm::StructType* structType = stIt->second;
                    auto* self = impl_->createEntryAlloca(
                        impl_->currentFunction, name + ".stack", structType);

                    uint64_t structSize =
                        impl_->module->getDataLayout().getTypeAllocSize(structType);
                    auto* sizeVal = llvm::ConstantInt::get(impl_->i64Type, structSize);
                    auto* memsetFunc = impl_->module->getFunction("memset");
                    if (!memsetFunc) {
                        auto* memsetType = llvm::FunctionType::get(impl_->i8PtrType,
                            {impl_->i8PtrType, llvm::Type::getInt32Ty(*impl_->context),
                             impl_->i64Type}, false);
                        memsetFunc = llvm::Function::Create(memsetType,
                            llvm::Function::ExternalLinkage, "memset", impl_->module.get());
                    }
                    impl_->builder->CreateCall(memsetFunc,
                        {self, llvm::ConstantInt::get(
                                   llvm::Type::getInt32Ty(*impl_->context), 0), sizeVal});
                    // Immortal refcount (header index 0): a stray dragon_decref
                    // - which should never be emitted, since scope cleanup skips
                    // stack instances - becomes a guaranteed no-op rather than a
                    // free() of stack memory. type_tag/vtable stay zeroed; they
                    // are only read on paths (method dispatch, decref, gc) that a
                    // non-escaping instance never reaches.
                    auto* rcGEP = impl_->builder->CreateStructGEP(structType, self, 0, "rc_ptr");
                    impl_->builder->CreateStore(
                        llvm::ConstantInt::get(impl_->i64Type, 0x4000000000000000LL), rcGEP);

                    // Evaluate ctor args, coerce to __init__'s param types
                    // (param 0 is self), then call __init__ on the stack slot.
                    auto* initTy = initFn->getFunctionType();
                    std::vector<llvm::Value*> initArgs = {self};
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        unsigned pidx = (unsigned)(i + 1);
                        if (pidx < initTy->getNumParams())
                            arg = impl_->coerceArg(arg, initTy->getParamType(pidx));
                        initArgs.push_back(arg);
                    }
                    impl_->builder->CreateCall(initFn, initArgs);
                    impl_->lastValue = self;
                    impl_->lastWasStackInstance = true;
                    return;
                }
            }

            // Determine which _new function to call.
            // Multi-constructor classes: dispatch by arity to ClassName_new_N.
            // Single-constructor classes: use ClassName_new (no suffix).
            // The LLVM symbol is mangled per the class's owning module so two
            // modules with same-named classes resolve to distinct bodies.
            const std::string ctorPrefix = impl_->classSymPrefix(name);
            std::string ctorName;
            auto ctorCountIt = impl_->classCtorCount.find(name);
            if (ctorCountIt != impl_->classCtorCount.end() && ctorCountIt->second > 1) {
                // Multi-constructor: match call arity against classCtorArities
                size_t callArity = node.args.size();
                auto& arityVec = impl_->classCtorArities[name];
                int matchedIdx = -1;
                for (auto& [arity, idx] : arityVec) {
                    if (arity == callArity) { matchedIdx = idx; break; }
                }
                if (matchedIdx >= 0) {
                    ctorName = ctorPrefix + "_new_" + std::to_string(matchedIdx);
                } else {
                    // No matching arity found -- fall back to first overload
                    // (type checker should have caught this)
                    ctorName = ctorPrefix + "_new_" + std::to_string(arityVec[0].second);
                }
            } else {
                ctorName = ctorPrefix + "_new";
            }

            auto* ctorFunc = impl_->module->getFunction(ctorName);
            if (ctorFunc) {
                // Descriptor backstop (fire-own-fwdref-hang.md): every ctor's
                // param kinds/own flags are registered in forwardDeclareClasses
                // before any body is lowered, so a missing entry here means
                // this construction site is being lowered against a half-built
                // class descriptor. Emitting anyway would route `own` args
                // through the erased-generics drain fallback below and
                // double-free the adopted +1. Fail the compile loudly instead.
                if (impl_->options.gcMode == GCMode::RC && !node.args.empty() &&
                    !impl_->funcParamKinds.count(ctorName)) {
                    impl_->addError(
                        "internal: cannot construct '" + name +
                        "' here: its constructor descriptor is not finalized",
                        node.location());
                    return;
                }
                std::vector<llvm::Value*> args;
                // Owned heap-temporary args to release after the call (the
                // ctor borrows; `self.f = param` increfs what it retains).
                std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                auto pkIt = impl_->funcParamKinds.find(ctorName);
                auto ctorFuncType = ctorFunc->getFunctionType();
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    // An own param ADOPTS the arg's +1 (moved binding or
                    // fresh temp): the callee releases it, so a caller-side
                    // drain here would double-free (A/B-proven).
                    bool argDrained = impl_->paramIsOwn(ctorName, (unsigned)i);
                    if (!argDrained &&
                        pkIt != impl_->funcParamKinds.end() && i < pkIt->second.size()) {
                        Impl::VarKind dk = impl_->argTempDecrefKind(
                            node.args[i].get(), pkIt->second[i], arg);
                        if (dk != Impl::VarKind::Other) {
                            argTemps.emplace_back(arg, dk);
                            argDrained = true;
                        }
                    }
                    if (!argDrained) {
                        // A MONOMORPHIZED GENERIC ctor (Shelter[Dog](Dog(...)))
                        // records its erased-T param as a non-heap kind, so the
                        // funcParamKinds drain above bails and the owned arg
                        // temp leaked one instance per call. Same fallback as
                        // the instance-method arg loops: provably-owned boxes
                        // by value, native temps via the expression-gated
                        // ownedTempDrainKind (borrowed getters rejected before
                        // isOwnedStrResult is consulted).
                        if (arg->getType() == impl_->boxType) {
                            if (impl_->isOwnedBoxResult(arg))
                                argTemps.emplace_back(arg, Impl::VarKind::Union);
                        } else {
                            Impl::VarKind dk = impl_->ownedTempDrainKind(
                                node.args[i].get(), arg);
                            if (dk != Impl::VarKind::Other)
                                argTemps.emplace_back(arg, dk);
                        }
                    }
                    // Use coerceArgFromExpr so a concrete-typed arg crossing
                    // into an `Any`/Union ctor param is wrapped in a box with
                    // the right tag (e.g. addbase("hello") where def(v: Any)).
                    // coerceArg alone would let the ptr fall through unchanged,
                    // failing the LLVM verifier at the call site.
                    if (i < ctorFuncType->getNumParams())
                        arg = impl_->coerceArgFromExpr(
                            node.args[i].get(), arg, ctorFuncType->getParamType(i));
                    args.push_back(arg);
                }
                // D040: bind ctor keyword arguments to named parameter
                // positions. Without this, Config(timeout=30) silently kept
                // the default (the canonical broken case the ADR fixes).
                if (!node.kwArgs.empty()) {
                    auto pnIt = impl_->funcParamNames.find(ctorName);
                    if (pnIt != impl_->funcParamNames.end()) {
                        const auto& paramNames = pnIt->second;
                        size_t numParams = ctorFuncType->getNumParams();
                        if (args.size() < numParams)
                            args.resize(numParams, nullptr);
                        for (auto& [kwName, kwVal] : node.kwArgs) {
                            auto nameIt = std::find(paramNames.begin(),
                                                    paramNames.end(), kwName);
                            if (nameIt == paramNames.end()) {
                                impl_->addError(
                                    "class '" + name +
                                    "' constructor got an unexpected "
                                    "keyword argument '" + kwName + "'",
                                    node.location());
                                return;
                            }
                            size_t idx = (size_t)std::distance(
                                paramNames.begin(), nameIt);
                            if (idx >= numParams) {
                                impl_->addError(
                                    "keyword argument '" + kwName +
                                    "' out of range for ctor '" + name + "'",
                                    node.location());
                                return;
                            }
                            if (args[idx] != nullptr) {
                                impl_->addError(
                                    "class '" + name +
                                    "' constructor got multiple values "
                                    "for argument '" + kwName + "'",
                                    node.location());
                                return;
                            }
                            kwVal->accept(*this);
                            llvm::Value* arg = impl_->lastValue;
                            // An own param ADOPTS the +1 exactly like the
                            // positional loop above - a kwarg bound to it must
                            // not be drained (Strm(s=SocketHandle.adopt_raw(fd))
                            // double-freed the handle without this check).
                            bool kwDrained = impl_->paramIsOwn(ctorName, (unsigned)idx);
                            if (!kwDrained &&
                                pkIt != impl_->funcParamKinds.end() &&
                                idx < pkIt->second.size()) {
                                Impl::VarKind dk = impl_->argTempDecrefKind(
                                    kwVal.get(), pkIt->second[idx], arg);
                                if (dk != Impl::VarKind::Other) {
                                    argTemps.emplace_back(arg, dk);
                                    kwDrained = true;
                                }
                            }
                            if (!kwDrained) {
                                // Same erased-T fallback as the positional loop.
                                if (arg->getType() == impl_->boxType) {
                                    if (impl_->isOwnedBoxResult(arg))
                                        argTemps.emplace_back(
                                            arg, Impl::VarKind::Union);
                                } else {
                                    Impl::VarKind dk = impl_->ownedTempDrainKind(
                                        kwVal.get(), arg);
                                    if (dk != Impl::VarKind::Other)
                                        argTemps.emplace_back(arg, dk);
                                }
                            }
                            llvm::Type* paramTy =
                                ctorFuncType->getParamType((unsigned)idx);
                            args[idx] = impl_->coerceArg(arg, paramTy);
                        }
                    }
                }
                // Fill missing args with default values
                impl_->fillDefaultArgs(ctorName, ctorFunc, args, *this, &argTemps);
                // Exception-safe temps: unwind frees on raise (a ctor body can
                // raise), pop+decref on normal return.
                auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                impl_->lastValue = impl_->normalizeIntC(
                    impl_->builder->CreateCall(ctorFunc, args, "inst"));
                impl_->popArgTempCleanups(argTempBases);
                // Release owned heap-temporary arguments now that the ctor has
                // borrowed them (the fresh instance can't alias an arg).
                for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                impl_->emitMoveOutSlots(node);
                return;
            }
        }

        // D024: Check if function was wrapped by a decorator - use indirect dispatch
        {
            auto decIt = impl_->decoratedFunctions.find(name);
            if (decIt != impl_->decoratedFunctions.end()) {
                // Load the decorated callable from the module global
                auto* gv = decIt->second;
                auto* fnPtr = impl_->builder->CreateLoad(
                    impl_->i8PtrType, gv, name + ".decorated");

                // Get the original function type for proper arg coercion
                llvm::FunctionType* fnType = nullptr;
                auto ctIt = impl_->callableTypes.find(name);
                if (ctIt != impl_->callableTypes.end()) {
                    fnType = ctIt->second;
                } else {
                    std::vector<llvm::Type*> pt(node.args.size(), impl_->i64Type);
                    fnType = llvm::FunctionType::get(impl_->i64Type, pt, false);
                }

                // Evaluate and coerce arguments
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    if (i < fnType->getNumParams())
                        arg = impl_->coerceArg(arg, fnType->getParamType(i));
                    args.push_back(arg);
                }

                // A wrapping decorator (`return lambda ...` capturing the
                // original fn) stores a DragonClosure*, not a bare code
                // pointer - calling it directly jumps into the closure
                // struct's bytes (SIGSEGV on the @-form). Route
                // through the shared closure-vs-bare runtime discrimination,
                // exactly like any other Callable value call. An identity
                // decorator (`return fn`) stores a bare fn ptr and takes the
                // bare branch with zero extra cost beyond the tag check.
                emitCallableValueCall(fnPtr, fnType, args,
                                      /*ownedClosure=*/false, "deccall");
                if (fnType->getReturnType() != impl_->voidType)
                    impl_->lastValue = impl_->normalizeIntC(impl_->lastValue);
                return;
            }
        }

        // Nested-def alias: when emitting a nested def's body, the function's
        // own user name is mapped to its mangled LLVM symbol so self-recursion
        // resolves to a direct call (env auto-appended for capturing variants).
        // Must fire before module->getFunction so a sibling top-level fn
        // doesn't shadow the inner. Outside the body, the alias is gone and
        // calls flow through the local closure variable instead.
        {
            auto naIt = impl_->nestedFunctionAliases.find(name);
            if (naIt != impl_->nestedFunctionAliases.end()) {
                auto& alias = naIt->second;
                auto* aliasFn = alias.fn;
                auto* userType = alias.userFnType;
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    if (i < userType->getNumParams())
                        arg = impl_->coerceArg(arg, userType->getParamType(i));
                    args.push_back(arg);
                }
                if (alias.envValue) args.push_back(alias.envValue);
                if (aliasFn->getReturnType() == impl_->voidType) {
                    impl_->builder->CreateCall(aliasFn, args);
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                } else {
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(aliasFn, args, "ncall"));
                }
                return;
            }
        }

        // User-defined function call (apply reserved-name mangling so `main()`
        // calls in user source resolve to `_dragon_user_main` rather than the
        // C-entry-point module-main).
        // Python semantics: a local var shadows any same-named module-level
        // function. Skip the direct call path when there's a local callable
        // alloca for this name so the first-class indirect path handles it
        // (used by nested `def`, lambda assignments, function references).
        // Same-module call path. Lookup order:
        //  1. importedFuncAliases - `from mod import fn` brought `fn` into
        //  this module's scope under its bare name; the alias map tells
        //  us the mangled symbol it actually resolves to.
        //  2. mangleFunc(currentModule, name) - same-module Dragon def.
        //  3. userFuncName(name) - extern-C symbol or entry-module def.
        llvm::Function* func = nullptr;
        std::string aliasSym = impl_->lookupImportedAlias(name);
        if (!aliasSym.empty()) {
            func = impl_->module->getFunction(aliasSym);
        }
        if (!func) {
            const std::string mangled = Impl::mangleFunc(impl_->currentModuleName, name);
            func = impl_->module->getFunction(mangled);
        }
        if (!func) {
            func = impl_->module->getFunction(Impl::userFuncName(name));
        }
        bool shadowedByLocal =
            func && impl_->callableTypes.count(name) && impl_->lookupVar(name);
        if (func && !shadowedByLocal) {
            // funcVarArgInfo is keyed by the LLVM symbol (post-mangling).
            // We already resolved `func` above; use its actual symbol so
            // two stdlib modules with same-named varargs functions don't
            // clobber each other's vaInfo.
            auto vaIt = impl_->funcVarArgInfo.find(func->getName().str());
            bool hasVarArgs = (vaIt != impl_->funcVarArgInfo.end());

            // *args/**kwargs call path: pack extra positional args into list,
            // kwargs into dict (shared with the module-qualified call path).
            if (hasVarArgs) {
                emitVarArgCall(func, node);
                return;
            }

            // Normal (non-vararg) call path.
            // D030 Phase 4: when the callee param is the union box type,
            // build a {tag, payload} box at the call site instead of passing
            // (val, hiddenTag). For non-union params, coerceArg as before.
            std::vector<llvm::Value*> args;
            // Owned heap-temporary args to release after the call (the callee
            // borrows; it increfs whatever it retains). Extern "C" callees are
            // drainable under the FFI v0 contract unless they return `ptr`
            // (see externDrainableFuncs) - nested owned temps like
            // dragon_str_concat("a", dragon_str_concat(...)) leak the inner
            // string per call without this.
            std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
            const std::string calleeSym = func->getName().str();
            const bool externNoDrain = impl_->externFuncNames.count(calleeSym) &&
                                       !impl_->externDrainableFuncs.count(calleeSym);
            auto fpkIt = externNoDrain
                             ? impl_->funcParamKinds.end()
                             : impl_->funcParamKinds.find(calleeSym);
            auto funcType = func->getFunctionType();
            for (size_t i = 0; i < node.args.size() && i < funcType->getNumParams(); ++i) {
                node.args[i]->accept(*this);
                llvm::Value* arg = impl_->lastValue;
                // D027: a BARE fn (an llvm::Function value) passed to a
                // Callable[...] parameter is wrapped as DragonClosure(fn, null)
                // so the param always holds a real DragonClosure - reliable
                // closure dispatch, no .text tag guess. A closure VALUE (call
                // result / closure var) is NOT an llvm::Function, so it passes
                // through unwrapped. The wrapper is freed after the call below.
                bool argWrapped = false;
                {
                    auto cpIt = impl_->funcCallableParam.find(func->getName().str());
                    if (cpIt != impl_->funcCallableParam.end() && i < cpIt->second.size() &&
                        cpIt->second[i] && llvm::isa<llvm::Function>(arg)) {
                        auto* fnI8 = impl_->builder->CreateBitCast(arg, impl_->i8PtrType);
                        auto* nullEnv = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                        arg = impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_closure_create"],
                            {fnI8, nullEnv}, "fn.wrap");
                        argWrapped = true;
                    }
                }
                if (argWrapped) {
                    // Free the per-call wrapper after the call (callee borrows).
                    argTemps.emplace_back(arg, Impl::VarKind::Closure);
                } else if (impl_->externDrainableFuncs.count(calleeSym)) {
                    // Extern callee: classify by the ARG's OWN static type, not
                    // the declared param kind. The same C symbol can be declared
                    // with disagreeing Dragon arg types across modules (e.g.
                    // dragon_tls_write's buf is `str` in ssl.dr but `ptr` in
                    // postgres.dr, and funcParamKinds keeps only the first);
                    // trusting `str` there decrefs dragon_bytes_data's INTERIOR
                    // pointer as a string - a use-after-free.
                    // ownedTempDrainKind gates on the arg expr's static type, so
                    // an interior-ptr result (type ptr, not a heap kind) is never
                    // drained while a genuine owned str/bytes temp still is.
                    Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[i].get(), arg);
                    if (dk != Impl::VarKind::Other)
                        argTemps.emplace_back(arg, dk);
                } else if (fpkIt != impl_->funcParamKinds.end() && i < fpkIt->second.size()) {
                    // An own param ADOPTS the arg's +1 (moved binding or fresh
                    // temp): the callee releases it; a caller drain here would
                    // double-free (A/B-proven fresh-temp probe).
                    if (!impl_->paramIsOwn(calleeSym, (unsigned)i)) {
                        Impl::VarKind dk = impl_->argTempDecrefKind(
                            node.args[i].get(), fpkIt->second[i], arg);
                        if (dk != Impl::VarKind::Other)
                            argTemps.emplace_back(arg, dk);
                    }
                }
                llvm::Type* paramTy = funcType->getParamType(i);
                // Box-or-coerce through the shared owner-aware path: a borrowed
                // heap value boxed into an Any param is incref'd so the box owns
                // a +1 (donate contract); typed params coerce as before.
                args.push_back(
                    impl_->coerceArgFromExpr(node.args[i].get(), arg, paramTy));
            }
            // Handle extra args beyond declared params (default-fill below catches missing).
            for (size_t i = funcType->getNumParams(); i < node.args.size(); ++i) {
                node.args[i]->accept(*this);
                args.push_back(impl_->lastValue);
            }
            // D040: bind call-site keyword arguments to their named parameter
            // positions. Without this, kwargs to non-vararg functions were
            // silently dropped and `fillDefaultArgs` filled the slots with
            // defaults - a silent miscompile (Config(timeout=30) ignored).
            if (!node.kwArgs.empty()) {
                auto pnIt = impl_->funcParamNames.find(func->getName().str());
                if (pnIt != impl_->funcParamNames.end()) {
                    const auto& paramNames = pnIt->second;
                    // Resize args to the full param count with nullptr holes;
                    // positional values stay at indices 0..node.args.size()-1.
                    size_t numParams = funcType->getNumParams();
                    if (args.size() < numParams)
                        args.resize(numParams, nullptr);
                    for (auto& [kwName, kwVal] : node.kwArgs) {
                        auto nameIt = std::find(paramNames.begin(),
                                                paramNames.end(), kwName);
                        if (nameIt == paramNames.end()) {
                            impl_->addError(
                                "function '" + name +
                                "' got an unexpected keyword argument '" +
                                kwName + "'",
                                node.location());
                            // Poison lastValue so a consumer of this (now
                            // aborted) call doesn't read a stale cross-function
                            // SSA value (M1: that produced a bogus follow-on
                            // "Referring to an instruction in another function"
                            // LLVM-verify error). The kwarg checks now also run
                            // at `check` (TypeChecker), so this is defense-in-depth.
                            impl_->lastValue = llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*impl_->context));
                            return;
                        }
                        size_t idx = (size_t)std::distance(paramNames.begin(),
                                                           nameIt);
                        if (idx >= numParams) {
                            impl_->addError(
                                "keyword argument '" + kwName +
                                "' resolves to a param index outside the "
                                "LLVM signature",
                                node.location());
                            impl_->lastValue = llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*impl_->context));
                            return;
                        }
                        if (args[idx] != nullptr) {
                            impl_->addError(
                                "function '" + name +
                                "' got multiple values for argument '" +
                                kwName + "'",
                                node.location());
                            impl_->lastValue = llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*impl_->context));
                            return;
                        }
                        kwVal->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        if (fpkIt != impl_->funcParamKinds.end() &&
                            idx < fpkIt->second.size()) {
                            Impl::VarKind dk = impl_->argTempDecrefKind(
                                kwVal.get(), fpkIt->second[idx], arg);
                            if (dk != Impl::VarKind::Other)
                                argTemps.emplace_back(arg, dk);
                        }
                        llvm::Type* paramTy = funcType->getParamType(idx);
                        args[idx] =
                            impl_->coerceArgFromExpr(kwVal.get(), arg, paramTy);
                    }
                }
            }
            // Fill missing args with default values; key by the resolved
            // LLVM symbol (matches funcParamDefaults' post-mangling write).
            impl_->fillDefaultArgs(func->getName().str(), func, args, *this, &argTemps);
            // Register the temps on the runtime cleanup stack: the decref
            // below only runs on normal return, so without this an owned temp
            // leaks whenever the callee raises. Freed by exactly one path: the
            // unwind on raise, or the pop+decref on normal return.
            auto argTempBases = impl_->pushArgTempCleanups(argTemps);
            if (func->getReturnType() == impl_->voidType) {
                impl_->builder->CreateCall(func, args);
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
            } else {
                impl_->lastValue = impl_->normalizeIntC(
                    impl_->builder->CreateCall(func, args, "call"));
            }
            // Normal return: rewind the cleanup entries (does NOT free) BEFORE
            // the decref, so each temp is released exactly once (here).
            impl_->popArgTempCleanups(argTempBases);
            // Release owned heap-temporary arguments. Safe even when the callee
            // returns one of them: the return path increfs borrowed values, so
            // the result carries its own reference independent of the arg temp.
            for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
            impl_->emitMoveOutSlots(node);
            return;
        }

        // Check stdlib symbol aliases (from `from math import sqrt` etc.)
        auto aliasIt = impl_->symbolAliases.find(name);
        if (aliasIt != impl_->symbolAliases.end()) {
            // D040: C-aliased functions have fixed positional signatures; they
            // do not participate in keyword-argument binding. Diagnose
            // explicitly rather than silently dropping kwargs (the pre-D040
            // behavior that motivated this decision).
            if (!node.kwArgs.empty()) {
                impl_->addError(
                    "function '" + name + "' (C alias) does not accept "
                    "keyword arguments",
                    node.location());
                return;
            }
            const std::string& cName = aliasIt->second;
            if (node.args.size() == 1) {
                node.args[0]->accept(*this);
                llvm::Value* arg = impl_->lastValue;
                if (arg->getType() == impl_->i64Type)
                    arg = impl_->builder->CreateSIToFP(arg, impl_->f64Type);
                auto* fn = impl_->getOrDeclareRuntime(cName,
                    llvm::FunctionType::get(impl_->f64Type, {impl_->f64Type}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, cName);
                return;
            }
            if (node.args.size() == 2) {
                node.args[0]->accept(*this);
                llvm::Value* a1 = impl_->lastValue;
                node.args[1]->accept(*this);
                llvm::Value* a2 = impl_->lastValue;
                if (a1->getType() == impl_->i64Type)
                    a1 = impl_->builder->CreateSIToFP(a1, impl_->f64Type);
                if (a2->getType() == impl_->i64Type)
                    a2 = impl_->builder->CreateSIToFP(a2, impl_->f64Type);
                auto* fn = impl_->getOrDeclareRuntime(cName,
                    llvm::FunctionType::get(impl_->f64Type, {impl_->f64Type, impl_->f64Type}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {a1, a2}, cName);
                return;
            }
        }

        // ── ADR 025 removal: no dynamic construction through a class value ──
        // A `VarKind::Type` variable that did NOT resolve to a concrete class
        // via typeVarClassName above (a `type`-typed parameter, a `list[type]`
        // element, a function-return-typed var, a `dict[K, type]` lookup) is a
        // class value whose class is not known at compile time. Classes are
        // compile-time entities (D021); there is no runtime descriptor-call.
        {
            auto varKind = impl_->lookupVarKind(name);
            if (varKind == Impl::VarKind::Type) {
                impl_->addError(
                    "classes are not values: cannot construct through '" +
                    name + "' (its class is not known at compile time). "
                    "Construct with the class name directly (e.g. ClassName(...)).",
                    node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }
        }

        // ── __call__ dunder: class instance used as callable ──
        // If the variable holds a class instance and the class has __call__, dispatch to it.
        {
            auto varKind = impl_->lookupVarKind(name);
            if (varKind == Impl::VarKind::ClassInstance) {
                auto cit = impl_->varClassNames.find(name);
                if (cit != impl_->varClassNames.end() && impl_->hasDunder(cit->second, "__call__")) {
                    // Load the instance pointer
                    llvm::Value* objPtr = nullptr;
                    auto* alloca = impl_->lookupVar(name);
                    if (alloca) {
                        objPtr = impl_->builder->CreateLoad(
                            alloca->getAllocatedType(), alloca, name + ".inst");
                    } else {
                        auto* gv = impl_->lookupModuleGlobal(name);
                        if (gv && impl_->shouldUseModuleGlobal(name)) {
                            objPtr = impl_->builder->CreateLoad(
                                gv->getValueType(), gv, name + ".inst");
                        }
                    }
                    if (objPtr) {
                        // Ensure it's a pointer for self param
                        if (!objPtr->getType()->isPointerTy())
                            objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);

                        // Find the __call__ function to coerce args
                        std::string defClass = impl_->findDunderClass(cit->second, "__call__");
                        std::string funcName = defClass + "___call__";
                        auto* callFunc = impl_->module->getFunction(funcName);

                        // Evaluate and coerce arguments
                        std::vector<llvm::Value*> extraArgs;
                        for (size_t i = 0; i < node.args.size(); ++i) {
                            node.args[i]->accept(*this);
                            llvm::Value* arg = impl_->lastValue;
                            // Coerce to match __call__ param type (skip param 0 = self)
                            if (callFunc && (i + 1) < callFunc->getFunctionType()->getNumParams())
                                arg = impl_->coerceArg(arg, callFunc->getFunctionType()->getParamType(i + 1));
                            extraArgs.push_back(arg);
                        }

                        // Dispatch: handle void vs non-void return
                        if (callFunc && callFunc->getReturnType() == impl_->voidType) {
                            std::vector<llvm::Value*> args = {objPtr};
                            args.insert(args.end(), extraArgs.begin(), extraArgs.end());
                            impl_->builder->CreateCall(callFunc, args);
                            impl_->lastValue = llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*impl_->context));
                        } else {
                            auto* result = impl_->callDunder(cit->second, "__call__", objPtr, extraArgs);
                            impl_->lastValue = result ? impl_->normalizeIntC(result)
                                : llvm::ConstantInt::get(impl_->i64Type, 0);
                        }
                        return;
                    }
                }
            }
        }

        // ── First-class function call: indirect call through a variable ──
        // If the name is a local/global variable holding a function pointer
        // (lambda, function reference, or ptr-typed parameter), emit an indirect call.
        {
            llvm::Value* calleePtrStorage = nullptr;
            llvm::Type* loadType = nullptr;

            // Check local variable
            auto* alloca = impl_->lookupVar(name);
            if (alloca) {
                calleePtrStorage = alloca;
                loadType = alloca->getAllocatedType();
            }
            // Check module globals
            if (!calleePtrStorage) {
                auto* gv = impl_->lookupModuleGlobal(name);
                if (gv && impl_->shouldUseModuleGlobal(name)) {
                    calleePtrStorage = gv;
                    loadType = gv->getValueType();
                }
            }

            if (calleePtrStorage) {
                // Load the value from the variable
                llvm::Value* calleeVal = impl_->builder->CreateLoad(
                    loadType, calleePtrStorage, name + ".load");

                // D027/D030: Check if this is a closure (VarKind::Closure)
                auto calleeKind = impl_->lookupVarKind(name);
                if (calleeKind == Impl::VarKind::Closure) {
                    // Get the user-facing signature from callableTypes (fallback:
                    // all-i64 params, i64 return).
                    llvm::FunctionType* userFnType = nullptr;
                    auto ctIt = impl_->callableTypes.find(name);
                    if (ctIt != impl_->callableTypes.end()) {
                        userFnType = ctIt->second;
                    } else {
                        std::vector<llvm::Type*> pt(node.args.size(), impl_->i64Type);
                        userFnType = llvm::FunctionType::get(impl_->i64Type, pt, false);
                    }

                    // Evaluate and coerce user arguments.
                    std::vector<llvm::Value*> args;
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        if (i < userFnType->getNumParams())
                            arg = impl_->coerceArg(arg, userFnType->getParamType(i));
                        args.push_back(arg);
                    }

                    // a VarKind::Closure value is NOT necessarily a real
                    // DragonClosure - with `: Callable` slots uniformly Closure, a
                    // bare top-level function passed to a Callable param/field/
                    // element lands here too (e.g. timeit(_bump), `[inc, mk(1)]`).
                    // emitCallableValueCall discriminates at runtime (TAG_CLOSURE
                    // header vs bare fn ptr, with an env==null sub-case), so the
                    // old inline GEP-unwrap (which assumed a real closure and read
                    // a code pointer's bytes as fn_ptr/env -> SIGSEGV on a bare fn)
                    // is replaced. The callee `name` is a borrow (a var/param/field
                    // read), so the closure is not decref'd after the call.
                    emitCallableValueCall(calleeVal, userFnType, args,
                                          /*ownedClosure=*/false, name);
                    return;
                }

                // Non-closure indirect call (bare function pointer)
                // D025: gate the unsigned-default-signature fallback. Without a
                // recorded callableTypes entry or a `: ptr` annotation, we have
                // no evidence the value is a function pointer - it may be a
                // class descriptor stored in an unannotated parameter, which
                // would segfault when called as code. Emit a clear error.
                llvm::FunctionType* fnType = nullptr;
                auto ctIt = impl_->callableTypes.find(name);
                if (ctIt != impl_->callableTypes.end()) {
                    fnType = ctIt->second;
                } else if (impl_->varIsPtrCallable.count(name)) {
                    std::vector<llvm::Type*> paramTypes(node.args.size(), impl_->i64Type);
                    fnType = llvm::FunctionType::get(impl_->i64Type, paramTypes, false);
                } else {
                    impl_->addError(
                        "cannot call '" + name + "': callee has no known signature; "
                        "annotate as ': type' (for a class) or ': ptr' (for a function pointer)",
                        node.location());
                    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
                    return;
                }
                // Only a typed `Callable[...]` value can hold a DragonClosure at
                // runtime; a `: ptr` value is by definition a RAW bare function
                // pointer (no header) - calling it must NOT read a type tag
                // (offset 8 of a code pointer is undefined data; a chance match
                // against TAG_CLOSURE would wrongly take the unwrap path and
                // crash). Gate the runtime closure discrimination on this.
                const bool valIsTypedCallable = (ctIt != impl_->callableTypes.end());

                // Cast the loaded value to a function pointer of the right type
                llvm::Value* fnPtr = calleeVal;
                if (!calleeVal->getType()->isPointerTy()) {
                    fnPtr = impl_->builder->CreateIntToPtr(
                        calleeVal, llvm::PointerType::getUnqual(*impl_->context));
                }

                // Evaluate and coerce arguments
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    if (i < fnType->getNumParams())
                        arg = impl_->coerceArg(arg, fnType->getParamType(i));
                    args.push_back(arg);
                }

                // The value may actually be a DragonClosure - e.g. a closure
                // passed to a Callable PARAMETER (tracked here as a bare fn
                // pointer), or a function that returns a closure on one path and
                // a bare fn on another. Discriminate at runtime by reading
                // DragonObjectHeader.type_tag @ offset 8: TAG_CLOSURE (10) =>
                // unwrap fn_ptr+env and call fn(args, env); else call the bare
                // fn pointer fn(args). Same proven, safe mechanism as the
                // Callable-field dispatch (CallMethods.cpp) - offset 8 of a real
                // fn ptr is a .text byte (r-x) and ~never equals 10.
                if (valIsTypedCallable) {
                    auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
                    auto* tagAddr = impl_->builder->CreateGEP(
                        i8Ty, fnPtr, llvm::ConstantInt::get(impl_->i64Type, 8),
                        name + ".tag.addr");
                    auto* tagByte = impl_->builder->CreateLoad(i8Ty, tagAddr, name + ".tag");
                    auto* isClosure = impl_->builder->CreateICmpEQ(
                        tagByte, llvm::ConstantInt::get(i8Ty, 10), name + ".is_closure");
                    auto* fnHere = impl_->currentFunction;
                    auto* closBB = llvm::BasicBlock::Create(*impl_->context, name + ".clos", fnHere);
                    auto* bareBB = llvm::BasicBlock::Create(*impl_->context, name + ".bare", fnHere);
                    auto* contBB = llvm::BasicBlock::Create(*impl_->context, name + ".cont", fnHere);
                    impl_->builder->CreateCondBr(isClosure, closBB, bareBB);
                    const bool retVoid = fnType->getReturnType() == impl_->voidType;

                    // Closure path: unwrap { [16 x i8] hdr, fn_ptr, env }.
                    impl_->builder->SetInsertPoint(closBB);
                    auto* closureStructType = llvm::StructType::getTypeByName(
                        *impl_->context, "DragonClosure");
                    if (!closureStructType) {
                        closureStructType = llvm::StructType::create(
                            *impl_->context,
                            {llvm::ArrayType::get(i8Ty, 16), impl_->i8PtrType, impl_->i8PtrType},
                            "DragonClosure");
                    }
                    std::vector<llvm::Type*> closParamTypes;
                    for (unsigned i = 0; i < fnType->getNumParams(); i++)
                        closParamTypes.push_back(fnType->getParamType(i));
                    closParamTypes.push_back(impl_->i8PtrType);
                    auto* closFnType = llvm::FunctionType::get(
                        fnType->getReturnType(), closParamTypes, false);
                    auto* fnPtrAddr = impl_->builder->CreateStructGEP(
                        closureStructType, fnPtr, 1, "closure.fn.ptr");
                    auto* closureFn = impl_->builder->CreateLoad(
                        impl_->i8PtrType, fnPtrAddr, "closure.fn");
                    auto* envAddr = impl_->builder->CreateStructGEP(
                        closureStructType, fnPtr, 2, "closure.env.ptr");
                    auto* envPtr = impl_->builder->CreateLoad(
                        impl_->i8PtrType, envAddr, "closure.env");
                    // A DragonClosure with a NULL env wraps a BARE fn (no env
                    // param) - call it with the user signature; a non-null env
                    // is a real capturing closure - call fn(args, env). This is
                    // why a bare fn passed to a Callable param is WRAPPED as
                    // DragonClosure(fn, null) at the call site: it makes the tag
                    // reliable (a real header), so the bare path below is only a
                    // safety fallback, never the hot path for Callable values.
                    auto* nullEnv = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                    auto* envNull = impl_->builder->CreateICmpEQ(
                        envPtr, nullEnv, name + ".env.null");
                    auto* cBareBB = llvm::BasicBlock::Create(*impl_->context, name + ".cl.bare", fnHere);
                    auto* cEnvBB = llvm::BasicBlock::Create(*impl_->context, name + ".cl.env", fnHere);
                    impl_->builder->CreateCondBr(envNull, cBareBB, cEnvBB);

                    impl_->builder->SetInsertPoint(cBareBB);
                    llvm::Value* cBareRet = nullptr;
                    if (retVoid) impl_->builder->CreateCall(fnType, closureFn, args);
                    else cBareRet = impl_->builder->CreateCall(fnType, closureFn, args, "clbare");
                    impl_->builder->CreateBr(contBB);
                    cBareBB = impl_->builder->GetInsertBlock();

                    impl_->builder->SetInsertPoint(cEnvBB);
                    std::vector<llvm::Value*> closArgs = args;
                    closArgs.push_back(envPtr);
                    llvm::Value* cEnvRet = nullptr;
                    if (retVoid) impl_->builder->CreateCall(closFnType, closureFn, closArgs);
                    else cEnvRet = impl_->builder->CreateCall(closFnType, closureFn, closArgs, "clenv");
                    impl_->builder->CreateBr(contBB);
                    cEnvBB = impl_->builder->GetInsertBlock();

                    // Bare path: call the value as a plain fn pointer (fallback
                    // for an unwrapped raw fn ptr - tag != TAG_CLOSURE).
                    impl_->builder->SetInsertPoint(bareBB);
                    llvm::Value* bareRet = nullptr;
                    if (retVoid) impl_->builder->CreateCall(fnType, fnPtr, args);
                    else bareRet = impl_->builder->CreateCall(fnType, fnPtr, args, "icall");
                    impl_->builder->CreateBr(contBB);
                    bareBB = impl_->builder->GetInsertBlock();

                    impl_->builder->SetInsertPoint(contBB);
                    if (retVoid) {
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        auto* phi = impl_->builder->CreatePHI(
                            fnType->getReturnType(), 3, "icall.res");
                        phi->addIncoming(cBareRet, cBareBB);
                        phi->addIncoming(cEnvRet, cEnvBB);
                        phi->addIncoming(bareRet, bareBB);
                        impl_->lastValue = impl_->normalizeIntC(phi);
                    }
                    return;
                } else {
                    // `: ptr` raw function pointer - plain bare call, no tag read.
                    if (fnType->getReturnType() == impl_->voidType) {
                        impl_->builder->CreateCall(fnType, fnPtr, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(fnType, fnPtr, args, "icall"));
                    }
                    return;
                }
            }
        }

        impl_->addError("Unknown function: " + name, node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }

    // Module-attr direct call: `pkg.sub.fn(args)` where the AttributeExpr's
    // base resolves to a ModuleType. This is the call-context counterpart to
    // the Module case in src/codegen/Attributes.cpp - it emits the same
    // direct LLVM call as `from pkg.sub import fn; fn(args)` (single `call
    // @fn` instruction, no fnptr load, no indirect dispatch). Routing this
    // through emitMethodCall would treat the module as a runtime instance
    // and synthesize a wrong dispatch.
    //
    // We MUST NOT visit attr->object - modules are compile-time only and
    // have no runtime value; the attribute name alone identifies the symbol
    // (all imported modules link into a single LLVM module,
    // so cross-module functions share a flat symbol namespace, modulo
    // userFuncName which only renames `main`).
    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        if (attr->object && attr->object->type &&
            attr->object->type->kind() == Type::Kind::Module) {
            // Class constructor: `mod.Foo(args)` resolves to `Foo_new`
            // (single ctor) or `Foo_new_N` (overloaded). Cross-module class
            // ctors share the flat LLVM symbol space with same-module ctors,
            // so registry lookup and arity match mirror the same-module path:
            // `classNames` is the existence check, `classCtorCount` only
            // disambiguates overloaded ctors.
            if (impl_->classNames.count(attr->attribute)) {
                // Cross-module class ctor: `mod.Foo(args)`. The owning module
                // is taken directly from the AttributeExpr's ModuleType - no
                // alias / classOwningModule fallback needed because the user
                // spelled it explicitly. Mangle with that module so two
                // same-named classes from different modules resolve cleanly.
                const std::string& srcModuleName =
                    static_cast<ModuleType&>(*attr->object->type).name;
                const std::string ctorPrefix =
                    Impl::mangleClass(srcModuleName, attr->attribute);
                std::string ctorName;
                auto ctorCountIt = impl_->classCtorCount.find(attr->attribute);
                if (ctorCountIt != impl_->classCtorCount.end() && ctorCountIt->second > 1) {
                    size_t callArity = node.args.size();
                    auto& arityVec = impl_->classCtorArities[attr->attribute];
                    int matchedIdx = -1;
                    for (auto& [arity, idx] : arityVec) {
                        if (arity == callArity) { matchedIdx = idx; break; }
                    }
                    if (matchedIdx < 0 && !arityVec.empty()) matchedIdx = arityVec[0].second;
                    ctorName = ctorPrefix + "_new_" + std::to_string(matchedIdx);
                } else {
                    ctorName = ctorPrefix + "_new";
                }
                if (auto* ctorFunc = impl_->module->getFunction(ctorName)) {
                    std::vector<llvm::Value*> args;
                    // Owned heap-temp args to release after the call (#3 class A):
                    // `mod.Foo(a + b)` borrows its args, so the temp leaks unless
                    // released. Mirrors the same-module ctor / cross-module fn path.
                    std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                    auto ctorFuncType = ctorFunc->getFunctionType();
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        impl_->collectArgTemp(ctorName, node.args[i].get(), arg,
                                              (unsigned)i, argTemps);
                        if (i < ctorFuncType->getNumParams())
                            arg = impl_->coerceArg(arg, ctorFuncType->getParamType(i));
                        args.push_back(arg);
                    }
                    impl_->fillDefaultArgs(ctorName, ctorFunc, args, *this, &argTemps);
                    // Exception-safe temps (see the same-module ctor path).
                    auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(ctorFunc, args, "inst"));
                    impl_->popArgTempCleanups(argTempBases);
                    for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                    impl_->emitMoveOutSlots(node);
                    return;
                }
            }
            // Bare function in the imported module - direct call. Resolve
            // through the source module's mangled symbol so `gzip.open` and
            // `tarfile.open` reach distinct LLVM bodies. Falls back to the
            // un-mangled name for legacy/extern paths (and so a same-named
            // entry-module function still resolves before mangling lands
            // there).
            const std::string& srcModuleName =
                static_cast<ModuleType&>(*attr->object->type).name;
            const std::string mangled = Impl::mangleFunc(srcModuleName, attr->attribute);
            llvm::Function* func = impl_->module->getFunction(mangled);
            if (!func) {
                func = impl_->module->getFunction(Impl::userFuncName(attr->attribute));
            }
            if (func) {
                // Variadic module function (`mod.pack(fmt, *args)`): pack the
                // trailing args into a list / kwargs into a dict, same as the
                // NameExpr path. Without this the args were passed raw and the
                // call mismatched the (fmt, args_list) signature.
                if (impl_->funcVarArgInfo.count(func->getName().str())) {
                    emitVarArgCall(func, node);
                    return;
                }
                auto funcType = func->getFunctionType();
                std::vector<llvm::Value*> args;
                // Owned heap-temporary args to release after the call: the callee
                // borrows its arguments (it increfs whatever it retains), so an
                // owned temp like `mod.f(a + b)` / `mod.f(str(x))` leaks once per
                // call unless released here. Mirrors the same-module direct-call
                // path; extern "C" callees are drainable under the FFI v0
                // contract unless they return `ptr` (see externDrainableFuncs).
                std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                const std::string calleeSym2 = func->getName().str();
                const bool externNoDrain2 =
                    impl_->externFuncNames.count(calleeSym2) &&
                    !impl_->externDrainableFuncs.count(calleeSym2);
                auto fpkIt = externNoDrain2
                                 ? impl_->funcParamKinds.end()
                                 : impl_->funcParamKinds.find(calleeSym2);
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    // Classify the raw (pre-box/coerce) value, as the same-module
                    // path does. For an extern callee, gate on the ARG's own
                    // static type (ownedTempDrainKind), never the declared param
                    // kind - the same C symbol can carry disagreeing arg types
                    // across modules (see the same-module path for the
                    // interior-pointer hazard). Dragon callees keep the
                    // param-kind classifier.
                    if (impl_->externDrainableFuncs.count(calleeSym2)) {
                        Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[i].get(), arg);
                        if (dk != Impl::VarKind::Other)
                            argTemps.emplace_back(arg, dk);
                    } else if (fpkIt != impl_->funcParamKinds.end() && i < fpkIt->second.size()) {
                        // own param: the callee adopts the +1 - no caller drain.
                        if (!impl_->paramIsOwn(calleeSym2, (unsigned)i)) {
                            Impl::VarKind dk = impl_->argTempDecrefKind(
                                node.args[i].get(), fpkIt->second[i], arg);
                            if (dk != Impl::VarKind::Other)
                                argTemps.emplace_back(arg, dk);
                        }
                    }
                    if (i < funcType->getNumParams()) {
                        // Box-or-coerce through the shared owner-aware path: an
                        // Any param gets a box owning a +1 (borrowed source
                        // incref'd, e.g. json.dumps(obj)); typed params coerce.
                        arg = impl_->coerceArgFromExpr(
                            node.args[i].get(), arg, funcType->getParamType(i));
                    }
                    args.push_back(arg);
                }
                // Fill missing args with default values. funcParamDefaults
                // is keyed by the LLVM symbol (post-mangling), so use the
                // resolved function's name - pre-mangling we passed
                // attr->attribute which silently missed the mangled key.
                impl_->fillDefaultArgs(func->getName().str(), func, args, *this, &argTemps);
                // Exception-safe temps: unwind frees on raise, pop+decref on
                // normal return (see the same-module direct-call path).
                auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                if (func->getReturnType() == impl_->voidType) {
                    impl_->builder->CreateCall(func, args);
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                } else {
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(func, args, "modcall"));
                }
                impl_->popArgTempCleanups(argTempBases);
                // Release owned heap-temporary arguments (callee borrowed them).
                for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                impl_->emitMoveOutSlots(node);
                return;
            }
            impl_->addError(
                "module function '" + attr->attribute +
                "' not found in linked module",
                node.location());
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
            return;
        }
    }

    // Method calls: obj.method(args)
    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        if (emitMethodCall(node, *attr)) return;
        // Silent-drop hardening (#2: a silent fallback is a silent lie). No dispatch
        // path claimed this method call, the old behaviour fell to the `lastValue = 0`
        // stub below and emitted nothing - compiled program simply skipped the call
        // (`self._lock.acquire()` locked nothing until the concurrent mutation detctor
        // exposed it). A call codegen cannot resolve should compile error never no-op.
        std::string recv = "<expression>";
        if (auto* on = dynamic_cast<NameExpr*>(attr->object.get()))
            recv = "'" + on->name + "'";
        else if (auto* oa = dynamic_cast<AttributeExpr*>(attr->object.get()))
            recv = "'" + oa->attribute + "'";
        impl_->addError(
            "cannot resolve method '" + attr->attribute + "' on receiver " +
            recv + ": no codegen dispatch path matched, so the call would "
            "have been silently dropped",
            node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }

    // Call of a callable VALUE - the callee is a value-producing expression
    // (NOT a Name/Attribute, which the dispatches above own) that evaluates to a
    // DragonClosure or a bare fn pointer. Covers the chained `make_adder(5)(10)`
    // (callee is a CallExpr) and `funcs[i](x)` over a list of callables (callee
    // is a SubscriptExpr). Without this we'd hit the `lastValue = 0` stub - the
    // chained-immediate-call miscompile. The callee's static type is the
    // closure's signature (FunctionType); emitCallableValueCall discriminates
    // closure-vs-bare at runtime, so both capturing closures and bare functions
    // stored as callables dispatch correctly. (NameExpr callees never reach here
    // - that branch above always returns.)
    if (!dynamic_cast<AttributeExpr*>(node.callee.get()) &&
        node.callee->type && node.callee->type->kind() == Type::Kind::Function) {
        auto& fnTy = static_cast<FunctionType&>(*node.callee->type);

        // Is the callee an OWNED closure temporary we must free after the call?
        // A direct call to a closure-returning function (`make_adder(5)`) yields
        // a fresh +1 closure with no owner in this immediate-call form, so it
        // leaks (env + wrapper) unless decref'd here - the stored form
        // `g = make_adder(5)` is freed by g's scope cleanup instead. A BORROWED
        // source (a list element `funcs[i]`, a field, a variable load) is owned
        // by its container, so it must NOT be decref'd here.
        // A CallExpr callee that returns a Callable yields an OWNED closure: a
        // closure-returning function always hands back an owning ref now (the
        // ReturnStmt path increfs a borrowed return). So the immediate
        // `f()()` / `make_adder(5)(10)` / `identity(mk(20))(2)` must decref that
        // transient result after the call, else it leaks. A non-call callee (a
        // SubscriptExpr `funcs[i]`, a field/var read) is a BORROW - owned by its
        // container - so it must NOT be decref'd here.
        bool closureOwned =
            dynamic_cast<CallExpr*>(node.callee.get()) != nullptr;

        // User-facing signature from the semantic Callable type.
        std::vector<llvm::Type*> userParamTypes;
        userParamTypes.reserve(fnTy.paramTypes.size());
        for (auto& p : fnTy.paramTypes)
            userParamTypes.push_back(
                impl_->typeKindToLLVM(p ? p->kind() : Type::Kind::Int));
        llvm::Type* userRet = fnTy.returnType
            ? impl_->typeKindToLLVM(fnTy.returnType->kind()) : impl_->i64Type;
        auto* userFnType = llvm::FunctionType::get(userRet, userParamTypes, false);

        // Evaluate the callee, then the args (Python order), then dispatch.
        node.callee->accept(*this);
        llvm::Value* calleeVal = impl_->lastValue;

        std::vector<llvm::Value*> args;
        for (size_t i = 0; i < node.args.size(); ++i) {
            node.args[i]->accept(*this);
            llvm::Value* arg = impl_->lastValue;
            if (i < userParamTypes.size())
                arg = impl_->coerceArg(arg, userParamTypes[i]);
            args.push_back(arg);
        }

        emitCallableValueCall(calleeVal, userFnType, args, closureOwned, "vcall");
        return;
    }

    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}

// Indirect call to a callable VALUE of statically-unknown closure-ness. See the
// header for the contract. Mirrors the runtime discrimination the NameExpr
// typed-callable path uses (tag@8 == TAG_CLOSURE), with the env==null sub-case
// for a bare fn wrapped as DragonClosure(fn, null).
void CodeGen::emitCallableValueCall(llvm::Value* fnPtrVal,
                                    llvm::FunctionType* userFnType,
                                    const std::vector<llvm::Value*>& args,
                                    bool ownedClosure,
                                    const std::string& label) {
    auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
    llvm::Value* fnPtr = fnPtrVal;
    if (!fnPtr->getType()->isPointerTy())
        fnPtr = impl_->builder->CreateIntToPtr(
            fnPtr, llvm::PointerType::getUnqual(*impl_->context), label + ".p");

    const bool retVoid = userFnType->getReturnType() == impl_->voidType;

    // Closure fn type = user params + trailing env ptr.
    std::vector<llvm::Type*> closParamTypes(
        userFnType->param_begin(), userFnType->param_end());
    closParamTypes.push_back(impl_->i8PtrType);
    auto* closFnType = llvm::FunctionType::get(
        userFnType->getReturnType(), closParamTypes, false);

    // Discriminate at runtime: DragonObjectHeader.type_tag @ offset 8 ==
    // TAG_CLOSURE (10)? (offset 8 of a real fn ptr is a .text byte, ~never 10.)
    auto* tagAddr = impl_->builder->CreateGEP(
        i8Ty, fnPtr, llvm::ConstantInt::get(impl_->i64Type, 8), label + ".tag.addr");
    auto* tagByte = impl_->builder->CreateLoad(i8Ty, tagAddr, label + ".tag");
    auto* isClosure = impl_->builder->CreateICmpEQ(
        tagByte, llvm::ConstantInt::get(i8Ty, 10), label + ".is_closure");
    auto* fnHere = impl_->currentFunction;
    auto* closBB = llvm::BasicBlock::Create(*impl_->context, label + ".clos", fnHere);
    auto* bareBB = llvm::BasicBlock::Create(*impl_->context, label + ".bare", fnHere);
    auto* contBB = llvm::BasicBlock::Create(*impl_->context, label + ".cont", fnHere);
    impl_->builder->CreateCondBr(isClosure, closBB, bareBB);

    // Closure path: unwrap { [16 x i8] hdr, fn, env }. A NULL env wraps a BARE
    // fn (no env param) - call it with the user signature; a non-null env is a
    // real capturing closure - call fn(args, env).
    impl_->builder->SetInsertPoint(closBB);
    auto* closureStructType = llvm::StructType::getTypeByName(
        *impl_->context, "DragonClosure");
    if (!closureStructType) {
        closureStructType = llvm::StructType::create(
            *impl_->context,
            {llvm::ArrayType::get(i8Ty, 16), impl_->i8PtrType, impl_->i8PtrType},
            "DragonClosure");
    }
    auto* fnPtrAddr = impl_->builder->CreateStructGEP(
        closureStructType, fnPtr, 1, label + ".fn.ptr");
    auto* closureFn = impl_->builder->CreateLoad(
        impl_->i8PtrType, fnPtrAddr, label + ".fn");
    auto* envAddr = impl_->builder->CreateStructGEP(
        closureStructType, fnPtr, 2, label + ".env.ptr");
    auto* envPtr = impl_->builder->CreateLoad(
        impl_->i8PtrType, envAddr, label + ".env");
    auto* nullEnv = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(impl_->i8PtrType));
    auto* envNull = impl_->builder->CreateICmpEQ(envPtr, nullEnv, label + ".env.null");
    auto* cBareBB = llvm::BasicBlock::Create(*impl_->context, label + ".cl.bare", fnHere);
    auto* cEnvBB = llvm::BasicBlock::Create(*impl_->context, label + ".cl.env", fnHere);
    impl_->builder->CreateCondBr(envNull, cBareBB, cEnvBB);

    impl_->builder->SetInsertPoint(cBareBB);
    llvm::Value* cBareRet = nullptr;
    if (retVoid) impl_->builder->CreateCall(userFnType, closureFn, args);
    else cBareRet = impl_->builder->CreateCall(userFnType, closureFn, args, label + ".clbare");
    impl_->builder->CreateBr(contBB);
    cBareBB = impl_->builder->GetInsertBlock();

    impl_->builder->SetInsertPoint(cEnvBB);
    std::vector<llvm::Value*> closArgs = args;
    closArgs.push_back(envPtr);
    llvm::Value* cEnvRet = nullptr;
    if (retVoid) impl_->builder->CreateCall(closFnType, closureFn, closArgs);
    else cEnvRet = impl_->builder->CreateCall(closFnType, closureFn, closArgs, label + ".clenv");
    impl_->builder->CreateBr(contBB);
    cEnvBB = impl_->builder->GetInsertBlock();

    // Bare path: call the value as a plain fn pointer (tag != TAG_CLOSURE).
    impl_->builder->SetInsertPoint(bareBB);
    llvm::Value* bareRet = nullptr;
    if (retVoid) impl_->builder->CreateCall(userFnType, fnPtr, args);
    else bareRet = impl_->builder->CreateCall(userFnType, fnPtr, args, label + ".icall");
    impl_->builder->CreateBr(contBB);
    bareBB = impl_->builder->GetInsertBlock();

    impl_->builder->SetInsertPoint(contBB);
    if (retVoid) {
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
    } else {
        auto* phi = impl_->builder->CreatePHI(
            userFnType->getReturnType(), 3, label + ".res");
        phi->addIncoming(cBareRet, cBareBB);
        phi->addIncoming(cEnvRet, cEnvBB);
        phi->addIncoming(bareRet, bareBB);
        impl_->lastValue = impl_->normalizeIntC(phi);
    }

    // Free an owned transient closure (cascades to its env). decref_callable is
    // tag-gated, so it no-ops on the bare path; the calls above already consumed
    // fn+env and the result never aliases the wrapper.
    if (ownedClosure && impl_->options.gcMode == GCMode::RC) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_decref_callable"], {fnPtr});
    }
}

void CodeGen::emitVarArgCall(llvm::Function* func, CallExpr& node) {
    auto vaIt = impl_->funcVarArgInfo.find(func->getName().str());
    if (vaIt == impl_->funcVarArgInfo.end()) return;  // caller guarantees variadic
    auto& vaInfo = vaIt->second;
    std::vector<llvm::Value*> args;
    auto funcType = func->getFunctionType();

    auto spreadFail = [&](const std::string& msg, SourceLocation loc) {
        impl_->addError(msg, loc);
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
    };

    // 1. Emit regular positional args (before *args)
    size_t llvmIdx = 0;
    for (size_t i = 0; i < vaInfo.numRegularParams && i < node.args.size(); ++i) {
        if (dynamic_cast<StarredExpr*>(node.args[i].get())) {
            // A `*list` spread landing in a fixed regular slot needs runtime
            // arity-splitting (peel N elements as positionals, rest into *args).
            // Deferred - diagnose rather than miscompile (#2).
            spreadFail("call-site spread into a positional parameter before "
                       "`*args` is not yet supported", node.args[i]->location());
            return;
        }
        node.args[i]->accept(*this);
        llvm::Value* arg = impl_->lastValue;
        if (llvmIdx < funcType->getNumParams())
            arg = impl_->coerceArg(arg, funcType->getParamType(llvmIdx));
        args.push_back(arg);
        llvmIdx++;
    }

    // 1b. Pad any regular-param slots the caller omitted (a default param
    // BEFORE *args, e.g. `def f(a, b=10, *xs)` called as `f(1)`) with nullptr
    // placeholders. Without this the *args list below would be pushed into the
    // first omitted slot (b's), leaving the real *args slot null - an LLVM
    // "Operand is null" verify crash. fillDefaultArgs (step 4) fills the holes.
    while (args.size() < vaInfo.numRegularParams) {
        args.push_back(nullptr);
        llvmIdx++;
    }

    // The *args pack and **kwargs dict are call-site-owned temporaries (the
    // callee borrows its params; its return/store paths take their own refs).
    // Tracked so the call tail can release them - they leaked per call before.
    llvm::Value* packedArgsList = nullptr;
    llvm::Value* packedKwargsDict = nullptr;

    // 2. Pack remaining positional args into a list for *args.
    //  Reuse the same monomorphized list machinery as list[T]
    //  literals (emitNewTypedList / emitTypedListAppend), keyed by
    //  the declared element tag from `*args: T`. This preserves
    //  native types: `*args: float` packs an f64 list, `*args: str`
    //  a ptr list, `*args: A | B` / `*args: Any` a box list -
    //  instead of erasing every element to i64. Bare `*args`
    //  (tag 0) keeps the legacy i64 list (correct for int/bool).
    if (vaInfo.hasVarArg) {
        size_t extraCount = (node.args.size() > vaInfo.numRegularParams)
            ? node.args.size() - vaInfo.numRegularParams : 0;
        auto* cap = llvm::ConstantInt::get(impl_->i64Type, (int64_t)extraCount);
        llvm::Value* argsList = impl_->emitNewTypedList(
            vaInfo.varArgElemTag, vaInfo.varArgElemIsAny, cap);
        for (size_t i = vaInfo.numRegularParams; i < node.args.size(); ++i) {
            if (auto* st = dynamic_cast<StarredExpr*>(node.args[i].get())) {
                // Spread a `*list[T]` into the *args pack. When the source and
                // the `*args: T` target share the plain DragonList monomorph,
                // bulk-extend via one tag-reconciling, incref-correct C loop
                // (dragon_list_extend) - strictly faster than emitting N
                // per-element IR appends, and the commandment-#1 lowering.
                auto* lt = dynamic_cast<ListType*>(st->value->type.get());
                bool srcConcrete = lt && lt->elementType &&
                    lt->elementType->kind() != Type::Kind::Any;
                int64_t srcTag = srcConcrete
                    ? impl_->typeKindToTag(lt->elementType->kind()) : -1;
                if (lt && !vaInfo.varArgElemIsAny && srcConcrete &&
                    srcTag == vaInfo.varArgElemTag) {
                    st->value->accept(*this);
                    llvm::Value* src = impl_->lastValue;
                    if (!src->getType()->isPointerTy())
                        src = impl_->builder->CreateIntToPtr(
                            src, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_extend"],
                        {argsList, src});
                } else {
                    // Box `*args: Any`, element-type mismatch, or a `*tuple`
                    // source aren't bulk-extendable into the pack yet.
                    spreadFail(
                        "call-site spread into `*args` is supported only for a "
                        "`*list[T]` whose element type matches the `*args: T` "
                        "element type", st->location());
                    return;
                }
            } else {
                node.args[i]->accept(*this);
                impl_->emitTypedListAppend(
                    argsList, impl_->lastValue, node.args[i].get(),
                    vaInfo.varArgElemTag, vaInfo.varArgElemIsAny, *this);
            }
        }
        args.push_back(argsList);
        packedArgsList = argsList;
        llvmIdx++;
    }
    // The *args pack is a call-site-owned temp alive across the binding guards
    // below (which can raise via longjmp), so register it on the unwind stack
    // too - a stray-key raise into an args-only callee leaks it otherwise.
    // Pushed AFTER argsList exists, popped before its
    // tail drain, in reverse order relative to the spread-dict entry.
    llvm::Value* packedArgsCleanupBase = nullptr;
    if (packedArgsList)
        packedArgsCleanupBase =
            impl_->emitCleanupPushTemp(packedArgsList, Impl::DCLEAN_OBJ);

    // 2b. Bind keyword arguments that name a regular parameter (before *args)
    // into that positional slot - mirrors the non-variadic D040 binding so
    // `def f(a, b=10, *xs)` accepts `f(a=1)` / `f(1, b=5)`. Keywords that do
    // NOT name a regular param fall through to the **kwargs pack (step 3); a
    // `*args`-only callee with such a leftover is rejected below (validateCall
    // also catches it at `check`). Without this, kwargs to a variadic function
    // were dropped and the regular slot stayed null -> an LLVM verify crash.
    std::vector<bool> kwConsumed(node.kwArgs.size(), false);
    if (!node.kwArgs.empty()) {
        auto pnIt = impl_->funcParamNames.find(func->getName().str());
        if (pnIt != impl_->funcParamNames.end()) {
            const auto& paramNames = pnIt->second;
            for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
                const std::string& kwName = node.kwArgs[ki].first;
                if (kwName.empty()) continue;  // ** spread - handled elsewhere
                auto nameIt = std::find(paramNames.begin(), paramNames.end(), kwName);
                if (nameIt == paramNames.end()) continue;  // unknown -> **kwargs
                size_t idx = (size_t)std::distance(paramNames.begin(), nameIt);
                if (idx >= vaInfo.numRegularParams) continue;  // *args/**kwargs name -> **kwargs
                if (idx < args.size() && args[idx] != nullptr) {
                    spreadFail("function got multiple values for argument '" +
                               kwName + "'", node.location());
                    return;
                }
                node.kwArgs[ki].second->accept(*this);
                llvm::Value* arg = impl_->lastValue;
                llvm::Type* paramTy = funcType->getParamType((unsigned)idx);
                args[idx] = impl_->coerceArgFromExpr(
                    node.kwArgs[ki].second.get(), arg, paramTy);
                kwConsumed[ki] = true;
            }
        }
    }

    // 2c. A `**dict` spread into a variadic callee (the D047 deferral). The
    // spread source is evaluated once. Regular-param slots the call site left
    // unfilled bind by NAME from it (same machinery as the fixed-arity spread
    // path); a spread key naming an already-filled slot raises TypeError
    // ("got multiple values"). When the callee declares **kwargs the
    // remaining entries flow into the kwargs dict via one excluding copy
    // (step 3); a *args-only callee instead rejects stray keys at runtime.
    std::string dispName = "function '" + func->getName().str() + "'";
    if (auto* cn = dynamic_cast<NameExpr*>(node.callee.get()))
        dispName = "function '" + cn->name + "'";
    else if (auto* ca = dynamic_cast<AttributeExpr*>(node.callee.get()))
        dispName = "function '" + ca->attribute + "'";
    llvm::Value* spreadSrc = nullptr;
    Expr* spreadSrcExpr = nullptr;  // the `**source` expr, for the owned-temp drain
    {
        Expr* spreadExpr = nullptr;
        int spreadCount = 0;
        for (auto& kw : node.kwArgs)
            if (kw.first.empty()) { spreadCount++; spreadExpr = kw.second.get(); }
        spreadSrcExpr = spreadExpr;
        if (spreadCount > 1) {
            spreadFail("multiple `**dict` spreads into one call are not "
                       "supported", node.location());
            return;
        }
        if (spreadExpr) {
            spreadExpr->accept(*this);
            spreadSrc = impl_->lastValue;
            if (!spreadSrc->getType()->isPointerTy())
                spreadSrc = impl_->builder->CreateIntToPtr(
                    spreadSrc, impl_->i8PtrType);
        }
    }
    // Register an inline `**{literal}` spread source on the unwind cleanup
    // stack for the whole bind+call: the duplicate/missing/stray-key guards
    // below raise via longjmp, skipping the normal-path drain at the tail, so
    // without this the synthesized dict leaks on every raising call (pinned
    // by test_kwargs_spread *_raises). Freed by exactly one path: the unwind
    // on raise, or the pop+decref at the tail.
    llvm::Value* spreadCleanupBase = nullptr;
    if (spreadSrc && spreadSrcExpr &&
        impl_->ownedTempDrainKind(spreadSrcExpr, spreadSrc) != Impl::VarKind::Other)
        spreadCleanupBase = impl_->emitCleanupPushTemp(spreadSrc, Impl::DCLEAN_OBJ);
    // Emit `if dict has <name>: raise TypeError multiple values` against the
    // spread source - used for filled regular slots and explicit keywords.
    auto emitSpreadDupCheck = [&](const std::string& argName) {
        auto* keyStr = impl_->builder->CreateGlobalString(argName);
        auto* has = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_has_key"], {spreadSrc, keyStr},
            "kwsp.has");
        auto* hasB = impl_->builder->CreateICmpNE(
            has, llvm::ConstantInt::get(impl_->i64Type, 0), "kwsp.dup");
        auto* fn = impl_->currentFunction;
        auto* dupBB = llvm::BasicBlock::Create(*impl_->context, "kwsp.raise", fn);
        auto* okBB = llvm::BasicBlock::Create(*impl_->context, "kwsp.ok", fn);
        impl_->builder->CreateCondBr(hasB, dupBB, okBB);
        impl_->builder->SetInsertPoint(dupBB);
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_raise_exc_cstr"],
            {llvm::ConstantInt::get(impl_->i64Type, 80),
             impl_->builder->CreateGlobalString(
                 "TypeError: " + dispName +
                 " got multiple values for argument '" + argName + "'")});
        impl_->builder->CreateUnreachable();
        impl_->builder->SetInsertPoint(okBB);
    };
    // Build an i8*[] of param-name globals (the excluding-copy / stray-key
    // reject argument). Returns {arrayPtr, count}.
    auto buildNamesArray = [&](const std::vector<std::string>& names)
            -> std::pair<llvm::Value*, int64_t> {
        if (names.empty())
            return {llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(impl_->i8PtrType)),
                    0};
        auto* arrTy = llvm::ArrayType::get(impl_->i8PtrType, names.size());
        auto* arr = impl_->createEntryAlloca(
            impl_->currentFunction, "kwsp.names", arrTy);
        for (size_t i = 0; i < names.size(); ++i) {
            auto* gep = impl_->builder->CreateGEP(arrTy, arr,
                {llvm::ConstantInt::get(impl_->i64Type, 0),
                 llvm::ConstantInt::get(impl_->i64Type, (int64_t)i)});
            impl_->builder->CreateStore(
                impl_->builder->CreateGlobalString(names[i]), gep);
        }
        return {impl_->builder->CreateBitCast(arr, impl_->i8PtrType),
                (int64_t)names.size()};
    };
    std::vector<std::string> regularNames;
    if (spreadSrc) {
        auto pnIt = impl_->funcParamNames.find(func->getName().str());
        if (pnIt == impl_->funcParamNames.end()) {
            spreadFail(dispName + " has no parameter metadata for a `**dict` "
                       "spread", node.location());
            return;
        }
        const auto& paramNames = pnIt->second;
        std::vector<size_t> bindIdx;
        for (size_t idx = 0; idx < vaInfo.numRegularParams &&
                             idx < paramNames.size(); ++idx) {
            regularNames.push_back(paramNames[idx]);
            if (idx < args.size() && args[idx] != nullptr)
                emitSpreadDupCheck(paramNames[idx]);
            else
                bindIdx.push_back(idx);
        }
        impl_->bindParamSlotsFromDict(*this, func, spreadSrc, args, bindIdx,
                                      paramNames, dispName);
        if (!vaInfo.hasKwArg) {
            // *args-only callee: a spread key that is not a regular param is
            // an unexpected keyword - reject at runtime.
            auto [arrPtr, n] = buildNamesArray(regularNames);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_reject_unknown_keys"],
                {spreadSrc, arrPtr,
                 llvm::ConstantInt::get(impl_->i64Type, n),
                 impl_->builder->CreateGlobalString(dispName)});
        }
    }

    // 3. Pack the remaining keyword args into a dict for **kwargs. With a
    // `**dict` spread present the dict starts as a copy of the spread minus
    // the regular-param names (those bound into their slots above).
    if (vaInfo.hasKwArg) {
        llvm::Value* kwargsDict = nullptr;
        if (spreadSrc) {
            auto [arrPtr, n] = buildNamesArray(regularNames);
            kwargsDict = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_copy_excluding"],
                {spreadSrc, arrPtr,
                 llvm::ConstantInt::get(impl_->i64Type, n)}, "kwargs");
        } else {
            auto* cap = llvm::ConstantInt::get(
                impl_->i64Type, (int64_t)node.kwArgs.size());
            kwargsDict = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_new"], {cap}, "kwargs");
        }
        for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
            if (kwConsumed[ki]) continue;  // already bound to a regular param
            const std::string& kwName = node.kwArgs[ki].first;
            if (kwName.empty()) continue;  // the `**dict` spread itself
            // An explicit keyword also present in the spread is a duplicate.
            if (spreadSrc) emitSpreadDupCheck(kwName);
            node.kwArgs[ki].second->accept(*this);
            llvm::Value* val = impl_->lastValue;
            // Determine tag for tagged dict entry
            int64_t tag = 0; // TAG_INT default
            if (val->getType() == impl_->i1Type) {
                tag = 3; // TAG_BOOL
                val = impl_->builder->CreateZExt(val, impl_->i64Type);
            } else if (val->getType() == impl_->f64Type) {
                tag = 2; // TAG_FLOAT
                val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            } else if (val->getType()->isPointerTy()) {
                tag = 1; // TAG_STR (default for pointers)
                // This kwargs dict-set ADOPTS one ref (its destroy decrefs the
                // value), so a BORROWED source - a local, field, or subscript,
                // e.g. `f(a=s)` - must be incref'd here or the kwargs dict frees
                // the caller's string out from under it (UAF of `s`). Owned
                // temporaries (a concat / str() result) already carry the +1 the
                // set consumes. Mirrors the dict-literal value path in
                // Collections.cpp.
                if (impl_->options.gcMode == GCMode::RC &&
                    Impl::isBorrowedHeapExpr(node.kwArgs[ki].second.get())) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref_str"], {val});
                }
                val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            }
            auto* keyStr = impl_->builder->CreateGlobalString(kwName);
            auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_set_tagged"],
                {kwargsDict, keyStr, val, tagVal});
        }
        args.push_back(kwargsDict);
        packedKwargsDict = kwargsDict;
        llvmIdx++;
    } else {
        // *args-only callee: a keyword that bound to no regular param is
        // unexpected. Guard so codegen never silently drops it.
        for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
            if (!kwConsumed[ki] && !node.kwArgs[ki].first.empty()) {
                spreadFail("function got an unexpected keyword argument '" +
                           node.kwArgs[ki].first + "'", node.location());
                return;
            }
        }
    }

    // Fill missing regular args with defaults
    // (only for params before *args - vararg/kwarg don't have defaults).
    // Key by the resolved LLVM symbol so per-module-mangled
    // defaults don't get clobbered across stdlib modules.
    impl_->fillDefaultArgs(func->getName().str(), func, args, *this);

    if (func->getReturnType() == impl_->voidType) {
        impl_->builder->CreateCall(func, args);
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
    } else {
        impl_->lastValue = impl_->normalizeIntC(
            impl_->builder->CreateCall(func, args, "call"));
    }

    // Normal path: rewind the unwind cleanup entries (does NOT free) BEFORE the
    // decrefs so each pack is released exactly once here. Pop in REVERSE push
    // order (spread dict pushed last, args pack first).
    if (spreadCleanupBase) impl_->emitCleanupPopTemp(spreadCleanupBase);
    if (packedArgsCleanupBase) impl_->emitCleanupPopTemp(packedArgsCleanupBase);

    // Release the call-site-owned packs (see declaration above). The callee
    // borrowed them; anything it kept took its own ref via the return/store
    // incref rules.
    if (packedArgsList)
        impl_->emitDecrefByKind(packedArgsList, Impl::VarKind::List);
    if (packedKwargsDict)
        impl_->emitDecrefByKind(packedKwargsDict, Impl::VarKind::Dict);
    // An INLINE `**{literal}` spread source into a variadic callee is an owned
    // temp only read from during binding - drain it here (a named `**opts`
    // source classifies borrowed and stays its owner's). This mirrors the
    // non-variadic emitSpreadCall drain; without it f(**{...}) leaks the whole
    // synthesized dict per call (pinned by test_kwargs_spread).
    if (spreadSrc && spreadSrcExpr) {
        Impl::VarKind dk = impl_->ownedTempDrainKind(spreadSrcExpr, spreadSrc);
        if (dk != Impl::VarKind::Other)
            impl_->emitDecrefByKind(spreadSrc, dk);
    }
}

//===----------------------------------------------------------------------===//
// C9-B - general call-site spread (`*tuple` / `*list` / `**dict`)
//===----------------------------------------------------------------------===//

bool CodeGen::callHasStarArg(CallExpr& node) {
    for (auto& a : node.args)
        if (dynamic_cast<StarredExpr*>(a.get())) return true;
    return false;
}

bool CodeGen::callHasSpread(CallExpr& node) {
    if (callHasStarArg(node)) return true;
    for (auto& kw : node.kwArgs)
        if (kw.first.empty()) return true;  // `**dict` sentinel (empty key)
    return false;
}

// Resolve a NameExpr / module-attr callee to a free function or class ctor and
// emit the expanded (spread) call. Returns false for any callee shape the
// spread machinery does not handle (decorated/closure/dynamic/vararg targets),
// so the caller emits a clear diagnostic.
bool CodeGen::emitSpreadDispatch(CallExpr& node) {
    // ── NameExpr callee: free function or same-module class ctor ──
    if (auto* callee = dynamic_cast<NameExpr*>(node.callee.get())) {
        const std::string& name = callee->name;

        // Class constructor spread. Single-ctor classes always work; an
        // overloaded ctor only resolves when the positional arity is known at
        // compile time (no `*list` / `**dict`), since codegen must pick a
        // `_new_N` body up front.
        if (impl_->classNames.count(name) &&
            !impl_->typedDictClasses.count(name) &&
            !impl_->decoratedClasses.count(name)) {
            const std::string ctorPrefix = impl_->classSymPrefix(name);
            std::string ctorName;
            auto ctorCountIt = impl_->classCtorCount.find(name);
            if (ctorCountIt != impl_->classCtorCount.end() &&
                ctorCountIt->second > 1) {
                // Need a statically-known positional arity to pick the overload.
                int64_t arity = -1;
                if (spreadStaticArity(node, arity)) {
                    auto& arityVec = impl_->classCtorArities[name];
                    int matchedIdx = -1;
                    for (auto& [a, idx] : arityVec)
                        if ((int64_t)a == arity) { matchedIdx = idx; break; }
                    if (matchedIdx >= 0)
                        ctorName = ctorPrefix + "_new_" +
                                   std::to_string(matchedIdx);
                }
                if (ctorName.empty()) return false;  // can't disambiguate
            } else {
                ctorName = ctorPrefix + "_new";
            }
            if (auto* ctorFunc = impl_->module->getFunction(ctorName)) {
                emitSpreadCall(ctorFunc, node, {}, "class '" + name + "' ctor");
                return true;
            }
            return false;
        }

        // Decorated / nested-alias callables are first-class indirect dispatch;
        // spread into them is deferred (increment 6).
        if (impl_->decoratedFunctions.count(name) ||
            impl_->nestedFunctionAliases.count(name))
            return false;

        // Plain user function - resolve with the same lookup order as the
        // direct-call path (imported alias -> same-module mangled -> extern/entry).
        llvm::Function* func = nullptr;
        std::string aliasSym = impl_->lookupImportedAlias(name);
        if (!aliasSym.empty()) func = impl_->module->getFunction(aliasSym);
        if (!func)
            func = impl_->module->getFunction(
                Impl::mangleFunc(impl_->currentModuleName, name));
        if (!func)
            func = impl_->module->getFunction(Impl::userFuncName(name));
        bool shadowedByLocal =
            func && impl_->callableTypes.count(name) && impl_->lookupVar(name);
        if (func && !shadowedByLocal) {
            // Spread into a variadic target routes through the variadic call
            // path: emitVarArgCall bulk-extends a positional `*list` into the
            // *args pack and binds/forwards a `**dict` spread (regular params
            // by name, the rest into **kwargs).
            if (impl_->funcVarArgInfo.count(func->getName().str())) {
                emitVarArgCall(func, node);
                return true;
            }
            emitSpreadCall(func, node, {}, "function '" + name + "'");
            return true;
        }
        return false;
    }

    // ── Module-qualified callee: `mod.fn(*args)` / `mod.Foo(*args)` ──
    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        if (attr->object && attr->object->type &&
            attr->object->type->kind() == Type::Kind::Module) {
            const std::string& srcModuleName =
                static_cast<ModuleType&>(*attr->object->type).name;
            // Cross-module class ctor.
            if (impl_->classNames.count(attr->attribute) &&
                !impl_->typedDictClasses.count(attr->attribute)) {
                const std::string ctorPrefix =
                    Impl::mangleClass(srcModuleName, attr->attribute);
                std::string ctorName;
                auto ctorCountIt = impl_->classCtorCount.find(attr->attribute);
                if (ctorCountIt != impl_->classCtorCount.end() &&
                    ctorCountIt->second > 1) {
                    int64_t arity = -1;
                    if (spreadStaticArity(node, arity)) {
                        auto& arityVec = impl_->classCtorArities[attr->attribute];
                        int matchedIdx = -1;
                        for (auto& [a, idx] : arityVec)
                            if ((int64_t)a == arity) { matchedIdx = idx; break; }
                        if (matchedIdx >= 0)
                            ctorName = ctorPrefix + "_new_" +
                                       std::to_string(matchedIdx);
                    }
                    if (ctorName.empty()) return false;
                } else {
                    ctorName = ctorPrefix + "_new";
                }
                if (auto* ctorFunc = impl_->module->getFunction(ctorName)) {
                    emitSpreadCall(ctorFunc, node, {},
                                   "class '" + attr->attribute + "' ctor");
                    return true;
                }
                return false;
            }
            // Bare module function.
            const std::string mangled =
                Impl::mangleFunc(srcModuleName, attr->attribute);
            llvm::Function* func = impl_->module->getFunction(mangled);
            if (!func)
                func = impl_->module->getFunction(
                    Impl::userFuncName(attr->attribute));
            if (func) {
                if (impl_->funcVarArgInfo.count(func->getName().str())) {
                    emitVarArgCall(func, node);
                    return true;
                }
                emitSpreadCall(func, node, {},
                               "function '" + attr->attribute + "'");
                return true;
            }
        }
    }
    return false;
}

// Compute the statically-known total positional arity of a call when every
// spread is a `*tuple` (whose length is fixed). Returns false if any spread is
// a `*list` (runtime length) or a `**dict` is present (binds by name), so an
// overloaded ctor can't be picked.
bool CodeGen::spreadStaticArity(CallExpr& node, int64_t& arityOut) {
    for (auto& kw : node.kwArgs)
        if (kw.first.empty()) return false;   // `**dict` -> arity unknown
    int64_t total = 0;
    for (auto& a : node.args) {
        if (auto* st = dynamic_cast<StarredExpr*>(a.get())) {
            auto ty = st->value ? st->value->type : nullptr;
            if (ty && ty->kind() == Type::Kind::Tuple)
                total += (int64_t)static_cast<TupleType&>(*ty).elementTypes.size();
            else
                return false;                  // `*list` -> arity unknown
        } else {
            total += 1;
        }
    }
    arityOut = total + (int64_t)node.kwArgs.size();
    return true;
}

// Shared spread expansion - fills `args` (which may already hold prefix values
// like `self`) by expanding `node`'s positional args (with `*tuple`/`*list`
// spread) and kwargs (with `**dict` spread), coercing each into the matching
// param slot of `func`'s signature and recording owned heap temporaries in
// `argTemps`. Spread elements are BORROWED from their source container, so they
// are never added to `argTemps` - the callee increfs whatever it retains. Does
// NOT fill defaults or emit the call (the caller owns dispatch, so a method
// call can route through its vtable). Returns false on a diagnosed error
// (lastValue poisoned to null).
bool CodeGen::Impl::expandSpreadCallArgs(
        CodeGen& cg, llvm::Function* func, CallExpr& node,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName) {
    auto* funcType = func->getFunctionType();
    const size_t numParams = funcType->getNumParams();
    const std::string symName = func->getName().str();
    const bool isExtern = externFuncNames.count(symName) > 0;
    auto fpkIt = isExtern ? funcParamKinds.end()
                          : funcParamKinds.find(symName);

    auto nullPtr = [&]() {
        return llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*context));
    };
    auto fail = [&](const std::string& msg, SourceLocation loc) -> bool {
        addError(msg, loc);
        lastValue = nullPtr();
        return false;
    };

    // Coerce one positional value into the next param slot and append it.
    // `srcExpr` is the AST node for an explicit arg (drives box-tag derivation
    // and owned-temp tracking); spread elements pass srcExpr=nullptr with
    // borrowed=true and supply their static element type for the box tag.
    auto pushPositional = [&](llvm::Value* v, Expr* srcExpr, bool borrowed,
                              std::shared_ptr<Type> staticType) {
        unsigned pidx = (unsigned)args.size();
        if (srcExpr && !borrowed && fpkIt != funcParamKinds.end() &&
            pidx < fpkIt->second.size()) {
            VarKind dk = argTempDecrefKind(srcExpr, fpkIt->second[pidx], v);
            if (dk != VarKind::Other) argTemps.emplace_back(v, dk);
        }
        if (pidx < numParams) {
            llvm::Type* paramTy = funcType->getParamType(pidx);
            if (paramTy == boxType && v->getType() != boxType) {
                if (srcExpr) {
                    v = coerceArgFromExpr(srcExpr, v, paramTy);
                } else {
                    // Borrowed spread element (from a tuple/list): box without
                    // taking ownership, matching the Any-param borrow model.
                    int64_t t = staticType ? typeKindToTag(staticType->kind()) : 0;
                    if (t < 0) t = 0;
                    v = makeBox(llvm::ConstantInt::get(i64Type, t), v);
                }
            } else if (paramTy != boxType) {
                v = srcExpr ? coerceArgFromExpr(srcExpr, v, paramTy)
                            : coerceArg(v, paramTy);
            }
        }
        args.push_back(v);
    };

    // 1. Positional args, expanding any `*spread`.
    for (auto& a : node.args) {
        if (auto* st = dynamic_cast<StarredExpr*>(a.get())) {
            st->value->accept(cg);
            llvm::Value* src = lastValue;
            if (!src->getType()->isPointerTy())
                src = builder->CreateIntToPtr(src, i8PtrType);
            auto srcTy = st->value->type;
            if (auto* tt = dynamic_cast<TupleType*>(srcTy.get())) {
                // *tuple (static arity): per-element typed loads, no runtime
                // check. ~free, heterogeneous element types OK.
                size_t L = tt->elementTypes.size();
                for (size_t k = 0; k < L; ++k) {
                    auto* idx = llvm::ConstantInt::get(i64Type, (int64_t)k);
                    llvm::Value* raw = builder->CreateCall(
                        runtimeFuncs["dragon_tuple_get"], {src, idx},
                        "spread.elem");
                    auto et = tt->elementTypes[k];
                    llvm::Value* native = containerSlotToNative(raw, et.get());
                    pushPositional(native, nullptr, /*borrowed=*/true, et);
                }
            } else if (auto* lt = dynamic_cast<ListType*>(srcTy.get())) {
                // *list[T] (dynamic length): the list must fill EXACTLY the
                // remaining positional param slots - a fixed compile-time count
                // R, since an LLVM call has fixed arity. Emit a runtime
                // `len == R` check + TypeError raise, then R typed loads. All R
                // slots must accept T (the checker verified element-type
                // compatibility). Borrowed elements - no argTemps.
                //
                // A `*list` must be the last positional and can't mix with
                // keyword / `**dict` binding (which competes for the same tail
                // slots and would make R indeterminate at compile time). Those
                // shapes are rejected here rather than miscompiled.
                if (&a != &node.args.back())
                    return fail("`*list` spread must be the last positional "
                                "argument", st->location());
                if (!node.kwArgs.empty())
                    return fail("`*list` spread cannot be combined with keyword "
                                "arguments", st->location());
                auto et = lt->elementType;
                if (et && et->kind() == Type::Kind::Any)
                    return fail("`*list[Any]` spread is not supported; use a "
                                "concrete element type or `*tuple`",
                                st->location());
                int64_t R = (int64_t)numParams - (int64_t)args.size();
                if (R < 0)
                    return fail("too many positional arguments before `*list` "
                                "spread", st->location());
                // Runtime length check: dragon_list_len(src) == R else raise.
                llvm::Value* len = builder->CreateCall(
                    runtimeFuncs["dragon_list_len"], {src}, "spread.len");
                auto* rConst = llvm::ConstantInt::get(i64Type, R);
                auto* ok = builder->CreateICmpEQ(len, rConst, "spread.len.ok");
                auto* fn = currentFunction;
                auto* okBB = llvm::BasicBlock::Create(*context, "spread.ok", fn);
                auto* badBB = llvm::BasicBlock::Create(*context, "spread.bad", fn);
                builder->CreateCondBr(ok, okBB, badBB);
                builder->SetInsertPoint(badBB);
                std::string msg = "TypeError: " + dispName + " expected " +
                    std::to_string(R) + " positional argument" +
                    (R == 1 ? "" : "s") + " from `*list` spread but the list "
                    "length did not match";
                builder->CreateCall(runtimeFuncs["dragon_raise_exc_cstr"],
                    {llvm::ConstantInt::get(i64Type, 80),
                     builder->CreateGlobalString(msg)});
                builder->CreateUnreachable();
                builder->SetInsertPoint(okBB);
                // Pick the typed getter by element kind so values cross the
                // boundary at their native type (no i64 funnel).
                Type::Kind ek = et ? et->kind() : Type::Kind::Int;
                for (int64_t k = 0; k < R; ++k) {
                    auto* idx = llvm::ConstantInt::get(i64Type, k);
                    llvm::Value* elem;
                    if (ek == Type::Kind::Float) {
                        elem = builder->CreateCall(
                            runtimeFuncs["dragon_list_get_f64"], {src, idx},
                            "spread.elem");
                    } else if (ek == Type::Kind::Str || ek == Type::Kind::Bytes ||
                               ek == Type::Kind::List || ek == Type::Kind::Dict ||
                               ek == Type::Kind::Set || ek == Type::Kind::Tuple ||
                               ek == Type::Kind::Instance || ek == Type::Kind::Ptr) {
                        elem = builder->CreateCall(
                            runtimeFuncs["dragon_list_get_ptr"], {src, idx},
                            "spread.elem");
                    } else {
                        // int / bool - DragonListI64 storage.
                        llvm::Value* raw = builder->CreateCall(
                            runtimeFuncs["dragon_list_get"], {src, idx},
                            "spread.elem");
                        elem = (ek == Type::Kind::Bool)
                            ? containerSlotToNative(raw, et.get())  // i64 -> i1
                            : raw;
                    }
                    pushPositional(elem, nullptr, /*borrowed=*/true, et);
                }
            } else {
                return fail("cannot spread a non-tuple/non-list value",
                            st->location());
            }
        } else {
            a->accept(cg);
            pushPositional(lastValue, a.get(), /*borrowed=*/false, nullptr);
        }
    }

    // 2. Keyword args + `**dict` spread.
    if (!node.kwArgs.empty()) {
        if (args.size() < numParams) args.resize(numParams, nullptr);
        auto pnIt = funcParamNames.find(symName);
        // First bind explicit named kwargs; collect the (at most one) `**dict`.
        Expr* dictSpread = nullptr;
        int dictSpreadCount = 0;
        for (auto& [kwName, kwVal] : node.kwArgs) {
            if (kwName.empty()) { dictSpread = kwVal.get(); ++dictSpreadCount; continue; }
            if (pnIt == funcParamNames.end()) continue;
            const auto& paramNames = pnIt->second;
            auto nameIt =
                std::find(paramNames.begin(), paramNames.end(), kwName);
            if (nameIt == paramNames.end())
                return fail(dispName + " got an unexpected keyword argument '" +
                            kwName + "'", node.location());
            size_t idx = (size_t)std::distance(paramNames.begin(), nameIt);
            if (idx >= numParams || args[idx] != nullptr)
                return fail(dispName + " got multiple values for argument '" +
                            kwName + "'", node.location());
            kwVal->accept(cg);
            llvm::Value* v = lastValue;
            if (fpkIt != funcParamKinds.end() && idx < fpkIt->second.size()) {
                VarKind dk = argTempDecrefKind(kwVal.get(), fpkIt->second[idx], v);
                if (dk != VarKind::Other) argTemps.emplace_back(v, dk);
            }
            llvm::Type* paramTy = funcType->getParamType((unsigned)idx);
            args[idx] = coerceArgFromExpr(kwVal.get(), v, paramTy);
        }

        // `**dict` general spread (generalizes the TypedDict `T(**row)` path):
        // bind each still-unfilled param by NAME via one hash lookup. Required
        // params (no default) raise TypeError if absent; optional params fall
        // back to their default. A stray dict key (not a bindable param) raises
        // "unexpected keyword argument". Bound heap values are BORROWED from the
        // dict (no argTemps). This is O(params) hash lookups - the documented
        // `**dict` cost (decisions/047).
        if (dictSpread) {
            if (dictSpreadCount > 1)
                return fail("multiple `**dict` spreads into one call are not "
                            "supported", node.location());
            if (pnIt == funcParamNames.end())
                return fail(dispName + " has no parameter metadata for a "
                            "`**dict` spread", node.location());
            const auto& paramNames = pnIt->second;
            dictSpread->accept(cg);
            llvm::Value* d = lastValue;
            if (!d->getType()->isPointerTy())
                d = builder->CreateIntToPtr(d, i8PtrType);
            // An INLINE literal spread source (`f(**{"name": v})`) is an owned
            // temp the binding only reads from: without a post-call drain the
            // whole synthesized dict (struct + buckets + keys + values) leaks
            // once per call. The bound args are borrows
            // into the dict, so it is released AFTER the call via argTemps,
            // never before. A named spread source (`f(**opts)`) classifies
            // borrowed and stays its owner's.
            {
                VarKind spreadDk = ownedTempDrainKind(dictSpread, d);
                if (spreadDk != VarKind::Other) argTemps.emplace_back(d, spreadDk);
            }
            auto defIt = funcParamDefaults.find(symName);

            // Allowed bindable names = the still-unfilled params (excludes
            // `self` and any positionally / kw-filled slot). Validate the dict
            // carries no key outside this set, then bind each.
            std::vector<llvm::Constant*> allowedPtrs;
            std::vector<size_t> bindIdx;
            for (size_t idx = 0; idx < numParams && idx < paramNames.size(); ++idx) {
                if (args[idx] != nullptr) continue;
                allowedPtrs.push_back(builder->CreateGlobalString(paramNames[idx]));
                bindIdx.push_back(idx);
            }
            auto* arrTy = llvm::ArrayType::get(i8PtrType, allowedPtrs.size());
            llvm::Value* arrPtr;
            if (!allowedPtrs.empty()) {
                auto* arr = createEntryAlloca(currentFunction,
                                              "spread.allowed", arrTy);
                for (size_t i = 0; i < allowedPtrs.size(); ++i) {
                    auto* gep = builder->CreateGEP(arrTy, arr,
                        {llvm::ConstantInt::get(i64Type, 0),
                         llvm::ConstantInt::get(i64Type, (int64_t)i)});
                    builder->CreateStore(allowedPtrs[i], gep);
                }
                arrPtr = builder->CreateBitCast(arr, i8PtrType);
            } else {
                arrPtr = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(i8PtrType));
            }
            builder->CreateCall(runtimeFuncs["dragon_dict_reject_unknown_keys"],
                {d, arrPtr,
                 llvm::ConstantInt::get(i64Type, (int64_t)allowedPtrs.size()),
                 builder->CreateGlobalString(dispName)});

            bindParamSlotsFromDict(cg, func, d, args, bindIdx, paramNames,
                                   dispName);
        }
    }
    return true;
}

void CodeGen::Impl::bindParamSlotsFromDict(
        CodeGen& cg, llvm::Function* func, llvm::Value* d,
        std::vector<llvm::Value*>& args, const std::vector<size_t>& bindIdx,
        const std::vector<std::string>& paramNames,
        const std::string& dispName) {
    auto* funcType = func->getFunctionType();
    const std::string symName = func->getName().str();
    const bool isExtern = externFuncNames.count(symName) > 0;
    auto fpkIt = isExtern ? funcParamKinds.end()
                          : funcParamKinds.find(symName);
    auto defIt = funcParamDefaults.find(symName);

    // Extract one param's value from the dict at its native LLVM type.
    // Only called on the key-present path (the typed getters raise on a
    // missing/mismatched key).
    auto extractParam = [&](size_t idx, llvm::Value* keyStr) -> llvm::Value* {
        llvm::Type* paramTy = funcType->getParamType((unsigned)idx);
        if (paramTy == f64Type)
            return builder->CreateCall(
                runtimeFuncs["dragon_dict_get_str_f64"], {d, keyStr},
                "kw.f64");
        if (paramTy == boxType) {
            auto* tag = builder->CreateCall(
                runtimeFuncs["dragon_dict_get_tag"], {d, keyStr}, "kw.tag");
            auto* payload = builder->CreateCall(
                runtimeFuncs["dragon_dict_get"], {d, keyStr}, "kw.pl");
            return makeBox(tag, payload);
        }
        if (paramTy->isPointerTy()) {
            int64_t tag = 1;  // TAG_STR default
            if (fpkIt != funcParamKinds.end() && idx < fpkIt->second.size()) {
                int64_t t = varKindToTag(fpkIt->second[idx]);
                if (t >= 0) tag = t;
            }
            return builder->CreateCall(
                runtimeFuncs["dragon_dict_get_str_ptr"],
                {d, keyStr, llvm::ConstantInt::get(i64Type, tag)}, "kw.ptr");
        }
        llvm::Value* raw = builder->CreateCall(
            runtimeFuncs["dragon_dict_get"], {d, keyStr}, "kw.raw");
        if (paramTy == i1Type)
            return builder->CreateICmpNE(
                raw, llvm::ConstantInt::get(i64Type, 0), "kw.bool");
        return raw;
    };

    for (size_t idx : bindIdx) {
        llvm::Type* paramTy = funcType->getParamType((unsigned)idx);
        auto* keyStr = builder->CreateGlobalString(paramNames[idx]);
        bool hasDefault = defIt != funcParamDefaults.end() &&
                          idx < defIt->second.size() && defIt->second[idx];
        auto* has = builder->CreateCall(
            runtimeFuncs["dragon_dict_has_key"], {d, keyStr}, "kw.has");
        auto* hasB = builder->CreateICmpNE(
            has, llvm::ConstantInt::get(i64Type, 0), "kw.present");
        auto* fn = currentFunction;
        if (!hasDefault) {
            // Required: raise if absent, else bind.
            auto* okBB = llvm::BasicBlock::Create(*context, "kw.ok", fn);
            auto* missBB = llvm::BasicBlock::Create(*context, "kw.miss", fn);
            builder->CreateCondBr(hasB, okBB, missBB);
            builder->SetInsertPoint(missBB);
            builder->CreateCall(runtimeFuncs["dragon_raise_exc_cstr"],
                {llvm::ConstantInt::get(i64Type, 80),
                 builder->CreateGlobalString(
                     "TypeError: " + dispName +
                     " missing required argument '" + paramNames[idx] +
                     "' in `**dict` spread")});
            builder->CreateUnreachable();
            builder->SetInsertPoint(okBB);
            args[idx] = extractParam(idx, keyStr);
        } else {
            // Optional: present -> dict value, absent -> default expr.
            auto* presentBB = llvm::BasicBlock::Create(*context, "kw.have", fn);
            auto* absentBB = llvm::BasicBlock::Create(*context, "kw.def", fn);
            auto* contBB = llvm::BasicBlock::Create(*context, "kw.cont", fn);
            builder->CreateCondBr(hasB, presentBB, absentBB);
            builder->SetInsertPoint(presentBB);
            llvm::Value* vHave = extractParam(idx, keyStr);
            auto* presentEnd = builder->GetInsertBlock();
            builder->CreateBr(contBB);
            builder->SetInsertPoint(absentBB);
            defIt->second[idx]->accept(cg);
            // coerceArgFromExpr (not coerceArg) so a native default into
            // an `Any`/box param is boxed - matching extractParam's box.
            llvm::Value* vDef = coerceArgFromExpr(
                defIt->second[idx], lastValue, paramTy);
            auto* absentEnd = builder->GetInsertBlock();
            builder->CreateBr(contBB);
            builder->SetInsertPoint(contBB);
            auto* phi = builder->CreatePHI(paramTy, 2, "kw.val");
            phi->addIncoming(vHave, presentEnd);
            phi->addIncoming(vDef, absentEnd);
            args[idx] = phi;
        }
    }
}

void CodeGen::emitSpreadCall(llvm::Function* func, CallExpr& node,
                             std::vector<llvm::Value*> args,
                             const std::string& dispName) {
    std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
    if (!impl_->expandSpreadCallArgs(*this, func, node, args, argTemps, dispName))
        return;  // error diagnosed; lastValue poisoned to null

    // Fill remaining holes with defaults, then emit the call.
    impl_->fillDefaultArgs(func->getName().str(), func, args, *this, &argTemps);
    // Exception-safe temps: unwind frees on raise, pop+decref on normal return.
    auto argTempBases = impl_->pushArgTempCleanups(argTemps);
    if (func->getReturnType() == impl_->voidType) {
        impl_->builder->CreateCall(func, args);
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
    } else {
        impl_->lastValue = impl_->normalizeIntC(
            impl_->builder->CreateCall(func, args, "spreadcall"));
    }
    impl_->popArgTempCleanups(argTempBases);
    // Release owned heap-temporary arguments (spread elements are borrowed and
    // intentionally absent from argTemps; an inline `**{...}` literal source
    // IS in argTemps and is released here, after its borrows are done).
    for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
    impl_->emitMoveOutSlots(node);
}

} // namespace dragon
