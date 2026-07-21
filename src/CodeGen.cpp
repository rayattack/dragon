/// Dragon CodeGen - Public API
/// Constructor, destructor, generate(), compile, link, diagnostics.
// HACK: setjmp/longjmp exception path -- revisit invoke lowering
#include "CodeGenImpl.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdlib>
#include <cstdio>

#if defined(_WIN32)
  #include <windows.h>
  #include <process.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <cerrno>
  #include <cstring>
#endif

namespace dragon {

CodeGen::CodeGen(CodeGenOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    impl_->init();
    impl_->declareRuntimeFunctions();
}

CodeGen::~CodeGen() = default;

bool CodeGen::generate(dragon::Module& module) {
    std::vector<dragon::Module*> noDeps;
    return generate(module, noDeps);
}

bool CodeGen::generate(dragon::Module& entryModule,
                       const std::vector<dragon::Module*>& depModules) {
    // Detect .dr vs .py mode from Module flag (set by Parser from ParserOptions)
    impl_->isDragonFile = entryModule.isDragonFile;
    impl_->depModulePtrs = depModules;
    impl_->entryModulePtr = &entryModule;

    // Track which modules were resolved as files (skip StdlibRegistry for them)
    for (auto* dep : depModules) {
        if (!dep->moduleName.empty())
            impl_->fileResolvedModules.insert(dep->moduleName);
    }

    // Stash module docstrings by name so attribute-access of `m.__doc__`
    // (handled in Attributes.cpp) can find the bytes. Entry module is keyed
    // by "" - the same empty key `currentModuleName` uses for the entry.
    if (entryModule.docstring)
        impl_->moduleDocstrings[""] = *entryModule.docstring;
    for (auto* dep : depModules) {
        if (dep->docstring && !dep->moduleName.empty())
            impl_->moduleDocstrings[dep->moduleName] = *dep->docstring;
    }

    // Forward-declare classes first (so function signatures can reference class types),
    // then functions from all modules (deps first). Each pass sets
    // currentModuleName so per-module symbol mangling (mangleFunc) emits
    // distinct symbols for same-named functions across modules - without
    // this, stdlib modules that share Python-conventional names like
    // `open` / `compress` / `decompress` would collapse onto one symbol
    // and the second emit would silently overwrite (or be dropped by) the
    // first. Restored to "" before entry-module emit so user code keeps
    // bare names.
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        impl_->forwardDeclareClasses(*dep);
    }
    impl_->currentModuleName = "";
    impl_->forwardDeclareClasses(entryModule);
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        impl_->forwardDeclareFunctions(*dep);
    }
    impl_->currentModuleName = "";
    impl_->forwardDeclareFunctions(entryModule);

    // Decorator pre-pass: register each decorated top-level function's
    // indirect-dispatch global + type BEFORE class bodies are emitted below. A
    // class method that calls a decorated free function is lowered in the
    // class-body pass (next), before that function's visit(FunctionDecl) runs
    // its decorator block - without this, the call site misses the
    // decoratedFunctions entry and binds the UNdecorated original (a wrapping
    // decorator's effect silently vanishes when invoked from a method).
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        for (auto& stmt : dep->body)
            if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get()))
                impl_->preregisterDecoratedFunction(*fd);
    }
    impl_->currentModuleName = "";
    for (auto& stmt : entryModule.body)
        if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get()))
            impl_->preregisterDecoratedFunction(*fd);

    // Class-layout pre-pass: register EVERY class's field layout (struct type,
    // field indices/types/kinds, list-elem/dict-value kinds) before any method
    // body is emitted. Without this, a method in class A that references a class
    // B defined later in the file saw empty layout metadata for B and miscompiled
    // every B field access to offset 0 (silent wrong answer). visit(ClassDecl)
    // returns right after layout registration while classLayoutPass is set.
    impl_->classLayoutPass = true;
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        for (auto& stmt : dep->body)
            if (dynamic_cast<ClassDecl*>(stmt.get())) stmt->accept(*this);
    }
    impl_->currentModuleName = "";
    for (auto& stmt : entryModule.body)
        if (dynamic_cast<ClassDecl*>(stmt.get())) stmt->accept(*this);
    impl_->classLayoutPass = false;

    // Forward-declare module-level globals from dependency modules.
    // This creates GlobalVariable stubs so dependency functions can reference
    // them. Actual initialization happens in main() (see below). The LLVM
    // type and VarKind are derived from the full annotation via the same
    // helpers the AnnAssignStmt path uses, so generic types like
    // `dict[str, str]` and `list[T]` get a `ptr` global instead of `i64`
    // (the previous `NamedTypeExpr`-only inspection silently fell through to
    // `i64`, which then mismatched the `dragon_dict_get_str_*` ABI at the
    // load site once the cross-module reader emitted the typed call).
    for (auto* dep : depModules) {
        for (auto& stmt : dep->body) {
            auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get());
            if (!ann || !ann->target) continue;
            auto* name = dynamic_cast<NameExpr*>(ann->target.get());
            if (!name) continue;

            std::string gvName = "global." + name->name;
            if (impl_->module->getGlobalVariable(gvName)) continue;

            llvm::Type* gvType = ann->annotation
                ? impl_->typeExprToLLVM(ann->annotation.get())
                : impl_->i64Type;
            Impl::VarKind vk = ann->annotation
                ? impl_->typeExprToKind(ann->annotation.get())
                : Impl::VarKind::Int;
            // Given the code -> `X: deque[T] = deque(...)`: the annotation lowers
            // list-like, but the val is a real DragonDeque. Correct the kidn + "__Deque"
            // tag here - method bodies compile before the module-body stmnt runs its own
            // correction (AugAnnAssign), and a method reading the global would misroute
            // len/pop through list path (reads the deque header as a list - wrong values,
            // runaway allocation on append)
            if (impl_->annAssignIsDeque(ann)) {
                vk = Impl::VarKind::Deque;
                impl_->varClassNames[name->name] = "__Deque";
            }

            auto* gv = new llvm::GlobalVariable(
                *impl_->module, gvType, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(gvType),
                gvName);
            impl_->moduleGlobals[name->name] = gv;
            impl_->moduleGlobalKinds[name->name] = vk;

            // For class-typed globals, record the class name (and owning
            // module) so attribute/method access through the cross-module
            // reader resolves correctly. resolveAnnotationClassName handles a
            // DOTTED annotation (`db: database.Connection`) by stripping to the
            // leaf class; a bare classNames.count(nt->name) missed it, leaving
            // the global's className empty. A method call on such a global then
            // fell to the "class unknown" vtable path, which guesses the callee
            // signature from an arbitrary same-named method and produced a
            // wrong-arity indirect call (malformed IR) - order-dependent, so it
            // only bit some multi-module builds. Mirrors the AnnAssignStmt path.
            impl_->bindClassVar(name->name, ann->annotation.get());
        }
    }

    // Forward-declare the ENTRY module's top-level globals too, BEFORE the
    // entry-module class bodies are emitted below (line ~140). A class method
    // that reads a module-level global/const is codegen'd before main() lazily
    // creates that global, so without this stub the read resolves to a spurious
    // "Undefined variable". (Entry-module free functions escape this: their
    // bodies are emitted during the main-body iteration, after the top-level
    // decl has already registered the global.) The module-level AnnAssign /
    // Assign paths both reuse an existing moduleGlobals entry, so main() still
    // creates the global exactly once and initializes it in place.
    for (auto& stmt : entryModule.body) {
        // Module-level UNPACK targets (`const a: T, b: U = f()`, an AssignStmt
        // with a TupleExpr target) are module globals too - forward declare
        // them from the checker-resolved element types so class methods can
        // reference them (methods compile before the module body initializes
        // the slots, the unpack lowering in Assign.cpp reuses these gvs).
        if (auto* as = dynamic_cast<AssignStmt*>(stmt.get())) {
            if (as->targets.size() == 1) {
                if (auto* tup = dynamic_cast<TupleExpr*>(as->targets[0].get())) {
                    auto elemType = [&](size_t i) -> std::shared_ptr<Type> {
                        if (i < tup->elements.size() && tup->elements[i]->type)
                            return tup->elements[i]->type;
                        if (as->value && as->value->type) {
                            if (auto* tt = dynamic_cast<TupleType*>(as->value->type.get()))
                                if (i < tt->elementTypes.size())
                                    return tt->elementTypes[i];
                        }
                        return nullptr;
                    };
                    for (size_t i = 0; i < tup->elements.size(); ++i) {
                        auto* nm = dynamic_cast<NameExpr*>(tup->elements[i].get());
                        if (!nm) continue;
                        std::string ugvName = "global." + nm->name;
                        if (impl_->module->getGlobalVariable(ugvName)) continue;
                        llvm::Type* ugvType = impl_->i64Type;
                        Impl::VarKind uvk = Impl::VarKind::Int;
                        if (auto et = elemType(i)) {
                            switch (et->kind()) {
                                case Type::Kind::Float:
                                    ugvType = impl_->f64Type;
                                    uvk = Impl::VarKind::Float;
                                    break;
                                case Type::Kind::Bool:
                                    ugvType = impl_->i1Type;
                                    uvk = Impl::VarKind::Bool;
                                    break;
                                case Type::Kind::Str:
                                case Type::Kind::Bytes:
                                case Type::Kind::List:
                                case Type::Kind::Dict:
                                case Type::Kind::Set:
                                case Type::Kind::Tuple:
                                case Type::Kind::Instance:
                                case Type::Kind::Ptr:
                                    ugvType = impl_->i8PtrType;
                                    uvk = Impl::typeKindToVarKind(et->kind());
                                    break;
                                default:
                                    break;
                            }
                        }
                        auto* ugv = new llvm::GlobalVariable(
                            *impl_->module, ugvType, /*isConstant=*/false,
                            llvm::GlobalValue::InternalLinkage,
                            llvm::Constant::getNullValue(ugvType), ugvName);
                        impl_->moduleGlobals[nm->name] = ugv;
                        impl_->moduleGlobalKinds[nm->name] = uvk;
                        impl_->entryGlobalsAwaitingInit.insert(nm->name);
                    }
                }
            }
            continue;
        }
        auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get());
        if (!ann || !ann->target) continue;
        auto* name = dynamic_cast<NameExpr*>(ann->target.get());
        if (!name) continue;

        std::string gvName = "global." + name->name;
        if (impl_->module->getGlobalVariable(gvName)) continue;

        llvm::Type* gvType = ann->annotation
            ? impl_->typeExprToLLVM(ann->annotation.get())
            : impl_->i64Type;
        Impl::VarKind vk = ann->annotation
            ? impl_->typeExprToKind(ann->annotation.get())
            : Impl::VarKind::Int;
        // Deque-kind correction - see the dep-module loop above.
        if (impl_->annAssignIsDeque(ann)) {
            vk = Impl::VarKind::Deque;
            impl_->varClassNames[name->name] = "__Deque";
        }
        // Callable-typed global: register its signature now, for same ordering reason
        // as deque correction - a class method calling `TAGGER(x)` compiles before module
        // body registers the type, and untyped indirect call fallback it would take will
        // not drain owned result temp (one leaked str per call site under LSan).
        if (auto* cte = dynamic_cast<CallableTypeExpr*>(ann->annotation.get())) {
            impl_->callableTypes[name->name] = impl_->callableTypeExprToFnType(cte);
            vk = Impl::VarKind::Closure;
        }

        auto* gv = new llvm::GlobalVariable(
            *impl_->module, gvType, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(gvType),
            gvName);
        impl_->moduleGlobals[name->name] = gv;
        impl_->moduleGlobalKinds[name->name] = vk;
        impl_->entryGlobalsAwaitingInit.insert(name->name);

        if (auto* nt = dynamic_cast<NamedTypeExpr*>(ann->annotation.get())) {
            if (impl_->classNames.count(nt->name))
                impl_->varClassNames[name->name] = nt->name;
        }
    }

    // Generate dependency module code (functions and classes only, no top-level exprs).
    // currentModuleName is restored per dep so visit(FunctionDecl) and
    // CallExpr's same-module call path resolve to the dep's mangled symbol
    // namespace, not the entry's.
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        for (auto& stmt : dep->body) {
            if (dynamic_cast<FunctionDecl*>(stmt.get()) ||
                dynamic_cast<ClassDecl*>(stmt.get()) ||
                dynamic_cast<ImportStmt*>(stmt.get()) ||
                dynamic_cast<FromImportStmt*>(stmt.get())) {
                stmt->accept(*this);
            }
        }
    }
    impl_->currentModuleName = "";

    // Process the entry module's own import statements BEFORE its class bodies
    // are emitted below. The import visitors only populate alias maps
    // (importedFuncAliasesByModule / symbolAliases) - no code emission, fully
    // idempotent - so re-processing them during the main-body iteration is
    // harmless. Without this, an entry-module method body is codegen'd with an
    // empty alias table, and a `from mod import fn` call inside the method
    // falls through to a 0 / "Unknown function" instead of the mangled symbol.
    // (Dependency modules already get this via the loop above; entry-module
    // free functions escape because their bodies emit during the main-body
    // iteration, after the imports have been processed.)
    for (auto& stmt : entryModule.body) {
        if (dynamic_cast<ImportStmt*>(stmt.get()) ||
            dynamic_cast<FromImportStmt*>(stmt.get())) {
            stmt->accept(*this);
        }
    }

    // Generate entry module class bodies BEFORE main,
    // so their deferredClassInits are processed in the main preamble.
    for (auto& stmt : entryModule.body) {
        if (dynamic_cast<ClassDecl*>(stmt.get())) {
            stmt->accept(*this);
        }
    }

    // Create main function for top-level code. Takes (argc, argv) so that
    // sys.argv / argparse can read process args. Forwards them to the runtime
    // via dragon_set_argv before any user code runs.
    auto* i32Ty = llvm::Type::getInt32Ty(*impl_->context);
    auto* charPtrPtrTy = llvm::PointerType::getUnqual(*impl_->context);
    auto* mainType = llvm::FunctionType::get(
        i32Ty, {i32Ty, charPtrPtrTy}, false);
    auto* mainFunc = llvm::Function::Create(
        mainType, llvm::Function::ExternalLinkage, "main", impl_->module.get());
    auto* mainEntry = llvm::BasicBlock::Create(*impl_->context, "entry", mainFunc);
    impl_->builder->SetInsertPoint(mainEntry);
    impl_->currentFunction = mainFunc;
    impl_->mainFunction = mainFunc;
    {
        // Stash argc/argv into the runtime so user code can read them later.
        auto argIt = mainFunc->arg_begin();
        llvm::Value* argcArg = &*argIt++;
        llvm::Value* argvArg = &*argIt;
        argcArg->setName("argc");
        argvArg->setName("argv");
        auto* setArgvFn = impl_->getOrDeclareRuntime(
            "dragon_set_argv",
            llvm::FunctionType::get(impl_->voidType,
                                    {i32Ty, charPtrPtrTy}, false));
        impl_->builder->CreateCall(setArgvFn, {argcArg, argvArg});
    }

    // Register user-defined exception types with the runtime
    for (auto& [code, parentCode] : impl_->userExcParentCodes) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_register"],
            {llvm::ConstantInt::get(impl_->i64Type, code),
             llvm::ConstantInt::get(impl_->i64Type, parentCode)});
    }

    // Phase 5: Register per-class dealloc + traverse functions and store class_ids
    for (auto& dci : impl_->deferredClassInits) {
        auto* fnPtr = impl_->builder->CreateBitCast(dci.deallocFn, impl_->i8PtrType);
        auto* classId = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_class_register_dealloc"], {fnPtr}, "classid");
        impl_->builder->CreateStore(classId, dci.classIdGlobal);
        // Register traverse function with same class_id
        if (dci.traverseFn) {
            auto* travPtr = impl_->builder->CreateBitCast(dci.traverseFn, impl_->i8PtrType);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_class_register_traverse"], {classId, travPtr});
        }
        // Register clear function with same class_id (cycle collector)
        if (dci.clearFn) {
            auto* clearPtr = impl_->builder->CreateBitCast(dci.clearFn, impl_->i8PtrType);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_class_register_clear"], {classId, clearPtr});
        }
        // D018: Register SHARED-mark function with same class_id
        if (dci.markSharedFn) {
            auto* msPtr = impl_->builder->CreateBitCast(dci.markSharedFn, impl_->i8PtrType);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_class_register_mark_shared"], {classId, msPtr});
        }

        // Decision 025: Create class descriptor after class_id is known
        {
            std::string className = dci.className;            // bare (for metadata maps)
            const std::string& clsSym = dci.classSymPrefix;   // <mod>__<className> (for LLVM symbols)

            // Find the constructor function pointer
            llvm::Value* ctorPtr = nullptr;
            auto ctorCountIt = impl_->classCtorCount.find(className);
            bool isMultiCtor = (ctorCountIt != impl_->classCtorCount.end() && ctorCountIt->second > 1);

            if (isMultiCtor) {
                // Multi-constructor: generate a dispatch wrapper that switches on nargs
                std::string dispatchName = clsSym + "__dispatch";
                auto* dispatchFn = impl_->module->getFunction(dispatchName);
                if (!dispatchFn) {
                    // i8* dispatch(i64* args, i64 nargs) - returns instance ptr
                    auto* dispatchFnType = llvm::FunctionType::get(
                        impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false);
                    dispatchFn = llvm::Function::Create(
                        dispatchFnType, llvm::Function::InternalLinkage,
                        dispatchName, impl_->module.get());

                    // Save current insert point
                    auto* savedBlock = impl_->builder->GetInsertBlock();
                    auto* savedPoint = impl_->builder->GetInsertPoint() != impl_->builder->GetInsertBlock()->end()
                        ? &*impl_->builder->GetInsertPoint() : nullptr;

                    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", dispatchFn);
                    impl_->builder->SetInsertPoint(entry);

                    auto argIt = dispatchFn->arg_begin();
                    llvm::Value* argsArray = &*argIt++;
                    llvm::Value* nargs = &*argIt;

                    auto& arityVec = impl_->classCtorArities[className];
                    auto* defaultBlock = llvm::BasicBlock::Create(*impl_->context, "default", dispatchFn);
                    auto* sw = impl_->builder->CreateSwitch(nargs, defaultBlock, arityVec.size());

                    for (auto& [arity, ctorIdx] : arityVec) {
                        std::string newName = clsSym + "_new_" + std::to_string(ctorIdx);
                        auto* newFn = impl_->module->getFunction(newName);
                        if (!newFn) continue;

                        auto* caseBlock = llvm::BasicBlock::Create(
                            *impl_->context, "arity_" + std::to_string(arity), dispatchFn);
                        sw->addCase(
                            llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, arity)),
                            caseBlock);

                        impl_->builder->SetInsertPoint(caseBlock);
                        // Load args from array and call the specific _new_N
                        std::vector<llvm::Value*> callArgs;
                        auto newFnType = newFn->getFunctionType();
                        for (size_t ai = 0; ai < arity; ai++) {
                            auto* gep = impl_->builder->CreateGEP(
                                impl_->i64Type, argsArray,
                                {llvm::ConstantInt::get(impl_->i64Type, ai)});
                            llvm::Value* argVal = impl_->builder->CreateLoad(impl_->i64Type, gep);
                            if (ai < newFnType->getNumParams())
                                argVal = impl_->coerceArg(argVal, newFnType->getParamType(ai));
                            callArgs.push_back(argVal);
                        }
                        auto* result = impl_->builder->CreateCall(newFn, callArgs, "inst");
                        impl_->builder->CreateRet(result);
                    }

                    // Default: call first overload with 0 args (or return null)
                    impl_->builder->SetInsertPoint(defaultBlock);
                    impl_->builder->CreateRet(
                        llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context)));

                    // Restore insert point
                    if (savedBlock) {
                        if (savedPoint)
                            impl_->builder->SetInsertPoint(savedBlock, savedPoint->getIterator());
                        else
                            impl_->builder->SetInsertPoint(savedBlock);
                    }
                }
                // The dispatch wrapper returns i8*; descriptor_call expects i64 return.
                // Cast dispatch fn ptr to i64 for the descriptor's constructor field.
                ctorPtr = impl_->builder->CreatePtrToInt(dispatchFn, impl_->i64Type);
            } else {
                // Single constructor: use <mod>__<className>_new directly
                std::string newName = clsSym + "_new";
                auto* newFn = impl_->module->getFunction(newName);
                if (newFn) {
                    ctorPtr = impl_->builder->CreatePtrToInt(newFn, impl_->i64Type);
                } else {
                    ctorPtr = llvm::ConstantInt::get(impl_->i64Type, 0);
                }
            }

            // Look up parent descriptor (if class has a parent). Resolve the
            // parent's owning module so two same-named parents from different
            // modules don't last-write-wins through the bare-keyed map.
            llvm::Value* parentDesc = llvm::ConstantInt::get(impl_->i64Type, 0);
            auto parentIt = impl_->classParentNames.find(className);
            if (parentIt != impl_->classParentNames.end()) {
                auto pmIt = impl_->classOwningModule.find(parentIt->second);
                std::string parentMod = pmIt != impl_->classOwningModule.end()
                                            ? pmIt->second
                                            : dci.owningModule;
                std::string parentSym = Impl::mangleClass(parentMod, parentIt->second);
                auto* parentDescGlobal = impl_->module->getNamedGlobal(parentSym + "__descriptor");
                if (!parentDescGlobal) {
                    // Fall back to bare-keyed map (e.g. parent is a builtin
                    // exception class with bare descriptor name).
                    auto descIt = impl_->classDescriptorGlobals.find(parentIt->second);
                    if (descIt != impl_->classDescriptorGlobals.end())
                        parentDescGlobal = descIt->second;
                }
                if (parentDescGlobal) {
                    parentDesc = impl_->builder->CreateLoad(
                        impl_->i64Type, parentDescGlobal, parentIt->second + "_desc");
                }
            }

            // Create the name string as a global constant. The string is the
            // bare class name (user-visible); the GLOBAL's symbol uses the
            // mangled prefix to avoid collision when two same-named classes
            // both reach this point.
            auto* nameStr = impl_->builder->CreateGlobalString(className, clsSym + "__name");
            auto* namePtr = impl_->builder->CreateBitCast(nameStr, impl_->i8PtrType);

            // Use the per-instance descriptor pointer captured at visit time.
            // This bypasses the bare-keyed classDescriptorGlobals last-wins
            // and guarantees this dci writes into ITS module's descriptor.
            llvm::GlobalVariable* descGlobal = dci.descriptorGlobal;
            if (!descGlobal) {
                // Defensive fallback (shouldn't happen for RC classes).
                descGlobal = new llvm::GlobalVariable(
                    *impl_->module, impl_->i64Type, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantInt::get(impl_->i64Type, 0),
                    clsSym + "__descriptor");
            }

            // Resolve the class docstring (if any) - emit it as a `.rodata`
            // global C string so the descriptor's `doc` field is a plain
            // const-char pointer. NULL when there's no docstring; that flows
            // through the niche-ptr Optional[str] ABI as `None`.
            llvm::Value* docPtr = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            auto docIt = impl_->classDocstrings.find(className);
            if (docIt != impl_->classDocstrings.end()) {
                auto* docStr = impl_->builder->CreateGlobalString(
                    docIt->second, clsSym + "__doc");
                docPtr = impl_->builder->CreateBitCast(docStr, impl_->i8PtrType);
            }

            // Call dragon_class_descriptor_create(name, ctor, class_id, parent_desc, doc)
            auto* descVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_class_descriptor_create"],
                {namePtr, ctorPtr, classId, parentDesc, docPtr}, clsSym + "_desc");
            impl_->builder->CreateStore(descVal, descGlobal);

            // Emit field metadata for hasattr()/getattr() reflection
            auto fieldIt = impl_->classFieldIndices.find(className);
            if (fieldIt != impl_->classFieldIndices.end() && !fieldIt->second.empty()) {
                size_t nfields = fieldIt->second.size();
                // Build sorted field list (deterministic order)
                std::vector<std::pair<std::string, unsigned>> fieldList(
                    fieldIt->second.begin(), fieldIt->second.end());
                std::sort(fieldList.begin(), fieldList.end(),
                    [](auto& a, auto& b) { return a.second < b.second; });

                // Emit global constant arrays: field names (i8*[]), BYTE offsets
                // (i64[]) and field byte widths (i64[]). getattr reads exactly
                // `width` bytes at `offset`; fields are native-typed (bool=i1,
                // float=f64), so the runtime must know the real byte offset and
                // width rather than assume an 8-byte GEP slot - otherwise a
                // narrow field misaligns or over-reads the allocation.
                const llvm::DataLayout& dl = impl_->module->getDataLayout();
                llvm::StructType* clsStruct = nullptr;
                auto cstIt = impl_->classStructTypes.find(className);
                if (cstIt != impl_->classStructTypes.end()) clsStruct = cstIt->second;
                const llvm::StructLayout* sl =
                    clsStruct ? dl.getStructLayout(clsStruct) : nullptr;

                std::vector<llvm::Constant*> nameConsts, offsetConsts, widthConsts;
                for (auto& [fname, foffset] : fieldList) {
                    nameConsts.push_back(
                        impl_->builder->CreateGlobalString(fname, clsSym + "_fn_" + fname));
                    // foffset is the LLVM struct element index. Translate to a
                    // byte offset + the element's byte width via the layout.
                    int64_t byteOff = (int64_t)foffset * 8;  // fallback if no layout
                    int64_t byteW = 8;
                    if (sl && clsStruct && foffset < clsStruct->getNumElements()) {
                        byteOff = (int64_t)sl->getElementOffset((unsigned)foffset);
                        byteW = (int64_t)dl.getTypeAllocSize(
                            clsStruct->getElementType((unsigned)foffset));
                    }
                    offsetConsts.push_back(llvm::ConstantInt::get(impl_->i64Type, byteOff));
                    widthConsts.push_back(llvm::ConstantInt::get(impl_->i64Type, byteW));
                }
                auto* nameArrayType = llvm::ArrayType::get(impl_->i8PtrType, nfields);
                auto* nameArray = new llvm::GlobalVariable(
                    *impl_->module, nameArrayType, /*isConstant=*/true,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantArray::get(nameArrayType, nameConsts),
                    clsSym + "__field_names");
                auto* offsetArrayType = llvm::ArrayType::get(impl_->i64Type, nfields);
                auto* offsetArray = new llvm::GlobalVariable(
                    *impl_->module, offsetArrayType, /*isConstant=*/true,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantArray::get(offsetArrayType, offsetConsts),
                    clsSym + "__field_offsets");
                auto* widthArray = new llvm::GlobalVariable(
                    *impl_->module, offsetArrayType, /*isConstant=*/true,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantArray::get(offsetArrayType, widthConsts),
                    clsSym + "__field_widths");

                auto* nameArrayPtr = impl_->builder->CreateBitCast(
                    nameArray, impl_->i8PtrType);
                auto* offsetArrayPtr = impl_->builder->CreateBitCast(
                    offsetArray, impl_->i8PtrType);
                auto* widthArrayPtr = impl_->builder->CreateBitCast(
                    widthArray, impl_->i8PtrType);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_class_descriptor_set_fields"],
                    {descVal, nameArrayPtr, offsetArrayPtr, widthArrayPtr,
                     llvm::ConstantInt::get(impl_->i64Type, (int64_t)nfields)});
            }

            // D033: Emit method-name reflection metadata. Each class
            // advertises its OWN methods; dragon_class_find_method walks the
            // parent chain at lookup time, mirroring how _find_field_offset
            // does it for fields. Skipped when the class has no own
            // non-__init__ methods (e.g. data-only classes).
            auto ownMethodsIt = impl_->classOwnMethods.find(className);
            if (ownMethodsIt != impl_->classOwnMethods.end() &&
                !ownMethodsIt->second.empty()) {
                const auto& ownMethods = ownMethodsIt->second;
                std::vector<llvm::Constant*> mNameConsts;
                std::vector<llvm::Constant*> mFnConsts;
                std::vector<llvm::Constant*> mKindConsts;
                auto& kindsForClass = impl_->classMethodKinds[className];
                for (auto& methodName : ownMethods) {
                    mNameConsts.push_back(impl_->builder->CreateGlobalString(
                        methodName, clsSym + "_mn_" + methodName));
                    llvm::Function* func = impl_->resolveMethodFunction(
                        dci.owningModule, className, methodName);
                    if (func) {
                        mFnConsts.push_back(func);
                    } else {
                        mFnConsts.push_back(llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(impl_->i8PtrType)));
                    }
                    uint8_t kind = 0;
                    auto kIt = kindsForClass.find(methodName);
                    if (kIt != kindsForClass.end()) kind = kIt->second;
                    mKindConsts.push_back(
                        llvm::ConstantInt::get(llvm::Type::getInt8Ty(*impl_->context), kind));
                }
                size_t nmethods = ownMethods.size();
                auto* mNameArrTy = llvm::ArrayType::get(impl_->i8PtrType, nmethods);
                auto* mNameArr = new llvm::GlobalVariable(
                    *impl_->module, mNameArrTy, /*isConstant=*/true,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantArray::get(mNameArrTy, mNameConsts),
                    clsSym + "__method_names");
                auto* mFnArrTy = llvm::ArrayType::get(impl_->i8PtrType, nmethods);
                auto* mFnArr = new llvm::GlobalVariable(
                    *impl_->module, mFnArrTy, /*isConstant=*/true,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantArray::get(mFnArrTy, mFnConsts),
                    clsSym + "__method_fn_ptrs");
                auto* mKindArrTy = llvm::ArrayType::get(
                    llvm::Type::getInt8Ty(*impl_->context), nmethods);
                auto* mKindArr = new llvm::GlobalVariable(
                    *impl_->module, mKindArrTy, /*isConstant=*/true,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantArray::get(mKindArrTy, mKindConsts),
                    clsSym + "__method_kinds");
                auto* mNamePtr = impl_->builder->CreateBitCast(mNameArr, impl_->i8PtrType);
                auto* mFnPtr   = impl_->builder->CreateBitCast(mFnArr,   impl_->i8PtrType);
                auto* mKindPtr = impl_->builder->CreateBitCast(mKindArr, impl_->i8PtrType);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_class_descriptor_set_methods"],
                    {descVal, mNamePtr, mFnPtr, mKindPtr,
                     llvm::ConstantInt::get(impl_->i64Type, (int64_t)nmethods)});

                // D033 Phase 3: parallel bound-thunks array. Same length as
                // method_names; NULL slots for static methods are fine because
                // dragon_getattr only consults this array for instance/class
                // methods (kind 0 / 2). Emitted only when at least one thunk
                // exists, to skip the work for purely-static classes.
                auto thunkMapIt = impl_->classMethodBoundThunks.find(className);
                bool anyThunk = false;
                if (thunkMapIt != impl_->classMethodBoundThunks.end()) {
                    for (auto& kv : thunkMapIt->second) {
                        if (kv.second) { anyThunk = true; break; }
                    }
                }
                if (anyThunk) {
                    std::vector<llvm::Constant*> thunkConsts;
                    for (auto& methodName : ownMethods) {
                        llvm::Function* tFn = nullptr;
                        auto tIt = thunkMapIt->second.find(methodName);
                        if (tIt != thunkMapIt->second.end()) tFn = tIt->second;
                        if (tFn) {
                            thunkConsts.push_back(tFn);
                        } else {
                            thunkConsts.push_back(llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(impl_->i8PtrType)));
                        }
                    }
                    auto* mThunkArrTy = llvm::ArrayType::get(impl_->i8PtrType, nmethods);
                    auto* mThunkArr = new llvm::GlobalVariable(
                        *impl_->module, mThunkArrTy, /*isConstant=*/true,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantArray::get(mThunkArrTy, thunkConsts),
                        clsSym + "__method_bound_thunks");
                    auto* mThunkPtr = impl_->builder->CreateBitCast(
                        mThunkArr, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_class_descriptor_set_method_bound_thunks"],
                        {descVal, mThunkPtr});
                }
            }

            // Class decorators (6.11) are NOT applied here - they run at the
            // class's source position during entry-module-body processing so
            // module-level globals referenced by the decorator are already
            // initialized.
        }
    }

    // Emit deferred static field initializers (collected from dep classes before main existed)
    for (auto& dsi : impl_->deferredStaticInits) {
        dsi.valueExpr->accept(*this);
        llvm::Value* val = impl_->lastValue;
        llvm::Type* fieldType = dsi.gv->getValueType();

        // Type coercion to match the global's type
        if (val->getType() != fieldType) {
            if (fieldType == impl_->f64Type && val->getType() == impl_->i64Type)
                val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
            else if (fieldType == impl_->i64Type && val->getType() == impl_->i1Type)
                val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (fieldType == impl_->i64Type && val->getType() == impl_->f64Type)
                val = impl_->builder->CreateFPToSI(val, impl_->i64Type);
        }

        impl_->builder->CreateStore(val, dsi.gv);
    }

    // Record the scope depth at module top level: declarations directly here
    // (dependency AND entry module top-level) are module globals; declarations
    // nested in a block scope (if/for/while) are block-locals, not globals.
    // Captured BEFORE the dependency-init loop so dep-module consts/vars are
    // gated as globals too (their init runs at this same top-level depth).
    impl_->moduleBodyScopeDepth = impl_->scopes.size();

    // Emit dependency module top-level variable declarations (const/var).
    // These create module globals that dependency functions can reference.
    // currentModuleName is restored per dep so any same-module function
    // call inside the initializer (e.g. `const TBL = _build()` where
    // `_build` is private to the dep) resolves to the dep's mangled
    // symbol instead of falling through to the bare name.
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        for (auto& stmt : dep->body) {
            if (dynamic_cast<AnnAssignStmt*>(stmt.get())) {
                stmt->accept(*this);
            }
        }
    }
    impl_->currentModuleName = "";

    // B Phase 1: decide which class constructions can be stack-allocated. Runs
    // after class/function forward-declaration (classNames populated) and class
    // bodies (classFieldKinds populated for the scalar-only gate at the fork),
    // before any entry-body statement is lowered.
    impl_->computeStackAllocSites(entryModule);

    // Generate entry module code (skip ClassDecl bodies - already emitted pre-main).
    // For decorated classes, apply decorators at the class's source position so
    // any module-level state the decorator depends on is already initialized.
    for (auto& stmt : entryModule.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (impl_->decoratedClasses.count(cd->name)) {
                auto dgIt = impl_->classDescriptorGlobals.find(cd->name);
                if (dgIt != impl_->classDescriptorGlobals.end()) {
                    auto* descGlobal = dgIt->second;
                    llvm::Value* current = impl_->builder->CreateLoad(
                        impl_->i64Type, descGlobal, cd->name + "_desc_pre");
                    auto& decs = impl_->classDecoratorExprs[cd->name];
                    for (int i = (int)decs.size() - 1; i >= 0; i--) {
                        Expr* decExpr = decs[i];
                        llvm::Function* decFn = nullptr;
                        if (auto* nameExpr = dynamic_cast<NameExpr*>(decExpr)) {
                            decFn = impl_->module->getFunction(
                                Impl::userFuncName(nameExpr->name));
                            if (!decFn) decFn = impl_->module->getFunction(nameExpr->name);
                        }
                        if (decFn) {
                            llvm::Value* arg = current;
                            if (decFn->getFunctionType()->getNumParams() > 0) {
                                auto* pt = decFn->getFunctionType()->getParamType(0);
                                arg = impl_->coerceArg(arg, pt);
                            }
                            llvm::Value* result = impl_->builder->CreateCall(
                                decFn, {arg}, "decorated_cls");
                            if (result->getType() == impl_->i8PtrType)
                                result = impl_->builder->CreatePtrToInt(result, impl_->i64Type);
                            current = result;
                        } else {
                            decExpr->accept(*this);
                            llvm::Value* decVal = impl_->lastValue;
                            if (decVal->getType()->isPointerTy())
                                decVal = impl_->builder->CreatePtrToInt(decVal, impl_->i64Type);
                            auto* fnPtr = impl_->builder->CreateIntToPtr(
                                decVal, llvm::PointerType::getUnqual(*impl_->context));
                            auto* indirectType = llvm::FunctionType::get(
                                impl_->i64Type, {impl_->i64Type}, false);
                            current = impl_->builder->CreateCall(
                                indirectType, fnPtr, {current}, "decorated_cls");
                        }
                    }
                    impl_->builder->CreateStore(current, descGlobal);
                }
            }
            continue;
        }
        stmt->accept(*this);
    }

    // 4.7: emit one-shot init for non-ASCII string literals at the FRONT of
    // module-main's entry block. Each literal becomes a dragon_str_intern
    // call whose result (immortal heap DragonString) is stored into a
    // per-literal i8* global. Use sites elsewhere just emit a load - zero
    // per-access cost. Module-main is guaranteed to run because user
    // functions named "main" are mangled (see Impl::userFuncName) so they
    // never collide with the C entry point.
    if (!impl_->utf8LiteralOrder.empty()) {
        auto* savedBB = impl_->builder->GetInsertBlock();
        auto* mainEntry = &impl_->mainFunction->getEntryBlock();
        impl_->builder->SetInsertPoint(mainEntry, mainEntry->begin());
        for (auto& bytes : impl_->utf8LiteralOrder) {
            auto* gv = impl_->utf8LiteralGlobals[bytes];
            auto* dataPtr = impl_->builder->CreateGlobalString(
                llvm::StringRef(bytes.data(), bytes.size()), "utf8lit.bytes");
            auto* lenVal = llvm::ConstantInt::get(impl_->i64Type, (int64_t)bytes.size());
            auto* internFn = impl_->runtimeFuncs["dragon_str_intern"];
            auto* call = impl_->builder->CreateCall(
                internFn, {dataPtr, lenVal}, "utf8lit.init");
            impl_->builder->CreateStore(call, gv);
        }
        impl_->builder->SetInsertPoint(savedBB);
    }

    // Add return 0 to main if the block isn't already terminated
    if (!impl_->builder->GetInsertBlock()->getTerminator()) {
        impl_->builder->CreateRet(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0));
    }

    // Verify the module
    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*impl_->module, &verifyStream)) {
        impl_->addError("LLVM verification failed: " + verifyErr);
        return false;
    }

    return !hasErrors();
}

llvm::Module* CodeGen::getLLVMModule() {
    return impl_->module.get();
}

bool CodeGen::writeIR(const std::string& filename) {
    std::error_code ec;
    llvm::raw_fd_ostream out(filename, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        impl_->addError("Cannot open IR output file: " + ec.message());
        return false;
    }
    impl_->module->print(out, nullptr);
    return true;
}

bool CodeGen::writeBitcode(const std::string& filename) {
    std::error_code ec;
    llvm::raw_fd_ostream out(filename, ec, llvm::sys::fs::OF_None);
    if (ec) {
        impl_->addError("Cannot open bitcode output file: " + ec.message());
        return false;
    }
    llvm::WriteBitcodeToFile(*impl_->module, out);
    return true;
}

bool CodeGen::compileToObject(const std::string& filename) {
    auto targetTriple = impl_->module->getTargetTriple();
    std::string error;
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        impl_->addError("Target lookup failed: " + error);
        return false;
    }

    auto cpu = "generic";
    auto features = "";
    llvm::TargetOptions targetOpts;
    auto rm = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
    auto targetMachine = target->createTargetMachine(
        targetTriple, cpu, features, targetOpts, rm);

    impl_->module->setDataLayout(targetMachine->createDataLayout());

    // Run optimization passes (new PassManager)
    impl_->runOptimizationPasses();

    // Debug: dump post-optimization IR when DRAGON_DUMP_IR=opt. The Driver's
    // pre-optimization dump (DRAGON_DUMP_IR=1) runs before this call, so the
    // -O2 result is invisible there; this captures the IR the backend actually
    // lowers. Path optional via DRAGON_IR_FILE (shared with the pre-opt dump).
    if (const char* mode = std::getenv("DRAGON_DUMP_IR")) {
        if (std::string(mode) == "opt") {
            const char* irFile = std::getenv("DRAGON_IR_FILE");
            std::string irPath = irFile ? irFile : "/tmp/dragon_dump.ll";
            writeIR(irPath);
            llvm::errs() << "[DRAGON_DUMP_IR] wrote post-optimization IR to "
                         << irPath << "\n";
        }
    }

    std::error_code ec;
    llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);
    if (ec) {
        impl_->addError("Cannot open object file: " + ec.message());
        return false;
    }

    llvm::legacy::PassManager pass;
    if (targetMachine->addPassesToEmitFile(
            pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        impl_->addError("Target machine cannot emit object file");
        return false;
    }

    pass.run(*impl_->module);
    dest.flush();
    return true;
}

// ADR 041 - run a subprocess (compiler/linker driver) by argv. Returns true on
// a clean exit(0). Shared by --cc-source shim compilation and the final link;
// on POSIX it fork/execvp's (no shell, no injection), on Windows it _spawnvp's.
// The child's stderr is merged into stdout so the user sees tool diagnostics.
static bool runTool(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
#if defined(_WIN32)
    intptr_t rc = _spawnvp(_P_WAIT, argv[0],
                           const_cast<const char* const*>(argv.data()));
    return rc == 0;
#else
    pid_t pid = fork();
    if (pid == -1) return false;
    if (pid == 0) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);  // execvp failed (tool not found on PATH)
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// ADR 041 - does this --cc-source path name a C++ translation unit? Selects the
// per-file compiler (c++ vs cc) and, transitively, the final link driver.
static bool isCxxSource(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    return ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++" ||
           ext == "C" || ext == "CPP" || ext == "CXX" || ext == "CC";
}

bool CodeGen::linkExecutable(const std::string& outputFile,
                              const std::string& objectFile) {
    // ADR 041 - compile any --cc-source FFI shims to temp objects first, so we
    // know whether a C++ translation unit is present before choosing the link
    // driver. Temp objects are anchored off objectFile's (writable temp) path.
    std::vector<std::string> shimObjects;
    bool anyCxxShim = false;
    for (size_t i = 0; i < impl_->options.ccSources.size(); ++i) {
        const std::string& src = impl_->options.ccSources[i];
        bool cxx = isCxxSource(src);
        anyCxxShim = anyCxxShim || cxx;
        std::string shimObj = objectFile + ".shim" + std::to_string(i) + ".o";
        std::vector<std::string> cc;
#if defined(_WIN32)
        cc.push_back(cxx ? "g++" : "gcc");
#else
        cc.push_back(cxx ? "c++" : "cc");
#endif
        cc.push_back("-c");
        cc.push_back(src);
        cc.push_back("-o");
        cc.push_back(shimObj);
        cc.push_back("-fPIC");
        if (impl_->options.optimizationLevel > 0)
            cc.push_back("-O" + std::to_string(impl_->options.optimizationLevel));
        for (const auto& inc : impl_->options.includePaths)
            cc.push_back("-I" + inc);
        if (!runTool(cc)) {
            impl_->addError("Failed to compile --cc-source shim: " + src);
            for (const auto& o : shimObjects) std::remove(o.c_str());
            return false;
        }
        shimObjects.push_back(shimObj);
    }

    // Build argv for the compiler driver. A C++ shim forces the C++ driver so
    // libstdc++, static initializers, and exception/RTTI tables link correctly
    // (bare `cc ... -lstdc++` is fragile for those). We avoid shell injection on
    // POSIX by using execvp; on Windows we _spawnvp which takes argv directly.
    bool useCxxDriver = anyCxxShim;
#ifdef DRAGON_ASAN_BUILD
    // The instrumented runtime is C++ and needs libstdc++; the c++ driver pulls
    // it (and the ASan runtime) cleanly. DEBUG-ONLY, compiled out otherwise.
    useCxxDriver = true;
#endif
    std::vector<std::string> args;
#if defined(_WIN32)
    args.push_back(useCxxDriver ? "g++" : "gcc");
#else
    args.push_back(useCxxDriver ? "c++" : "cc");
#endif
    args.push_back("-o");
    args.push_back(outputFile);
    args.push_back(objectFile);
    for (const auto& o : shimObjects) args.push_back(o);
#ifdef DRAGON_ASAN_BUILD
    // DEBUG-ONLY (CMake -DDRAGON_ASAN=ON): runtimeLibPath already points at the
    // instrumented runtime twin; add the sanitizer link flags so the compiled
    // program is AddressSanitizer-checked. The `cc` driver doesn't pull
    // libstdc++, so it's named explicitly. This `#ifdef` is compiled out
    // entirely in a normal build - zero effect on shipped binaries.
    args.push_back("-fsanitize=address");
    args.push_back("-fno-omit-frame-pointer");
#endif
    if (!impl_->options.runtimeLibPath.empty()) {
        args.push_back(impl_->options.runtimeLibPath);
    }
    // Always link llhttp (runtime depends on it for HTTP parsing)
    if (!impl_->options.llhttpLibPath.empty()) {
        args.push_back(impl_->options.llhttpLibPath);
    }
    // Link bundled sqlite3 if the program uses sqlite3 functions
    if (!impl_->options.sqlite3LibPath.empty() && impl_->needsSqlite3) {
        args.push_back(impl_->options.sqlite3LibPath);
#if !defined(__APPLE__) && !defined(_WIN32)
        args.push_back("-ldl");
#endif
    }
    // Link bundled PCRE2 if the program uses pcre2 functions
    if (!impl_->options.pcre2LibPath.empty() && impl_->needsPcre2) {
        args.push_back(impl_->options.pcre2LibPath);
    }
    // Link bundled mbedTLS if the program uses TLS. Placed after the runtime
    // archive so the linker resolves runtime_tls.o's mbedtls_* references
    // (mbedTLS is self-contained, so no back-reference into the runtime).
    if (!impl_->options.mbedtlsLibPath.empty() && impl_->needsMbedtls) {
        args.push_back(impl_->options.mbedtlsLibPath);
    }
    for (const auto& path : impl_->options.librarySearchPaths) {
        args.push_back("-L" + path);
    }
#if !defined(_WIN32)
    args.push_back("-lm");
#endif
    // zlib + zstd: dragon_runtime exports compression entry points
    // (dragon_zlib_*, dragon_zstd_*) that depend on the system libz /
    // libzstd. Gate on needsZ / needsZstd (set by forwardDeclareFunctions
    // when an extern decl references those prefixes) so programs that
    // never touch compression don't link against unused system libs -
    // mirrors the sqlite3 / pcre2 gating pattern.
#ifdef __APPLE__
    // Apple clang does not search Homebrew's lib dir on arm64 (/opt/homebrew);
    // Intel brew's /usr/local/lib it does. Only reached when no bundled zstd
    // archive resolved (dev trees without an installed prefix): ld silently
    // ignores absent dirs, and system paths still win for libs in both.
    // libz needs nothing extra (it ships with the OS).
    if (impl_->needsZstd && impl_->options.zstdLibPath.empty()) {
        args.push_back("-L/opt/homebrew/lib");
        args.push_back("-L/usr/local/lib");
    }
#endif
    if (impl_->needsZ) {
        args.push_back("-lz");
    }
    // tested on demzs system - prefer bundled static archive (zstd) - shpped on
    // macOS, where no system libzstd exists and a hombrew .dylib path would break
    // the binary on other machines. 
    if (impl_->needsZstd) {
        if (!impl_->options.zstdLibPath.empty()) {
            args.push_back(impl_->options.zstdLibPath);
        } else {
            args.push_back("-lzstd");
        }
    }
    for (const auto& lib : impl_->options.linkedLibraries) {
        args.push_back("-l" + lib);
    }
    // Auto-link libraries from extern "C" from "lib" { } hints
    for (const auto& lib : impl_->externLibs) {
        args.push_back("-l" + lib);
    }

#if defined(_WIN32)
    // Windows MinGW: dlopen lives in libdl on POSIX but is in libdl/winpthread
    // on MinGW. Threading and sockets need explicit libs.
    if (impl_->needsPthread) {
        args.push_back("-lpthread");
    }
    args.push_back("-lws2_32");
    args.push_back("-liphlpapi");
    args.push_back("-lpsapi");
    args.push_back("-luserenv");
#elif defined(__APPLE__)
    // macOS: dlopen is in libSystem; pthreads is in libSystem too. No -ldl,
    // no explicit -lpthread (linker handles it).
    if (impl_->needsPthread) {
        args.push_back("-lpthread");
    }
#else
    // Linux / generic POSIX
    if (impl_->needsPthread) {
        args.push_back("-lpthread");
    }
    args.push_back("-ldl");
#endif

    bool ok = runTool(args);
    // Clean up the temp shim objects regardless of link outcome.
    for (const auto& o : shimObjects) std::remove(o.c_str());
    if (!ok) {
        impl_->addError("Linking failed");
        return false;
    }
    return true;
}

const std::vector<CodeGenDiagnostic>& CodeGen::diagnostics() const {
    return impl_->diagnostics;
}

bool CodeGen::hasErrors() const {
    for (const auto& d : impl_->diagnostics) {
        if (d.level == CodeGenDiagnostic::Level::Error) return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Visitor: Type Expressions (no-op in codegen)
//===----------------------------------------------------------------------===//

void CodeGen::visit(NamedTypeExpr&) {}
void CodeGen::visit(GenericTypeExpr&) {}
void CodeGen::visit(OptionalTypeExpr&) {}
void CodeGen::visit(UnionTypeExpr&) {}
void CodeGen::visit(CallableTypeExpr&) {}
void CodeGen::visit(TupleTypeExpr&) {}

} // namespace dragon
