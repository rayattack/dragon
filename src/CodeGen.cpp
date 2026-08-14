/// Dragon CodeGen Public API: constructor, destructor, generate(), compile, link, diagnostics.
// HACK: setjmp/longjmp exception path -- revisit invoke lowering
#include "CodeGenImpl.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

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

    // Stash module docstrings by name so `m.__doc__` (Attributes.cpp) can find them.
    // Entry module is keyed by "", the same empty key currentModuleName uses.
    if (entryModule.docstring)
        impl_->moduleDocstrings[""] = *entryModule.docstring;
    for (auto* dep : depModules) {
        if (dep->docstring && !dep->moduleName.empty())
            impl_->moduleDocstrings[dep->moduleName] = *dep->docstring;
    }

    for (auto* dep : depModules) impl_->collectContracts(*dep);
    impl_->collectContracts(entryModule);

    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        impl_->forwardDeclareClasses(*dep);
    }
    impl_->currentModuleName = "";
    impl_->forwardDeclareClasses(entryModule);

    impl_->assignContractSlots();
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        impl_->forwardDeclareFunctions(*dep);
    }
    impl_->currentModuleName = "";
    impl_->forwardDeclareFunctions(entryModule);

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

    for (auto* dep : depModules) {
        // Module context: a dep's global is keyed and class-resolved in the dep.
        impl_->currentModuleName = dep->moduleName;
        for (auto& stmt : dep->body) {
            auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get());
            if (!ann || !ann->target) continue;
            auto* name = dynamic_cast<NameExpr*>(ann->target.get());
            if (!name) continue;

            std::string gKey = Impl::mangleGlobal(dep->moduleName, name->name);
            std::string gvName = "global." + gKey;
            if (impl_->module->getGlobalVariable(gvName)) continue;

            llvm::Type* gvType = ann->annotation
                ? impl_->typeExprToLLVM(ann->annotation.get())
                : impl_->i64Type;
            Impl::VarKind vk = ann->annotation
                ? impl_->typeExprToKind(ann->annotation.get())
                : Impl::VarKind::Int;

            if (impl_->annAssignIsDeque(ann)) {
                vk = Impl::VarKind::Deque;
                impl_->varClassNames[name->name] = "__Deque";
                impl_->moduleGlobalClassNames[gKey] = {"__Deque", ""};
            }

            auto* gv = new llvm::GlobalVariable(
                *impl_->module, gvType, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(gvType),
                gvName);
            impl_->moduleGlobals[gKey] = gv;
            impl_->moduleGlobalKinds[gKey] = vk;

            impl_->bindGlobalClassVar(gKey, name->name, ann->annotation.get());
        }
    }
    impl_->currentModuleName = "";

    for (auto& stmt : entryModule.body) {
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
                        // Entry-module key: mangleGlobal("", name) == bare name.
                        std::string ugKey = Impl::mangleGlobal("", nm->name);
                        std::string ugvName = "global." + ugKey;
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
                        impl_->moduleGlobals[ugKey] = ugv;
                        impl_->moduleGlobalKinds[ugKey] = uvk;
                        impl_->entryGlobalsAwaitingInit.insert(ugKey);
                    }
                }
            }
            continue;
        }
        auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get());
        if (!ann || !ann->target) continue;
        auto* name = dynamic_cast<NameExpr*>(ann->target.get());
        if (!name) continue;

        // Entry-module key: mangleGlobal("", name) == bare name.
        std::string gKey = Impl::mangleGlobal("", name->name);
        std::string gvName = "global." + gKey;
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
            impl_->moduleGlobalClassNames[gKey] = {"__Deque", ""};
        }
        if (auto* cte = dynamic_cast<CallableTypeExpr*>(ann->annotation.get())) {
            impl_->callableTypes[name->name] = impl_->callableTypeExprToFnType(cte);
            vk = Impl::VarKind::Closure;
        }

        auto* gv = new llvm::GlobalVariable(
            *impl_->module, gvType, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(gvType),
            gvName);
        impl_->moduleGlobals[gKey] = gv;
        impl_->moduleGlobalKinds[gKey] = vk;
        impl_->entryGlobalsAwaitingInit.insert(gKey);

        impl_->bindGlobalClassVar(gKey, name->name, ann->annotation.get());
    }

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

    for (auto& stmt : entryModule.body) {
        if (dynamic_cast<ImportStmt*>(stmt.get()) ||
            dynamic_cast<FromImportStmt*>(stmt.get())) {
            stmt->accept(*this);
        }
    }

    for (auto& stmt : entryModule.body) {
        if (dynamic_cast<ClassDecl*>(stmt.get())) {
            stmt->accept(*this);
        }
    }

    // Create main() for top-level code. Takes (argc, argv) so sys.argv/argparse can read
    // process args, forwarded to the runtime via dragon_set_argv before user code runs.
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
            auto ctorCountIt = impl_->classCtorCountBySym.find(clsSym);
            bool isMultiCtor = (ctorCountIt != impl_->classCtorCountBySym.end() && ctorCountIt->second > 1);

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

                    auto& arityVec = impl_->classCtorAritiesBySym[clsSym];
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

            // Look up the parent descriptor (if any); resolve the parent's owning module so
            // two same-named parents from different modules don't last-write-wins the bare-keyed map.
            llvm::Value* parentDesc = llvm::ConstantInt::get(impl_->i64Type, 0);
            auto parentIt = impl_->classParentNamesBySym.find(clsSym);
            if (parentIt != impl_->classParentNamesBySym.end()) {
                // Parent entry IS its sym; the descriptor symbol is direct.
                const std::string& parentSym = parentIt->second;
                auto* parentDescGlobal = impl_->module->getNamedGlobal(parentSym + "__descriptor");
                if (!parentDescGlobal) {
                    auto descIt = impl_->classDescriptorGlobalsBySym.find(parentSym);
                    if (descIt != impl_->classDescriptorGlobalsBySym.end())
                        parentDescGlobal = descIt->second;
                }
                if (parentDescGlobal) {
                    parentDesc = impl_->builder->CreateLoad(
                        impl_->i64Type, parentDescGlobal, parentIt->second + "_desc");
                }
            }

            // The name string is the bare class name (user-visible); the GLOBAL's symbol
            // uses the mangled prefix so two same-named classes don't collide.
            auto* nameStr = impl_->builder->CreateGlobalString(className, clsSym + "__name");
            auto* namePtr = impl_->builder->CreateBitCast(nameStr, impl_->i8PtrType);

            // Use the per-instance descriptor pointer captured at visit time; this bypasses
            // the bare-keyed classDescriptorGlobals last-wins so this dci writes its own module's descriptor.
            llvm::GlobalVariable* descGlobal = dci.descriptorGlobal;
            if (!descGlobal) {
                impl_->addError(
                    "internal error: class '" + className +
                    "' has no descriptor global; reflection metadata would "
                    "have been written to a fresh unused slot");
                descGlobal = new llvm::GlobalVariable(
                    *impl_->module, impl_->i64Type, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantInt::get(impl_->i64Type, 0),
                    clsSym + "__descriptor");
            }

            // Resolve the class docstring (if any) into a plain const-char global; NULL
            // when absent flows through the niche-ptr Optional[str] ABI as None.
            llvm::Value* docPtr = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            auto docIt = impl_->classDocstringsBySym.find(clsSym);
            if (docIt != impl_->classDocstringsBySym.end()) {
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
            auto fieldIt = impl_->classFieldIndicesBySym.find(clsSym);
            if (fieldIt != impl_->classFieldIndicesBySym.end() && !fieldIt->second.empty()) {
                size_t nfields = fieldIt->second.size();
                // Build sorted field list (deterministic order)
                std::vector<std::pair<std::string, unsigned>> fieldList(
                    fieldIt->second.begin(), fieldIt->second.end());
                std::sort(fieldList.begin(), fieldList.end(),
                    [](auto& a, auto& b) { return a.second < b.second; });

                const llvm::DataLayout& dl = impl_->module->getDataLayout();
                llvm::StructType* clsStruct = nullptr;
                auto cstIt = impl_->classStructTypesBySym.find(clsSym);
                if (cstIt != impl_->classStructTypesBySym.end()) clsStruct = cstIt->second;
                const llvm::StructLayout* sl =
                    clsStruct ? dl.getStructLayout(clsStruct) : nullptr;

                std::vector<llvm::Constant*> nameConsts, offsetConsts, widthConsts;
                for (auto& [fname, foffset] : fieldList) {
                    nameConsts.push_back(
                        impl_->builder->CreateGlobalString(fname, clsSym + "_fn_" + fname));
                    // foffset is the LLVM struct element index. Translate to a
                    // byte offset + the element's byte width via the layout.
                    int64_t byteOff;
                    int64_t byteW;
                    if (sl && clsStruct && foffset < clsStruct->getNumElements()) {
                        byteOff = (int64_t)sl->getElementOffset((unsigned)foffset);
                        byteW = (int64_t)dl.getTypeAllocSize(
                            clsStruct->getElementType((unsigned)foffset));
                    } else {
                        impl_->addError(
                            "internal error: struct layout for class '" + className +
                            "' is missing field '" + fname +
                            "'; getattr/hasattr offsets would have been guessed "
                            "at 8 bytes per field");
                        byteOff = (int64_t)foffset * 8;
                        byteW = 8;
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

            auto ownMethodsIt = impl_->classOwnMethodsBySym.find(clsSym);
            if (ownMethodsIt != impl_->classOwnMethodsBySym.end() &&
                !ownMethodsIt->second.empty()) {
                const auto& ownMethods = ownMethodsIt->second;
                std::vector<llvm::Constant*> mNameConsts;
                std::vector<llvm::Constant*> mFnConsts;
                std::vector<llvm::Constant*> mKindConsts;
                auto& kindsForClass = impl_->classMethodKindsBySym[clsSym];
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

                auto thunkMapIt = impl_->classMethodBoundThunksBySym.find(clsSym);
                bool anyThunk = false;
                if (thunkMapIt != impl_->classMethodBoundThunksBySym.end()) {
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

    // Scope depth at module top level: declarations here (dep or entry top-level) are
    // module globals, nested ones (if/for/while) are block-locals. Captured before the dependency-init loop so dep consts/vars are gated as globals too.
    impl_->moduleBodyScopeDepth = impl_->scopes.size();

    // Emit dependency module top-level var declarations (const/var), creating module
    // globals dependency functions can reference. currentModuleName is restored per dep so a same-module call in the initializer (e.g. a private `_build()`) resolves to the dep's mangled symbol.
    for (auto* dep : depModules) {
        impl_->currentModuleName = dep->moduleName;
        for (auto& stmt : dep->body) {
            if (dynamic_cast<AnnAssignStmt*>(stmt.get())) {
                stmt->accept(*this);
            }
        }
    }
    impl_->currentModuleName = "";

    // B Phase 1: decide which class constructions can stack-allocate. Runs after
    // class/function forward-declaration and class bodies, before any entry-body statement lowers.
    impl_->computeStackAllocSites(entryModule);

    // Generate entry module code (ClassDecl bodies already emitted pre-main). Decorated
    // classes apply decorators at the class's source position so module-level state they depend on is already initialized.
    for (auto& stmt : entryModule.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (impl_->decoratedClassesBySym.count(impl_->classSym(cd->name))) {
                auto dgIt = impl_->classDescriptorGlobalsBySym.find(impl_->classSym(cd->name));
                if (dgIt != impl_->classDescriptorGlobalsBySym.end()) {
                    auto* descGlobal = dgIt->second;
                    llvm::Value* current = impl_->builder->CreateLoad(
                        impl_->i64Type, descGlobal, cd->name + "_desc_pre");
                    auto& decs = impl_->classDecoratorExprsBySym[impl_->classSym(cd->name)];
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

    // 4.7: one-shot init for non-ASCII string literals at the front of module-main's
    // entry block: each becomes a dragon_str_intern call stored into a per-literal i8* global, so use sites just load it (zero per-access cost). Module-main always runs since user "main" is mangled (Impl::userFuncName), never colliding with the C entry point.
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

    // Debug: dump post-optimization IR when DRAGON_DUMP_IR=opt. The Driver's pre-opt dump
    // (DRAGON_DUMP_IR=1) runs before this call and can't see the -O2 result; path optional via DRAGON_IR_FILE.
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

// ADR 041: runs a subprocess (compiler/linker driver) by argv, true on clean exit(0).
// POSIX uses fork/execvp (no shell, no injection), Windows uses _spawnvp; child stderr merges into stdout.
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

#if !defined(_WIN32)
// D031: runs a subprocess and captures its stdout (pkg-config queries for the webview
// shell); true on clean exit(0) with `out` holding the raw text. Same fork/execvp discipline as runTool.
static bool runToolCapture(const std::vector<std::string>& args,
                           std::string& out) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid == -1) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);  // execvp failed (tool not found on PATH)
    }
    close(fds[1]);
    out.clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, (size_t) n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Split pkg-config output into whitespace-separated flag tokens.
static std::vector<std::string> splitFlagTokens(const std::string& s) {
    std::vector<std::string> toks;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                                s[i] == '\r'))
            i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' &&
               s[j] != '\r')
            j++;
        if (j > i) toks.push_back(s.substr(i, j - i));
        i = j;
    }
    return toks;
}
#endif

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
    // ADR 041: compile any --cc-source FFI shims to temp objects first, so we know
    // whether a C++ TU is present before choosing the link driver. Temp objects anchor off objectFile's writable temp path.
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

    // D031 `import ui`: auto-compiles and links the platform webview shell (kept out of
    // the runtime archive so non-UI binaries skip GTK/webkit), resolving pkg-config flags when dragon_webview_* externs are referenced. A user --cc-source whose basename mentions "webview" skips the auto path.
#if defined(__APPLE__)
#define DRAGON_WEBVIEW_SHIM_NAME "platform/webview_macos.mm"
#else
#define DRAGON_WEBVIEW_SHIM_NAME "platform/webview_linux.cpp"
#endif
    std::vector<std::string> webviewLinkTokens;
#if !defined(_WIN32)
    if (impl_->needsWebview) {
        // D031 app:// assets: emit a TU with every assets/ file as byte arrays plus the
        // dragon_ui_assets[] table the shell's scheme handler resolves against. Always emitted (even empty) since the stock shell references the symbols unconditionally.
        {
            namespace fs = std::filesystem;
            struct AssetEntry {
                std::string rel;
                std::string array;
                unsigned long len;
            };
            std::vector<AssetEntry> entries;
            std::string assetSrc = objectFile + ".uiassets.cpp";
            std::ofstream out(assetSrc, std::ios::binary);
            out << "// generated by dragon: assets embedded for the app:// "
                   "scheme (D031)\n";
            out << "extern \"C\" {\n";
            out << "typedef struct { const char* path; const unsigned char* "
                   "data; unsigned long len; } DragonUiAsset;\n";
            if (!impl_->options.assetsDir.empty()) {
                // Deterministic order: collect, then sort by relative path.
                std::vector<fs::path> files;
                std::error_code ec;
                for (fs::recursive_directory_iterator
                         it(impl_->options.assetsDir, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    if (it->is_regular_file(ec)) files.push_back(it->path());
                }
                std::sort(files.begin(), files.end());
                size_t idx = 0;
                for (const auto& f : files) {
                    std::ifstream in(f, std::ios::binary);
                    if (!in) continue;
                    std::string bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
                    AssetEntry e;
                    e.rel = fs::relative(f, impl_->options.assetsDir, ec)
                                .generic_string();
                    if (ec) continue;
                    e.array = "dragon__ui_asset_" + std::to_string(idx++);
                    e.len = (unsigned long) bytes.size();
                    out << "static const unsigned char " << e.array << "[] = {";
                    for (size_t i = 0; i < bytes.size(); ++i) {
                        if (i % 24 == 0) out << "\n";
                        out << (unsigned) (unsigned char) bytes[i] << ",";
                    }
                    out << "0};\n";  // storage even for an empty file
                    entries.push_back(std::move(e));
                }
            }
            // `extern` + initializer: C++ gives namespace-scope const objects internal
            // linkage by default, which would leave the shell's references undefined at link time.
            out << "extern const DragonUiAsset dragon_ui_assets[] = {\n";
            for (const auto& e : entries) {
                out << "  {\"";
                for (char c : e.rel) {
                    if (c == '"' || c == '\\') out << '\\';
                    out << c;
                }
                out << "\", " << e.array << ", " << e.len << "},\n";
            }
            out << "  {0, 0, 0}\n};\n";
            out << "extern const unsigned long dragon_ui_asset_count = "
                << entries.size() << ";\n";
            out << "}\n";
            out.close();
            std::string assetObj = objectFile + ".uiassets.o";
            std::vector<std::string> cc = {"c++",  "-c", assetSrc, "-o",
                                           assetObj, "-fPIC"};
            bool ok = runTool(cc);
            std::remove(assetSrc.c_str());
            if (!ok) {
                impl_->addError(
                    "import ui: failed to compile the embedded-assets object");
                for (const auto& o : shimObjects) std::remove(o.c_str());
                return false;
            }
            shimObjects.push_back(assetObj);
            anyCxxShim = true;
        }
        bool userShell = false;
        for (const auto& src : impl_->options.ccSources) {
            auto slash = src.find_last_of("/\\");
            std::string base =
                slash == std::string::npos ? src : src.substr(slash + 1);
            if (base.find("webview") != std::string::npos) {
                userShell = true;
                break;
            }
        }
        if (!userShell) {
            if (impl_->options.webviewShimPath.empty()) {
                impl_->addError(
                    "import ui: cannot locate the webview shell source "
                    "(" DRAGON_WEBVIEW_SHIM_NAME ") in the platform/ tree "
                    "installed beside the stdlib");
                for (const auto& o : shimObjects) std::remove(o.c_str());
                return false;
            }
            std::string cflags, libs;
#if defined(__APPLE__)
            // The system WebKit and Cocoa are always present, so there is no
            // probe to run and no development package for the user to install.
            cflags = "-fobjc-arc";
            libs = "-framework Cocoa -framework WebKit";
#else
            // webkit2gtk-4.1 (libsoup3) is the primary target; 4.0 is API-compatible for
            // everything the shell uses and covers older distros that never shipped 4.1.
            std::string pkg;
            for (const char* cand : {"webkit2gtk-4.1", "webkit2gtk-4.0"}) {
                if (runTool({"pkg-config", "--exists", cand})) {
                    pkg = cand;
                    break;
                }
            }
            if (pkg.empty() ||
                !runToolCapture({"pkg-config", "--cflags", pkg}, cflags) ||
                !runToolCapture({"pkg-config", "--libs", pkg}, libs)) {
                impl_->addError(
                    "import ui: the desktop webview shell needs webkit2gtk "
                    "(pkg-config found neither webkit2gtk-4.1 nor "
                    "webkit2gtk-4.0). Install the development package "
                    "(Debian/Ubuntu: libwebkit2gtk-4.1-dev, Fedora: "
                    "webkit2gtk4.1-devel), or compile the shell yourself "
                    "with --cc-source platform/webview_linux.cpp "
                    "plus matching -I/-l flags (docs: 1802-windows)");
                for (const auto& o : shimObjects) std::remove(o.c_str());
                return false;
            }
#endif
            std::string shimObj = objectFile + ".webview.o";
            std::vector<std::string> cc;
            cc.push_back("c++");
            cc.push_back("-c");
            cc.push_back(impl_->options.webviewShimPath);
            cc.push_back("-o");
            cc.push_back(shimObj);
            cc.push_back("-fPIC");
            if (impl_->options.optimizationLevel > 0)
                cc.push_back("-O" +
                             std::to_string(impl_->options.optimizationLevel));
            for (const auto& tok : splitFlagTokens(cflags)) cc.push_back(tok);
            if (!runTool(cc)) {
                impl_->addError(
                    "import ui: failed to compile the webview shell: " +
                    impl_->options.webviewShimPath);
                for (const auto& o : shimObjects) std::remove(o.c_str());
                return false;
            }
            shimObjects.push_back(shimObj);
            anyCxxShim = true;
            webviewLinkTokens = splitFlagTokens(libs);
            impl_->needsPthread = true;  // the shell's loop serves fire/timers
        }
    }
#endif

    // Build argv for the compiler driver. A C++ shim forces the C++ driver so libstdc++,
    // static initializers, and exception/RTTI tables link correctly. Avoids shell injection via execvp (POSIX) / _spawnvp (Windows), which take argv directly.
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
    // DEBUG-ONLY (-DDRAGON_ASAN=ON): runtimeLibPath already points at the instrumented
    // runtime twin; add sanitizer flags so the binary is ASan-checked. Compiled out entirely in a normal build.
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
    // Link bundled mbedTLS if the program uses TLS, placed after the runtime archive so
    // the linker resolves runtime_tls.o's mbedtls_* references (mbedTLS is self-contained).
    if (!impl_->options.mbedtlsLibPath.empty() && impl_->needsMbedtls) {
        args.push_back(impl_->options.mbedtlsLibPath);
    }
    for (const auto& path : impl_->options.librarySearchPaths) {
        args.push_back("-L" + path);
    }
#if !defined(_WIN32)
    args.push_back("-lm");
#endif
    // zlib/zstd: dragon_runtime's compression entry points depend on system libz/libzstd.
    // Gate on needsZ/needsZstd (set when an extern decl references those prefixes) so unrelated programs skip the link, mirroring the sqlite3/pcre2 pattern.
#ifdef __APPLE__
    // Dev-tree fallback when no bundled archive resolved: arm64 brew's
    // /opt/homebrew is not on the default search path.
    if (impl_->needsZstd && impl_->options.zstdLibPath.empty()) {
        args.push_back("-L/opt/homebrew/lib");
        args.push_back("-L/usr/local/lib");
    }
#endif
    if (impl_->needsZ) {
        args.push_back("-lz");
    }
    // Bundled static archive on macOS (no system libzstd there); -lzstd on Linux.
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
    // D031: webkit2gtk link flags (pkg-config --libs) for `import ui`.
    for (const auto& tok : webviewLinkTokens) {
        args.push_back(tok);
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

// Visitor: Type Expressions (no-op in codegen)
void CodeGen::visit(NamedTypeExpr&) {}
void CodeGen::visit(GenericTypeExpr&) {}
void CodeGen::visit(OptionalTypeExpr&) {}
void CodeGen::visit(UnionTypeExpr&) {}
void CodeGen::visit(CallableTypeExpr&) {}
void CodeGen::visit(TupleTypeExpr&) {}

} // namespace dragon
