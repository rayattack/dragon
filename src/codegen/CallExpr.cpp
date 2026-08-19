#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(CallExpr& node) {
    if (callHasSpread(node)) {
        bool isTypedDict = false;
        if (auto* c = dynamic_cast<NameExpr*>(node.callee.get()))
            isTypedDict = impl_->typedDictClassesBySym.count(impl_->classSym(c->name)) > 0;
        bool isMethodCall = false;
        if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get()))
            isMethodCall = !(attr->object && attr->object->type &&
                             attr->object->type->kind() == Type::Kind::Module);
        if (!isTypedDict) {
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
            impl_->addError(
                "call-site spread (`*` / `**`) into this callable is not "
                "supported", node.location());
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return;
        }
    }

    if (auto* callee = dynamic_cast<NameExpr*>(node.callee.get())) {
        const std::string& name = callee->name;

        if (name == "super" && impl_->isDragonFile) {
            if (impl_->currentClassName.empty()) {
                impl_->addError("super(...) is only valid inside a method of a "
                    "class with a parent", node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }
            auto parentIt = impl_->classParentNamesBySym.find(
                impl_->classSym(impl_->currentClassName));
            if (parentIt == impl_->classParentNamesBySym.end()) {
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

        if (!nameIsUserBound && emitBuiltinCall(node, name)) return;

        if (impl_->classNames.count(name) && !impl_->currentClassName.empty()) {
            auto parentIt = impl_->classParentNamesBySym.find(
                impl_->classSym(impl_->currentClassName));
            if (parentIt != impl_->classParentNamesBySym.end() && parentIt->second == name) {
                std::string initName = impl_->classSymPrefix(name) + "___init__";
                auto* initFunc = impl_->module->getFunction(initName);
                if (initFunc) {
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

        if (impl_->typedDictClassesBySym.count(impl_->classSym(name))) {
            if (node.args.size() == 1) {
                node.args[0]->accept(*this);
                return;
            }
            if (!node.kwArgs.empty()) {
                if (node.kwArgs.size() == 1 && node.kwArgs[0].first.empty()) {
                    node.kwArgs[0].second->accept(*this);
                    llvm::Value* src = impl_->lastValue;
                    if (!src->getType()->isPointerTy())
                        src = impl_->builder->CreateIntToPtr(src, impl_->i8PtrType);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_copy"], {src}, "td");
                    return;
                }
                auto* cap = llvm::ConstantInt::get(impl_->i64Type, node.kwArgs.size());
                llvm::Value* dict = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_new"], {cap}, "td");
                for (auto& [kwName, kwVal] : node.kwArgs) {
                    kwVal->accept(*this);
                    llvm::Value* val = impl_->lastValue;
                    if (kwName.empty()) {
                        if (!val->getType()->isPointerTy())
                            val = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_dict_update"], {dict, val});
                        continue;
                    }
                    int64_t tag = TAG_INT;
                    auto schemaIt = impl_->typedDictFieldKindsBySym.find(impl_->classSym(name));
                    if (schemaIt != impl_->typedDictFieldKindsBySym.end()) {
                        auto fIt = schemaIt->second.find(kwName);
                        if (fIt != schemaIt->second.end())
                            tag = Impl::typeKindToTag(fIt->second);
                        if (tag < 0) tag = 0;
                    }
                    llvm::Value* boxTagVal = nullptr;
                    if (val->getType() == impl_->i1Type)
                        val = impl_->builder->CreateZExt(val, impl_->i64Type);
                    else if (val->getType() == impl_->f64Type)
                        val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                    else if (val->getType() == impl_->boxType) {
                        boxTagVal = impl_->boxTag(val, "td.tag");
                        llvm::Value* payload =
                            impl_->boxPayloadI64(val, "td.payload");
                        if (impl_->options.gcMode == GCMode::RC &&
                            !impl_->isOwnedBoxResult(val))
                            impl_->emitUnionIncref(payload, boxTagVal);
                        val = payload;
                    }
                    else if (val->getType()->isPointerTy()) {
                        auto adopted =
                            impl_->adoptPtrValueForTaggedDict(val, kwVal.get());
                        val = adopted.first;
                        tag = adopted.second;
                    }
                    auto* keyStr = impl_->builder->CreateGlobalString(kwName);
                    llvm::Value* tagVal =
                        boxTagVal ? boxTagVal
                                  : llvm::ConstantInt::get(impl_->i64Type, tag);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_tagged"],
                        {dict, keyStr, val, tagVal});
                }
                impl_->lastValue = dict;
                return;
            }
            auto* cap = llvm::ConstantInt::get(impl_->i64Type, 0);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_new"], {cap}, "td");
            return;
        }

        if (impl_->enumKindBySym.count(impl_->classSym(name)) && node.args.size() == 1) {
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

        const std::string ctorClassName =
            impl_->classNames.count(name) ? name : std::string();
        if (impl_->classNames.count(ctorClassName)) {
            if (impl_->decoratedClassesBySym.count(impl_->classSym(ctorClassName))) {
                impl_->addError(
                    "class decorators are not supported: classes are "
                    "compile-time entities and cannot be wrapped at runtime "
                    "(class '" + ctorClassName + "')",
                    node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return;
            }

            const std::string& name = ctorClassName;
            (void)name;

            if (impl_->stackAllocSites.count(&node) &&
                impl_->stackEligibleClassesBySym.count(impl_->classSym(name))) {
                auto stIt = impl_->classStructTypesBySym.find(impl_->classSym(name));
                auto* initFn = impl_->module->getFunction(
                    impl_->classSymPrefix(name) + "___init__");
                if (stIt != impl_->classStructTypesBySym.end() && initFn) {
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
                    auto* rcGEP = impl_->builder->CreateStructGEP(structType, self, 0, "rc_ptr");
                    impl_->builder->CreateStore(
                        llvm::ConstantInt::get(impl_->i64Type, 0x4000000000000000LL), rcGEP);

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

            const std::string ctorPrefix = impl_->classSymPrefix(name);
            std::string ctorName;
            auto ctorCountIt = impl_->classCtorCountBySym.find(impl_->classSym(name));
            if (ctorCountIt != impl_->classCtorCountBySym.end() && ctorCountIt->second > 1) {
                size_t callArity = node.args.size();
                auto& arityVec = impl_->classCtorAritiesBySym[impl_->classSym(name)];
                int matchedIdx = -1;
                for (auto& [arity, idx] : arityVec) {
                    if (arity == callArity) { matchedIdx = idx; break; }
                }
                if (matchedIdx >= 0) {
                    ctorName = ctorPrefix + "_new_" + std::to_string(matchedIdx);
                } else {
                    impl_->addError(
                        "internal error: no constructor overload of '" + name +
                        "' matches arity " + std::to_string(callArity) +
                        "; calling the first overload would pass the wrong "
                        "argument count",
                        node.location());
                    ctorName = ctorPrefix + "_new_" + std::to_string(arityVec[0].second);
                }
            } else {
                ctorName = ctorPrefix + "_new";
            }

            auto* ctorFunc = impl_->module->getFunction(ctorName);
            if (ctorFunc) {
                // Backstop: a missing param-kind entry means a half-built class
                // descriptor; emitting would double-free an adopted `own` +1. Fail loudly.
                if (impl_->options.gcMode == GCMode::RC && !node.args.empty() &&
                    !impl_->funcParamKinds.count(ctorName)) {
                    impl_->addError(
                        "internal: cannot construct '" + name +
                        "' here: its constructor descriptor is not finalized",
                        node.location());
                    return;
                }
                std::vector<llvm::Value*> args;
                std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                auto pkIt = impl_->funcParamKinds.find(ctorName);
                auto ctorFuncType = ctorFunc->getFunctionType();
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    // An own param adopts the arg's +1 (callee releases it); a
                    // caller-side drain here would double-free (A/B-proven).
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
                    if (i < ctorFuncType->getNumParams())
                        arg = impl_->coerceArgFromExpr(
                            node.args[i].get(), arg, ctorFuncType->getParamType(i));
                    args.push_back(arg);
                }
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
                            // own param adopts the +1 (as positional above); don't
                            // drain a kwarg bound to it or it double-frees.
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
                            args[idx] = impl_->coerceArgFromExpr(kwVal.get(), arg, paramTy);
                        }
                    }
                }
                impl_->fillDefaultArgs(ctorName, ctorFunc, args, *this, &argTemps);
                auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                impl_->lastValue = impl_->normalizeIntC(
                    impl_->builder->CreateCall(ctorFunc, args, "inst"));
                impl_->popArgTempCleanups(argTempBases);
                impl_->drainBorrowTemps(argTemps);
                impl_->emitMoveOutSlots(node);
                return;
            }
        }

        {
            auto decIt = impl_->decoratedFunctions.find(name);
            if (decIt != impl_->decoratedFunctions.end()) {
                auto* gv = decIt->second;
                auto* fnPtr = impl_->builder->CreateLoad(
                    impl_->i8PtrType, gv, name + ".decorated");

                llvm::FunctionType* fnType = nullptr;
                auto ctIt = impl_->callableTypes.find(name);
                if (ctIt != impl_->callableTypes.end()) {
                    fnType = ctIt->second;
                } else {
                    std::vector<llvm::Type*> pt(node.args.size(), impl_->i64Type);
                    fnType = llvm::FunctionType::get(impl_->i64Type, pt, false);
                }

                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    if (i < fnType->getNumParams())
                        arg = impl_->coerceArg(arg, fnType->getParamType(i));
                    args.push_back(arg);
                }

                emitCallableValueCall(fnPtr, fnType, args,
                                      false, "deccall");
                if (fnType->getReturnType() != impl_->voidType)
                    impl_->lastValue = impl_->normalizeIntC(impl_->lastValue);
                return;
            }
        }

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
            auto vaIt = impl_->funcVarArgInfo.find(func->getName().str());
            bool hasVarArgs = (vaIt != impl_->funcVarArgInfo.end());

            if (hasVarArgs) {
                emitVarArgCall(func, node);
                return;
            }

            std::vector<llvm::Value*> args;
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
                    argTemps.emplace_back(arg, Impl::VarKind::Closure);
                } else if (impl_->externDrainableFuncs.count(calleeSym)) {
                    // Extern callee: drain by the arg's own static type, not the
                    // param kind (a mis-declared type could decref an interior ptr - UAF).
                    Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[i].get(), arg);
                    if (dk != Impl::VarKind::Other)
                        argTemps.emplace_back(arg, dk);
                } else if (fpkIt != impl_->funcParamKinds.end() && i < fpkIt->second.size()) {
                    // own param adopts the arg's +1 (callee releases); a caller
                    // drain here would double-free (A/B-proven).
                    if (!impl_->paramIsOwn(calleeSym, (unsigned)i)) {
                        Impl::VarKind dk = impl_->argTempDecrefKind(
                            node.args[i].get(), fpkIt->second[i], arg);
                        if (dk != Impl::VarKind::Other)
                            argTemps.emplace_back(arg, dk);
                    }
                }
                llvm::Type* paramTy = funcType->getParamType(i);
                args.push_back(
                    impl_->coerceArgFromExpr(node.args[i].get(), arg, paramTy));
            }
            for (size_t i = funcType->getNumParams(); i < node.args.size(); ++i) {
                node.args[i]->accept(*this);
                args.push_back(impl_->lastValue);
            }
            if (!node.kwArgs.empty()) {
                auto pnIt = impl_->funcParamNames.find(func->getName().str());
                if (pnIt != impl_->funcParamNames.end()) {
                    const auto& paramNames = pnIt->second;
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
                        if (!impl_->paramIsOwn(calleeSym, (unsigned)idx) &&
                            fpkIt != impl_->funcParamKinds.end() &&
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
            impl_->fillDefaultArgs(func->getName().str(), func, args, *this, &argTemps);
            auto argTempBases = impl_->pushArgTempCleanups(argTemps);
            if (func->getReturnType() == impl_->voidType) {
                impl_->builder->CreateCall(func, args);
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
            } else {
                impl_->lastValue = impl_->normalizeIntC(
                    impl_->builder->CreateCall(func, args, "call"));
            }
            impl_->popArgTempCleanups(argTempBases);
            impl_->drainBorrowTemps(argTemps);
            impl_->emitMoveOutSlots(node);
            return;
        }

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

        {
            auto varKind = impl_->lookupVarKind(name);
            if (varKind == Impl::VarKind::ClassInstance) {
                auto cit = impl_->varClassNames.find(name);
                if (cit != impl_->varClassNames.end() && impl_->hasDunder(cit->second, "__call__")) {
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
                        if (!objPtr->getType()->isPointerTy())
                            objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);

                        std::string defClass = impl_->findDunderClass(cit->second, "__call__");
                        std::string funcName = defClass + "___call__";
                        auto* callFunc = impl_->module->getFunction(funcName);

                        std::vector<llvm::Value*> extraArgs;
                        for (size_t i = 0; i < node.args.size(); ++i) {
                            node.args[i]->accept(*this);
                            llvm::Value* arg = impl_->lastValue;
                            if (callFunc && (i + 1) < callFunc->getFunctionType()->getNumParams())
                                arg = impl_->coerceArg(arg, callFunc->getFunctionType()->getParamType(i + 1));
                            extraArgs.push_back(arg);
                        }

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

        {
            llvm::Value* calleePtrStorage = nullptr;
            llvm::Type* loadType = nullptr;

            auto* alloca = impl_->lookupVar(name);
            if (alloca) {
                calleePtrStorage = alloca;
                loadType = alloca->getAllocatedType();
            }
            if (!calleePtrStorage) {
                auto* gv = impl_->lookupModuleGlobal(name);
                if (gv && impl_->shouldUseModuleGlobal(name)) {
                    calleePtrStorage = gv;
                    loadType = gv->getValueType();
                }
            }

            if (calleePtrStorage) {
                llvm::Value* calleeVal = impl_->builder->CreateLoad(
                    loadType, calleePtrStorage, name + ".load");

                auto calleeKind = impl_->lookupVarKind(name);
                if (calleeKind == Impl::VarKind::Closure) {
                    llvm::FunctionType* userFnType = nullptr;
                    auto ctIt = impl_->callableTypes.find(name);
                    if (ctIt != impl_->callableTypes.end()) {
                        userFnType = ctIt->second;
                    } else {
                        std::vector<llvm::Type*> pt(node.args.size(), impl_->i64Type);
                        userFnType = llvm::FunctionType::get(impl_->i64Type, pt, false);
                    }

                    std::vector<llvm::Value*> args;
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        if (i < userFnType->getNumParams())
                            arg = impl_->coerceArg(arg, userFnType->getParamType(i));
                        args.push_back(arg);
                    }

                    emitCallableValueCall(calleeVal, userFnType, args,
                                          false, name);
                    return;
                }

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
                const bool valIsTypedCallable = (ctIt != impl_->callableTypes.end());

                llvm::Value* fnPtr = calleeVal;
                if (!calleeVal->getType()->isPointerTy()) {
                    fnPtr = impl_->builder->CreateIntToPtr(
                        calleeVal, llvm::PointerType::getUnqual(*impl_->context));
                }

                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    if (i < fnType->getNumParams())
                        arg = impl_->coerceArg(arg, fnType->getParamType(i));
                    args.push_back(arg);
                }

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

    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        if (attr->object && attr->object->type &&
            attr->object->type->kind() == Type::Kind::Module) {
            if (impl_->classNames.count(attr->attribute)) {
                const std::string& srcModuleName =
                    static_cast<ModuleType&>(*attr->object->type).name;
                const std::string ctorPrefix =
                    Impl::mangleClass(srcModuleName, attr->attribute);
                std::string ctorName;
                auto ctorCountIt = impl_->classCtorCountBySym.find(ctorPrefix);
                if (ctorCountIt != impl_->classCtorCountBySym.end() && ctorCountIt->second > 1) {
                    size_t callArity = node.args.size();
                    auto& arityVec = impl_->classCtorAritiesBySym[ctorPrefix];
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
                    auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(ctorFunc, args, "inst"));
                    impl_->popArgTempCleanups(argTempBases);
                    impl_->drainBorrowTemps(argTemps);
                    impl_->emitMoveOutSlots(node);
                    return;
                }
            }
            const std::string& srcModuleName =
                static_cast<ModuleType&>(*attr->object->type).name;
            const std::string mangled = Impl::mangleFunc(srcModuleName, attr->attribute);
            llvm::Function* func = impl_->module->getFunction(mangled);
            if (!func) {
                func = impl_->module->getFunction(Impl::userFuncName(attr->attribute));
            }
            if (func) {
                if (impl_->funcVarArgInfo.count(func->getName().str())) {
                    emitVarArgCall(func, node);
                    return;
                }
                auto funcType = func->getFunctionType();
                std::vector<llvm::Value*> args;
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
                    // Extern callee: drain by the arg's own static type, not the
                    // param kind (mis-declared types risk an interior-ptr UAF).
                    if (impl_->externDrainableFuncs.count(calleeSym2)) {
                        Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[i].get(), arg);
                        if (dk != Impl::VarKind::Other)
                            argTemps.emplace_back(arg, dk);
                    } else if (fpkIt != impl_->funcParamKinds.end() && i < fpkIt->second.size()) {
                        if (!impl_->paramIsOwn(calleeSym2, (unsigned)i)) {
                            Impl::VarKind dk = impl_->argTempDecrefKind(
                                node.args[i].get(), fpkIt->second[i], arg);
                            if (dk != Impl::VarKind::Other)
                                argTemps.emplace_back(arg, dk);
                        }
                    }
                    if (i < funcType->getNumParams()) {
                        arg = impl_->coerceArgFromExpr(
                            node.args[i].get(), arg, funcType->getParamType(i));
                    }
                    args.push_back(arg);
                }
                impl_->fillDefaultArgs(func->getName().str(), func, args, *this, &argTemps);
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
                impl_->drainBorrowTemps(argTemps);
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

    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        if (emitMethodCall(node, *attr)) return;
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

    if (!dynamic_cast<AttributeExpr*>(node.callee.get()) &&
        node.callee->type && node.callee->type->kind() == Type::Kind::Function) {
        auto& fnTy = static_cast<FunctionType&>(*node.callee->type);

        bool closureOwned =
            dynamic_cast<CallExpr*>(node.callee.get()) != nullptr;

        std::vector<llvm::Type*> userParamTypes;
        userParamTypes.reserve(fnTy.paramTypes.size());
        for (auto& p : fnTy.paramTypes)
            userParamTypes.push_back(
                impl_->typeKindToLLVM(p ? p->kind() : Type::Kind::Int));
        llvm::Type* userRet = fnTy.returnType
            ? impl_->typeKindToLLVM(fnTy.returnType->kind()) : impl_->i64Type;
        auto* userFnType = llvm::FunctionType::get(userRet, userParamTypes, false);

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

    impl_->addError(
        "internal error: no call dispatch path matched this callee; the "
        "front end should have rejected it",
        node.location());
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}

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

    std::vector<llvm::Type*> closParamTypes(
        userFnType->param_begin(), userFnType->param_end());
    closParamTypes.push_back(impl_->i8PtrType);
    auto* closFnType = llvm::FunctionType::get(
        userFnType->getReturnType(), closParamTypes, false);

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

    if (ownedClosure && impl_->options.gcMode == GCMode::RC) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_decref_callable"], {fnPtr});
    }
}

void CodeGen::emitVarArgCall(llvm::Function* func, CallExpr& node) {
    auto vaIt = impl_->funcVarArgInfo.find(func->getName().str());
    if (vaIt == impl_->funcVarArgInfo.end()) return;
    auto& vaInfo = vaIt->second;
    std::vector<llvm::Value*> args;
    auto funcType = func->getFunctionType();

    auto spreadFail = [&](const std::string& msg, SourceLocation loc) {
        impl_->addError(msg, loc);
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
    };

    size_t llvmIdx = 0;
    for (size_t i = 0; i < vaInfo.numRegularParams && i < node.args.size(); ++i) {
        if (dynamic_cast<StarredExpr*>(node.args[i].get())) {
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

    while (args.size() < vaInfo.numRegularParams) {
        args.push_back(nullptr);
        llvmIdx++;
    }

    llvm::Value* packedArgsList = nullptr;
    llvm::Value* packedKwargsDict = nullptr;

    if (vaInfo.hasVarArg) {
        size_t extraCount = (node.args.size() > vaInfo.numRegularParams)
            ? node.args.size() - vaInfo.numRegularParams : 0;
        auto* cap = llvm::ConstantInt::get(impl_->i64Type, (int64_t)extraCount);
        llvm::Value* argsList = impl_->emitNewTypedList(
            vaInfo.varArgElemTag, vaInfo.varArgElemIsAny, cap);
        for (size_t i = vaInfo.numRegularParams; i < node.args.size(); ++i) {
            if (auto* st = dynamic_cast<StarredExpr*>(node.args[i].get())) {
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
    llvm::Value* packedArgsCleanupBase = nullptr;
    if (packedArgsList)
        packedArgsCleanupBase =
            impl_->emitCleanupPushTemp(packedArgsList, Impl::DCLEAN_OBJ);

    std::vector<bool> kwConsumed(node.kwArgs.size(), false);
    if (!node.kwArgs.empty()) {
        auto pnIt = impl_->funcParamNames.find(func->getName().str());
        if (pnIt != impl_->funcParamNames.end()) {
            const auto& paramNames = pnIt->second;
            for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
                const std::string& kwName = node.kwArgs[ki].first;
                if (kwName.empty()) continue;
                auto nameIt = std::find(paramNames.begin(), paramNames.end(), kwName);
                if (nameIt == paramNames.end()) continue;
                size_t idx = (size_t)std::distance(paramNames.begin(), nameIt);
                if (idx >= vaInfo.numRegularParams) continue;
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

    std::string dispName = "function '" + func->getName().str() + "'";
    if (auto* cn = dynamic_cast<NameExpr*>(node.callee.get()))
        dispName = "function '" + cn->name + "'";
    else if (auto* ca = dynamic_cast<AttributeExpr*>(node.callee.get()))
        dispName = "function '" + ca->attribute + "'";
    llvm::Value* spreadSrc = nullptr;
    Expr* spreadSrcExpr = nullptr;
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
    llvm::Value* spreadCleanupBase = nullptr;
    if (spreadSrc && spreadSrcExpr &&
        impl_->ownedTempDrainKind(spreadSrcExpr, spreadSrc) != Impl::VarKind::Other)
        spreadCleanupBase = impl_->emitCleanupPushTemp(spreadSrc, Impl::DCLEAN_OBJ);
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
            auto [arrPtr, n] = buildNamesArray(regularNames);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_reject_unknown_keys"],
                {spreadSrc, arrPtr,
                 llvm::ConstantInt::get(impl_->i64Type, n),
                 impl_->builder->CreateGlobalString(dispName)});
        }
    }

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
            if (kwConsumed[ki]) continue;
            const std::string& kwName = node.kwArgs[ki].first;
            if (kwName.empty()) continue;
            if (spreadSrc) emitSpreadDupCheck(kwName);
            node.kwArgs[ki].second->accept(*this);
            llvm::Value* val = impl_->lastValue;
            int64_t tag = TAG_INT;
            llvm::Value* tagVal = nullptr;
            if (val->getType() == impl_->i1Type) {
                tag = TAG_BOOL;
                val = impl_->builder->CreateZExt(val, impl_->i64Type);
            } else if (val->getType() == impl_->f64Type) {
                tag = TAG_FLOAT;
                val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            } else if (val->getType() == impl_->boxType) {
                tagVal = impl_->boxTag(val, "kw.tag");
                llvm::Value* payload = impl_->boxPayloadI64(val, "kw.payload");
                if (impl_->options.gcMode == GCMode::RC &&
                    !impl_->isOwnedBoxResult(val))
                    impl_->emitUnionIncref(payload, tagVal);
                val = payload;
            } else if (val->getType()->isPointerTy()) {
                // The dict-set adopts one ref, so incref a borrowed source (f(a=s))
                // or the dict frees the caller's string (UAF); owned temps already carry +1.
                auto adopted = impl_->adoptPtrValueForTaggedDict(
                    val, node.kwArgs[ki].second.get());
                val = adopted.first;
                tag = adopted.second;
            }
            auto* keyStr = impl_->builder->CreateGlobalString(kwName);
            if (!tagVal) tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_set_tagged"],
                {kwargsDict, keyStr, val, tagVal});
        }
        args.push_back(kwargsDict);
        packedKwargsDict = kwargsDict;
        llvmIdx++;
    } else {
        for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
            if (!kwConsumed[ki] && !node.kwArgs[ki].first.empty()) {
                spreadFail("function got an unexpected keyword argument '" +
                           node.kwArgs[ki].first + "'", node.location());
                return;
            }
        }
    }

    impl_->fillDefaultArgs(func->getName().str(), func, args, *this);

    if (func->getReturnType() == impl_->voidType) {
        impl_->builder->CreateCall(func, args);
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
    } else {
        impl_->lastValue = impl_->normalizeIntC(
            impl_->builder->CreateCall(func, args, "call"));
    }

    impl_->emitMoveOutSlots(node);

    if (spreadCleanupBase) impl_->emitCleanupPopTemp(spreadCleanupBase);
    if (packedArgsCleanupBase) impl_->emitCleanupPopTemp(packedArgsCleanupBase);

    if (packedArgsList)
        impl_->emitDecrefByKind(packedArgsList, Impl::VarKind::List);
    if (packedKwargsDict)
        impl_->emitDecrefByKind(packedKwargsDict, Impl::VarKind::Dict);
    if (spreadSrc && spreadSrcExpr) {
        Impl::VarKind dk = impl_->ownedTempDrainKind(spreadSrcExpr, spreadSrc);
        if (dk != Impl::VarKind::Other)
            impl_->emitDecrefByKind(spreadSrc, dk);
    }
}

bool CodeGen::callHasStarArg(CallExpr& node) {
    for (auto& a : node.args)
        if (dynamic_cast<StarredExpr*>(a.get())) return true;
    return false;
}

bool CodeGen::callHasSpread(CallExpr& node) {
    if (callHasStarArg(node)) return true;
    for (auto& kw : node.kwArgs)
        if (kw.first.empty()) return true;
    return false;
}

bool CodeGen::emitSpreadDispatch(CallExpr& node) {
    if (auto* callee = dynamic_cast<NameExpr*>(node.callee.get())) {
        const std::string& name = callee->name;

        if (impl_->classNames.count(name) &&
            !impl_->typedDictClassesBySym.count(impl_->classSym(name)) &&
            !impl_->decoratedClassesBySym.count(impl_->classSym(name))) {
            const std::string ctorPrefix = impl_->classSymPrefix(name);
            std::string ctorName;
            auto ctorCountIt = impl_->classCtorCountBySym.find(impl_->classSym(name));
            if (ctorCountIt != impl_->classCtorCountBySym.end() &&
                ctorCountIt->second > 1) {
                int64_t arity = -1;
                if (spreadStaticArity(node, arity)) {
                    auto& arityVec = impl_->classCtorAritiesBySym[impl_->classSym(name)];
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
                emitSpreadCall(ctorFunc, node, {}, "class '" + name + "' ctor");
                return true;
            }
            return false;
        }

        if (impl_->decoratedFunctions.count(name) ||
            impl_->nestedFunctionAliases.count(name))
            return false;

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
            if (impl_->funcVarArgInfo.count(func->getName().str())) {
                emitVarArgCall(func, node);
                return true;
            }
            emitSpreadCall(func, node, {}, "function '" + name + "'");
            return true;
        }
        return false;
    }

    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        if (attr->object && attr->object->type &&
            attr->object->type->kind() == Type::Kind::Module) {
            const std::string& srcModuleName =
                static_cast<ModuleType&>(*attr->object->type).name;
            if (impl_->classNames.count(attr->attribute) &&
                !impl_->typedDictClassesBySym.count(
                    Impl::mangleClass(srcModuleName, attr->attribute))) {
                const std::string ctorPrefix =
                    Impl::mangleClass(srcModuleName, attr->attribute);
                std::string ctorName;
                auto ctorCountIt = impl_->classCtorCountBySym.find(ctorPrefix);
                if (ctorCountIt != impl_->classCtorCountBySym.end() &&
                    ctorCountIt->second > 1) {
                    int64_t arity = -1;
                    if (spreadStaticArity(node, arity)) {
                        auto& arityVec = impl_->classCtorAritiesBySym[ctorPrefix];
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

bool CodeGen::spreadStaticArity(CallExpr& node, int64_t& arityOut) {
    for (auto& kw : node.kwArgs)
        if (kw.first.empty()) return false;
    int64_t total = 0;
    for (auto& a : node.args) {
        if (auto* st = dynamic_cast<StarredExpr*>(a.get())) {
            auto ty = st->value ? st->value->type : nullptr;
            if (ty && ty->kind() == Type::Kind::Tuple)
                total += (int64_t)static_cast<TupleType&>(*ty).elementTypes.size();
            else
                return false;
        } else {
            total += 1;
        }
    }
    arityOut = total + (int64_t)node.kwArgs.size();
    return true;
}

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

    for (auto& a : node.args) {
        if (auto* st = dynamic_cast<StarredExpr*>(a.get())) {
            st->value->accept(cg);
            llvm::Value* src = lastValue;
            if (!src->getType()->isPointerTy())
                src = builder->CreateIntToPtr(src, i8PtrType);
            auto srcTy = st->value->type;
            if (auto* tt = dynamic_cast<TupleType*>(srcTy.get())) {
                size_t L = tt->elementTypes.size();
                for (size_t k = 0; k < L; ++k) {
                    auto* idx = llvm::ConstantInt::get(i64Type, (int64_t)k);
                    llvm::Value* raw = builder->CreateCall(
                        runtimeFuncs["dragon_tuple_get"], {src, idx},
                        "spread.elem");
                    auto et = tt->elementTypes[k];
                    llvm::Value* native = containerSlotToNative(raw, et.get());
                    pushPositional(native, nullptr, true, et);
                }
            } else if (auto* lt = dynamic_cast<ListType*>(srcTy.get())) {
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
                        llvm::Value* raw = builder->CreateCall(
                            runtimeFuncs["dragon_list_get"], {src, idx},
                            "spread.elem");
                        elem = (ek == Type::Kind::Bool)
                            ? containerSlotToNative(raw, et.get())
                            : raw;
                    }
                    pushPositional(elem, nullptr, true, et);
                }
            } else {
                return fail("cannot spread a non-tuple/non-list value",
                            st->location());
            }
        } else {
            a->accept(cg);
            pushPositional(lastValue, a.get(), false, nullptr);
        }
    }

    if (!node.kwArgs.empty()) {
        if (args.size() < numParams) args.resize(numParams, nullptr);
        auto pnIt = funcParamNames.find(symName);
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
            if (!paramIsOwn(symName, (unsigned)idx) &&
                fpkIt != funcParamKinds.end() && idx < fpkIt->second.size()) {
                VarKind dk = argTempDecrefKind(kwVal.get(), fpkIt->second[idx], v);
                if (dk != VarKind::Other) argTemps.emplace_back(v, dk);
            }
            llvm::Type* paramTy = funcType->getParamType((unsigned)idx);
            args[idx] = coerceArgFromExpr(kwVal.get(), v, paramTy);
        }

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
            {
                VarKind spreadDk = ownedTempDrainKind(dictSpread, d);
                if (spreadDk != VarKind::Other) argTemps.emplace_back(d, spreadDk);
            }
            auto defIt = funcParamDefaults.find(symName);

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
            int64_t tag = TAG_STR;
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
        return;

    impl_->fillDefaultArgs(func->getName().str(), func, args, *this, &argTemps);
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
    impl_->drainBorrowTemps(argTemps);
    impl_->emitMoveOutSlots(node);
}

}
