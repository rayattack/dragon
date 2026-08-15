/// Dragon CodeGen - builtin function call dispatch.
#include "../CodeGenImpl.h"

namespace dragon {


// Print one argument through the per-type `_raw` printers; the caller inserts
// separators and the trailing newline.
void CodeGen::emitPrintArgRaw(Expr* argExpr) {
    argExpr->accept(*this);
    llvm::Value* arg = impl_->lastValue;
    llvm::Type* argType = arg->getType();

    // D039: a box-typed arg (Union/Any local, box element) tag-switches to the
    // right per-type printer.
    if (argType == impl_->boxType) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_box_raw"], {arg});
        // print borrows: without this an owned box temp (`anyA + anyB`) leaks
        // its payload. isOwnedBoxResult rejects borrowed box reads.
        if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedBoxResult(arg)) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_decref"], {arg});
        }
        return;
    }

    // A statically container/instance-typed value must take the general dispatch
    // below; the tag/str cases render a dict[int, list[int]] value as blank.
    bool staticContainerVal = false;
    if (argExpr->type) {
        auto svk = argExpr->type->kind();
        staticContainerVal = (svk == Type::Kind::List || svk == Type::Kind::Dict ||
                              svk == Type::Kind::Tuple || svk == Type::Kind::Set ||
                              svk == Type::Kind::Instance);
    }

    // Dict subscript / dot-access: the runtime value tag picks the printer.
    if (auto* subscript = dynamic_cast<SubscriptExpr*>(argExpr)) {
        bool isSubDict = dynamic_cast<DictExpr*>(subscript->object.get()) != nullptr;
        if (!isSubDict) {
            if (auto* sn = dynamic_cast<NameExpr*>(subscript->object.get())) {
                isSubDict = impl_->lookupVarKind(sn->name) == Impl::VarKind::Dict;
            }
        }
        if (isSubDict && !staticContainerVal) {
            if (argType == impl_->f64Type) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_print_float_raw"], {arg});
                return;
            }
            if (argType->isPointerTy() && argType != impl_->i64Type) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_print_str_raw"], {arg});
                return;
            }
            Type::Kind subDictKk =
                impl_->resolveDictKeyKind(subscript->object.get());
            bool intKeyedSub = subDictKk == Type::Kind::Int ||
                               subDictKk == Type::Kind::Float;
            subscript->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            subscript->index->accept(*this);
            llvm::Value* key = impl_->lastValue;
            if (intKeyedSub) {
                if (subDictKk == Type::Kind::Float)
                    key = impl_->emitFloatDictKeyBits(key);
                if (key->getType() == impl_->i1Type)
                    key = impl_->builder->CreateZExt(key, impl_->i64Type);
                else if (key->getType()->isPointerTy())
                    key = impl_->builder->CreatePtrToInt(key, impl_->i64Type);
                else if (key->getType() != impl_->i64Type)
                    key = impl_->builder->CreateZExtOrTrunc(key, impl_->i64Type);
                auto* tag = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get_tag"], {dict, key}, "dtag.i");
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_print_tagged_raw"], {arg, tag});
            } else {
                auto* tag = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_tag"], {dict, key}, "dtag");
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_print_tagged_raw"], {arg, tag});
            }
            return;
        }
    }
    if (auto* dotAccess = dynamic_cast<AttributeExpr*>(argExpr)) {
        if (auto* objName = dynamic_cast<NameExpr*>(dotAccess->object.get())) {
            if (impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict && !staticContainerVal) {
                if (argType == impl_->f64Type) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_print_float_raw"], {arg});
                    return;
                }
                if (argType->isPointerTy() && argType != impl_->i64Type) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_print_str_raw"], {arg});
                    return;
                }
                dotAccess->object->accept(*this);
                llvm::Value* dict = impl_->lastValue;
                auto* keyStr = impl_->builder->CreateGlobalString(dotAccess->attribute);
                auto* tag = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_tag"], {dict, keyStr}, "dtag");
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_print_tagged_raw"], {arg, tag});
                return;
            }
        }
    }

    if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
        auto vk = impl_->lookupVarKind(argName->name);
        // Classes are compile-time entities (D021), so `print(SomeClass)` is an
        // error. Exception names lower to int type codes, never VarKind::Type.
        if (vk == Impl::VarKind::Type ||
            (impl_->classNames.count(argName->name) &&
             !impl_->isExcType(argName->name))) {
            impl_->addError(
                "classes are not values: cannot print '" + argName->name +
                "' (classes are compile-time entities; there is no class value "
                "to print).",
                argExpr->location());
            return;
        }
    }

    // D030: Union arg - switch on the box tag, extracting the payload at the
    // right native type per branch.
    if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
        if (impl_->lookupVarKind(argName->name) == Impl::VarKind::Union &&
            arg->getType() == impl_->boxType) {
            auto* tag = impl_->boxTag(arg, "print.tag");
            auto* payload = impl_->boxPayloadI64(arg, "print.payload");
            auto* func2 = impl_->currentFunction;
            auto* mergePrint = llvm::BasicBlock::Create(
                *impl_->context, "print.union.end", func2);
            auto* defaultBB = llvm::BasicBlock::Create(
                *impl_->context, "print.union.default", func2);
            auto* sw = impl_->builder->CreateSwitch(tag, defaultBB, 6);

            auto* intBB = llvm::BasicBlock::Create(*impl_->context, "print.int", func2);
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, 0)), intBB);
            impl_->builder->SetInsertPoint(intBB);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_int_raw"], {payload});
            impl_->builder->CreateBr(mergePrint);

            auto* strBB = llvm::BasicBlock::Create(*impl_->context, "print.str", func2);
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, 1)), strBB);
            impl_->builder->SetInsertPoint(strBB);
            auto* strPtr = impl_->builder->CreateIntToPtr(payload, impl_->i8PtrType);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_str_raw"], {strPtr});
            impl_->builder->CreateBr(mergePrint);

            auto* floatBB = llvm::BasicBlock::Create(*impl_->context, "print.float", func2);
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, 2)), floatBB);
            impl_->builder->SetInsertPoint(floatBB);
            auto* floatVal = impl_->builder->CreateBitCast(payload, impl_->f64Type);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_float_raw"], {floatVal});
            impl_->builder->CreateBr(mergePrint);

            auto* boolBB = llvm::BasicBlock::Create(*impl_->context, "print.bool", func2);
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, 3)), boolBB);
            impl_->builder->SetInsertPoint(boolBB);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_bool_raw"], {payload});
            impl_->builder->CreateBr(mergePrint);

            auto* listBB = llvm::BasicBlock::Create(*impl_->context, "print.list", func2);
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, 5)), listBB);
            impl_->builder->SetInsertPoint(listBB);
            auto* listPtr = impl_->builder->CreateIntToPtr(payload, impl_->i8PtrType);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_list_int_raw"], {listPtr});
            impl_->builder->CreateBr(mergePrint);

            auto* dictBB = llvm::BasicBlock::Create(*impl_->context, "print.dict", func2);
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, 6)), dictBB);
            impl_->builder->SetInsertPoint(dictBB);
            auto* dictPtr = impl_->builder->CreateIntToPtr(payload, impl_->i8PtrType);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_dict_raw"], {dictPtr});
            impl_->builder->CreateBr(mergePrint);

            impl_->builder->SetInsertPoint(defaultBB);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_int_raw"], {payload});
            impl_->builder->CreateBr(mergePrint);

            impl_->builder->SetInsertPoint(mergePrint);
            return;
        }
    }

    // Container detection for print dispatch.
    bool isPrintDict = dynamic_cast<DictExpr*>(argExpr) != nullptr;
    if (!isPrintDict) {
        if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
            isPrintDict = impl_->lookupVarKind(argName->name) == Impl::VarKind::Dict;
        }
    }
    // D030: bytes detection leads - bytes-typed slots collapse onto
    // VarKind::List, so the bare List check would misroute print(bytes).
    bool isPrintBytes = impl_->exprIsBytes(argExpr);
    bool isPrintList = !isPrintBytes && dynamic_cast<ListExpr*>(argExpr) != nullptr;
    if (!isPrintList && !isPrintBytes) {
        if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
            isPrintList = impl_->lookupVarKind(argName->name) == Impl::VarKind::List;
        }
    }
    bool isPrintTuple = dynamic_cast<TupleExpr*>(argExpr) != nullptr;
    if (!isPrintTuple) {
        if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
            isPrintTuple = impl_->lookupVarKind(argName->name) == Impl::VarKind::Tuple;
        }
    }
    bool isPrintSet = dynamic_cast<SetExpr*>(argExpr) != nullptr;
    if (!isPrintSet) {
        if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
            isPrintSet = impl_->lookupVarKind(argName->name) == Impl::VarKind::Set;
        }
    }
    // An inline set-method result (`a.union(b)`) must beat the type-kind
    // fallback, which maps copy()'s ListType to isPrintList (list brackets).
    if (!isPrintSet && impl_->resolveExprVarKind(argExpr) == Impl::VarKind::Set)
        isPrintSet = true;
    // Deque precedes the fallback too: it is typed ListType, and a list printer
    // reads the DragonDeque header as raw-pointer garbage.
    bool isPrintDeque = false;
    if (auto* argName = dynamic_cast<NameExpr*>(argExpr)) {
        isPrintDeque = impl_->lookupVarKind(argName->name) == Impl::VarKind::Deque;
        if (!isPrintDeque) {
            auto dqIt = impl_->varClassNames.find(argName->name);
            isPrintDeque = dqIt != impl_->varClassNames.end() &&
                           dqIt->second == "__Deque";
        }
    }
    // Fallback: typechecker-propagated argument type (subscripts, call results).
    if (!isPrintBytes && !isPrintList && !isPrintDict && !isPrintTuple && !isPrintSet &&
        !isPrintDeque && argExpr->type) {
        switch (argExpr->type->kind()) {
            case Type::Kind::List:  isPrintList  = true; break;
            case Type::Kind::Dict:  isPrintDict  = true; break;
            case Type::Kind::Tuple: isPrintTuple = true; break;
            case Type::Kind::Set:   isPrintSet   = true; break;
            default: break;
        }
    }

    std::string printClassName = impl_->resolveExprClassName(argExpr);

    if (isPrintTuple && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_tuple_raw"], {arg});
    } else if (isPrintDeque && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_deque_raw"], {arg});
    } else if (isPrintSet && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_set_raw"], {arg});
    } else if (isPrintDict && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        bool keyIsInt = impl_->dictKeyUsesIntEngine(argExpr);
        bool valIsNested = false;  // dict value is itself a container
        if (auto* dt = dynamic_cast<DictType*>(argExpr->type.get())) {
            if (dt->valueType) {
                auto vk = dt->valueType->kind();
                valIsNested = vk == Type::Kind::List || vk == Type::Kind::Dict ||
                              vk == Type::Kind::Tuple || vk == Type::Kind::Set;
            }
        }
        std::string printDictFn =
            valIsNested ? (keyIsInt ? "dragon_print_dict_int_nested_raw"
                                    : "dragon_print_dict_nested_raw")
                        : (keyIsInt ? "dragon_print_dict_int_raw"
                                    : "dragon_print_dict_raw");
        impl_->builder->CreateCall(impl_->runtimeFuncs[printDictFn], {arg});
    } else if (isPrintList && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        std::string printFn = "dragon_print_list_int_raw";
        if (argExpr && argExpr->type) {
            if (auto* lt = dynamic_cast<ListType*>(argExpr->type.get())) {
                if (lt->elementType) {
                    auto ek = lt->elementType->kind();
                    if (ek == Type::Kind::Str) printFn = "dragon_print_list_str_raw";
                    else if (ek == Type::Kind::Float) printFn = "dragon_print_list_float_raw";
                    else if (ek == Type::Kind::Bool) printFn = "dragon_print_list_bool_raw";
                    else if (ek == Type::Kind::Any) printFn = "dragon_print_list_box_raw";
                    // Container elements render recursively; the int printer
                    // would show the element pointers as integers.
                    else if (ek == Type::Kind::List || ek == Type::Kind::Dict ||
                             ek == Type::Kind::Tuple || ek == Type::Kind::Set)
                        printFn = "dragon_print_list_nested_raw";
                }
            }
        }
        if (printFn == "dragon_print_list_int_raw") {
            if (auto* nameArg = dynamic_cast<NameExpr*>(argExpr)) {
                auto vit = impl_->varListElemKinds.find(nameArg->name);
                if (vit != impl_->varListElemKinds.end() &&
                    vit->second == Type::Kind::Any)
                    printFn = "dragon_print_list_box_raw";
            }
        }
        impl_->builder->CreateCall(impl_->runtimeFuncs[printFn], {arg});
    } else if (!printClassName.empty() && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        if (impl_->hasDunder(printClassName, "__str__")) {
            auto* strResult = impl_->callDunder(printClassName, "__str__", arg);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_print_str_raw"], {strResult});
        } else if (impl_->hasDunder(printClassName, "__repr__")) {
            auto* reprResult = impl_->callDunder(printClassName, "__repr__", arg);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_print_str_raw"], {reprResult});
        } else if (impl_->userExcCodesBySym.count(impl_->classSym(printClassName)) > 0) {
            // Exception with no __str__/__repr__: `str(e)` is args[0], which
            // the raise snapshots into the runtime msg slot a handler reads.
            auto* msg = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "exc.msg");
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_print_str_raw"], {msg});
        } else {
            std::string repr = "<" + printClassName + " instance>";
            auto* reprStr = impl_->builder->CreateGlobalString(repr, "class_repr");
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_print_str_raw"], {reprStr});
        }
    } else if (argType == impl_->i64Type) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_int_raw"], {arg});
    } else if (argType == impl_->f64Type) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_float_raw"], {arg});
    } else if (isPrintBytes && (argType == impl_->i8PtrType || argType->isPointerTy())) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_bytes_raw"], {arg});
    } else if (argType == impl_->i8PtrType || argType->isPointerTy()) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_str_raw"], {arg});
        // print borrows: without this a loop over `print(a + b)` grows RSS
        // unbounded. isOwnedStrResult rejects borrowed reads and literals.
        if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedStrResult(arg)) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {arg});
        }
    } else if (argType == impl_->i1Type) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_bool_raw"],
            {impl_->builder->CreateZExt(arg, impl_->i64Type)});
    } else {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_int_raw"], {arg});
    }

    // Same for an owned container temp: `print(list(filter(f, xs)))` leaks the
    // whole container per call. isOwnedPtrResult rejects borrowed args.
    if (impl_->options.gcMode == GCMode::RC && staticContainerVal &&
        arg->getType()->isPointerTy() && impl_->isOwnedPtrResult(arg)) {
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {arg});
    }
}

bool CodeGen::emitBuiltinCall(CallExpr& node, const std::string& name) {
    // Owned temps in a borrow-builtin arg slot (`len(a+b)`, `sorted(make())`),
    // drained at the common tail. print and list()/set() manage their own args.
    std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
    std::vector<llvm::Value*> argTempBases;
    bool builtinHandled = [&]() -> bool {
    // print(*args): one space between args, one trailing newline. Per-arg type
    // dispatch lives in emitPrintArgRaw.
    if (name == "print") {
        if (node.args.empty()) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_print_newline"], {});
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        for (size_t pi = 0; pi < node.args.size(); pi++) {
            if (pi > 0)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_print_space"], {});
            emitPrintArgRaw(node.args[pi].get());
        }
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_newline"], {});
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
        return true;
    }

    // issubclass constant-folds over the single-inheritance chain, so it costs
    // nothing at runtime. Builtin names match only themselves or `object`.
    if (name == "issubclass" && node.args.size() == 2) {
        auto nameOf = [&](Expr* e) -> std::string {
            if (auto* ne = dynamic_cast<NameExpr*>(e)) return ne->name;
            return impl_->resolveExprClassName(e);
        };
        std::string sub = nameOf(node.args[0].get());
        std::string base = nameOf(node.args[1].get());
        bool result = false;
        if (base == "object") {
            result = true;
        } else if (!base.empty() && sub == base) {
            result = true;
        } else if (!base.empty()) {
            std::string cur = sub;
            for (int guard = 0; !cur.empty() && guard < 1000; ++guard) {
                auto pit = impl_->classParentNamesBySym.find(impl_->classSym(cur));
                if (pit == impl_->classParentNamesBySym.end()) break;
                cur = pit->second;
                if (cur == base) { result = true; break; }
            }
        }
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, result ? 1 : 0);
        return true;
    }
    // map(f, xs) desugars to [f(__map_x) for __map_x in xs]. Seeding the
    // synthetic call's type lets the comprehension pick the right elemTag.
    if (name == "map" && node.args.size() == 2) {
        std::shared_ptr<Type> elemType;        // f's return type -> list element
        if (node.args[0]->type && node.args[0]->type->kind() == Type::Kind::Function)
            elemType = static_cast<FunctionType&>(*node.args[0]->type).returnType;
        std::shared_ptr<Type> iterElemType;    // xs's element type -> f's arg
        if (node.args[1]->type && node.args[1]->type->kind() == Type::Kind::List)
            iterElemType = static_cast<ListType&>(*node.args[1]->type).elementType;

        auto loopVar = std::make_unique<NameExpr>();
        loopVar->name = "__map_x";
        loopVar->type = iterElemType;

        auto callElem = std::make_unique<CallExpr>();
        callElem->callee = std::move(node.args[0]);
        callElem->args.push_back(std::move(loopVar));
        callElem->type = elemType;

        ListCompExpr lc;
        lc.varName = "__map_x";
        lc.iterable = std::move(node.args[1]);
        lc.element = std::move(callElem);
        lc.type = node.type;
        lc.accept(*this);
        return true;
    }

    // filter(f, xs) desugars to [x for x in xs if f(x)]; filter never
    // transforms, so a `list[T]` input yields `list[T]`.
    if (name == "filter" && node.args.size() == 2) {
        std::shared_ptr<Type> iterElemType;
        if (node.args[1]->type && node.args[1]->type->kind() == Type::Kind::List)
            iterElemType = static_cast<ListType&>(*node.args[1]->type).elementType;

        auto predVar = std::make_unique<NameExpr>();
        predVar->name = "__filter_x";
        predVar->type = iterElemType;
        auto cond = std::make_unique<CallExpr>();
        cond->callee = std::move(node.args[0]);
        cond->args.push_back(std::move(predVar));
        // No static type needed: the condition codegen tests the predicate
        // call's result for truthiness directly.

        // The kept element is the loop variable itself (identity).
        auto elemVar = std::make_unique<NameExpr>();
        elemVar->name = "__filter_x";
        elemVar->type = iterElemType;

        ListCompExpr lc;
        lc.varName = "__filter_x";
        lc.iterable = std::move(node.args[1]);
        lc.element = std::move(elemVar);
        lc.condition = std::move(cond);
        lc.type = node.type;
        lc.accept(*this);
        return true;
    }

    if (name == "len" && node.args.size() == 1) {
        bool isDict = dynamic_cast<DictExpr*>(node.args[0].get()) != nullptr;
        if (!isDict) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isDict = impl_->lookupVarKind(argName->name) == Impl::VarKind::Dict;
            }
        }
        bool isList = dynamic_cast<ListExpr*>(node.args[0].get()) != nullptr;
        if (!isList) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isList = impl_->lookupVarKind(argName->name) == Impl::VarKind::List;
            }
        }
        bool isTuple = dynamic_cast<TupleExpr*>(node.args[0].get()) != nullptr;
        if (!isTuple) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isTuple = impl_->lookupVarKind(argName->name) == Impl::VarKind::Tuple;
            }
        }
        bool isSet = dynamic_cast<SetExpr*>(node.args[0].get()) != nullptr;
        if (!isSet) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isSet = impl_->lookupVarKind(argName->name) == Impl::VarKind::Set;
            }
        }
        // An inline set-method result must beat the fallback below, which maps
        // copy()'s ListType to dragon_list_len (wrong offset on a DragonSet*).
        if (!isSet && impl_->resolveExprVarKind(node.args[0].get()) == Impl::VarKind::Set)
            isSet = true;
        if (!isList && !isDict && !isTuple && !isSet) {
            if (auto* argAttr = dynamic_cast<AttributeExpr*>(node.args[0].get())) {
                std::string className;
                if (auto* objName = dynamic_cast<NameExpr*>(argAttr->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty())
                        className = impl_->currentClassName;
                    else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) className = vit->second;
                    }
                }
                if (!className.empty()) {
                    auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                    if (fkIt != impl_->classFieldKindsBySym.end()) {
                        auto fkIt2 = fkIt->second.find(argAttr->attribute);
                        if (fkIt2 != fkIt->second.end()) {
                            if (fkIt2->second == Impl::VarKind::List) isList = true;
                            else if (fkIt2->second == Impl::VarKind::Dict) isDict = true;
                            else if (fkIt2->second == Impl::VarKind::Tuple) isTuple = true;
                            else if (fkIt2->second == Impl::VarKind::Set) isSet = true;
                        }
                    }
                }
            }
        }
        // Fallback for shapes the heuristics miss (`len(a[i])`, `len(f())`):
        // without it, dragon_str_len reads a list header as a C string.
        if (!isList && !isDict && !isTuple && !isSet && node.args[0]->type) {
            switch (node.args[0]->type->kind()) {
                case Type::Kind::List:  isList  = true; break;
                case Type::Kind::Dict:  isDict  = true; break;
                case Type::Kind::Tuple: isTuple = true; break;
                case Type::Kind::Set:   isSet   = true; break;
                default: break;
            }
        }
        bool isDeque = false;
        if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
            isDeque = impl_->lookupVarKind(argName->name) == Impl::VarKind::Deque;
        }
        bool isBytes = impl_->exprIsBytes(node.args[0].get());
        std::string lenClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!lenClassName.empty() && impl_->hasDunder(lenClassName, "__len__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(lenClassName, "__len__", arg);
            return true;
        }
        if (arg->getType() == impl_->boxType) {
            // A box value beats every static hint: isinstance narrowing can
            // stamp the arg's type `list` while the binding stays a box.
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_len"], {arg}, "len");
        } else if (isDeque) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_deque_len"], {arg}, "len");
        } else if (isBytes) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_len"], {arg}, "len");
        } else if (isTuple) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_len"], {arg}, "len");
        } else if (isSet) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_len"], {arg}, "len");
        } else if (isDict) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_len"], {arg}, "len");
        } else if (isList) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_len"], {arg}, "len");
        } else if (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_len"], {arg}, "len");
        } else {
            impl_->addError(
                "len() operand lowered to a non-container scalar; the call "
                "would have silently produced 0",
                node.location());
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        }
        return true;
    }

    if (name == "abs" && node.args.size() == 1) {
        std::string absClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!absClassName.empty() && impl_->hasDunder(absClassName, "__abs__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(absClassName, "__abs__", arg);
            return true;
        }
        // A float must take dragon_abs_float: the int one takes i64 and fails
        // LLVM verify on a double. Bool widens to i64 for the int path.
        if (arg->getType() == impl_->f64Type) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_abs_float"], {arg}, "fabs");
            return true;
        }
        if (arg->getType() == impl_->i1Type)
            arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_abs_int"], {arg}, "abs");
        return true;
    }

    if (name == "int" && node.args.size() == 1) {
        // Resolve the class before evaluating the arg so __int__ dispatch knows
        // its receiver.
        std::string intClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!intClassName.empty() && impl_->hasDunder(intClassName, "__int__")) {
            llvm::Value* r = impl_->callDunder(intClassName, "__int__", arg);
            // __int__ returns int; normalize a bool/intc result to i64.
            if (r->getType() == impl_->i1Type)
                r = impl_->builder->CreateZExt(r, impl_->i64Type, "btoi");
            else
                r = impl_->normalizeIntC(r);
            impl_->lastValue = r;
            return true;
        }
        if (arg->getType() == impl_->f64Type) {
            impl_->lastValue = impl_->builder->CreateFPToSI(
                arg, impl_->i64Type, "ftoi");
        } else if (arg->getType() == impl_->i1Type) {
            impl_->lastValue = impl_->builder->CreateZExt(
                arg, impl_->i64Type, "btoi");
        } else if (arg->getType() == impl_->i8PtrType) {
            // int("200") parses at runtime; without this the i8* passes through
            // and every consumer gets a pointer where it expects an integer.
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_to_int",
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, "stoi");
        } else {
            impl_->lastValue = arg;
        }
        return true;
    }

    if (name == "float" && node.args.size() == 1) {
        std::string floatClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!floatClassName.empty() && impl_->hasDunder(floatClassName, "__float__")) {
            llvm::Value* r = impl_->callDunder(floatClassName, "__float__", arg);
            // __float__ returns float; widen an int/bool/intc result to f64.
            impl_->lastValue = impl_->coerceArg(r, impl_->f64Type);
            return true;
        }
        if (arg->getType() == impl_->i64Type) {
            impl_->lastValue = impl_->builder->CreateSIToFP(
                arg, impl_->f64Type, "itof");
        } else if (arg->getType() == impl_->i1Type) {
            impl_->lastValue = impl_->builder->CreateUIToFP(
                arg, impl_->f64Type, "btof");
        } else if (arg->getType()->isPointerTy()) {
            // float("3.5") parses at runtime; str is the only pointer kind that
            // is a valid float() input.
            auto* fnTy = llvm::FunctionType::get(
                impl_->f64Type, {impl_->i8PtrType}, false);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_to_float", fnTy);
            impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, "stof");
        } else {
            impl_->lastValue = arg;  // already f64
        }
        return true;
    }

    if (name == "str" && node.args.size() == 1) {
        std::string strClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!strClassName.empty() && impl_->hasDunder(strClassName, "__str__")) {
            impl_->lastValue = impl_->callDunder(strClassName, "__str__", arg);
        } else if (!strClassName.empty() && impl_->hasDunder(strClassName, "__repr__")) {
            impl_->lastValue = impl_->callDunder(strClassName, "__repr__", arg);
        } else if (arg->getType() == impl_->i64Type) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_int_to_str"], {arg}, "itos");
        } else if (arg->getType() == impl_->f64Type) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_float_to_str"], {arg}, "ftos");
        } else if (arg->getType() == impl_->i1Type) {
            auto* ext = impl_->builder->CreateZExt(arg, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bool_to_str"], {ext}, "btos");
        } else if (arg->getType() == impl_->boxType) {
            // D039: tag-dispatched conversion for a boxed value.
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_to_str"], {arg}, "btos.any");
        } else if (!strClassName.empty() &&
                   impl_->userExcCodesBySym.count(impl_->classSym(strClassName)) > 0) {
            // Exception with no __str__/__repr__: `str(e)` is the raise-time
            // msg slot, duped because str() results are owned by convention.
            auto* slotMsg = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "exc.msg.b");
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_string_dup"], {slotMsg}, "exc.msg");
        } else if (arg->getType()->isPointerTy()) {
            // Container (list/dict/set/tuple) -> its repr.
            std::string creprFn = impl_->containerReprFn(node.args[0].get());
            if (!creprFn.empty()) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs[creprFn], {arg}, "ctos");
            } else if (impl_->options.gcMode == GCMode::RC &&
                       (impl_->resolveExprVarKind(node.args[0].get()) ==
                            Impl::VarKind::Str ||
                        (node.args[0]->type &&
                         node.args[0]->type->kind() == Type::Kind::Str))) {
                // str(s) is identity but owned by convention, so retain: a bare
                // borrow makes `msg = str(e)` over-release the exception slot.
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_retain"], {arg}, "stos");
            }
        }
        // A non-str pointer with no repr (instance without __str__) is left
        // as-is.
        return true;
    }

    if (name == "bool" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        // bool() only reads its arg; an owned temp drains at the argTemps tail.
        llvm::Value* arg = impl_->trackBorrowTempGuarded(
            node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        // Same truthiness as an if/while condition: numeric != 0, len != 0,
        // __bool__/__len__, else non-null.
        impl_->lastValue = impl_->toBool(arg, node.args[0].get());
        return true;
    }

    // bytes(): empty; bytes(int): zero-filled buffer; bytes(list[int]): values.
    if (name == "bytes") {
        if (node.args.empty()) {
            llvm::Value* nullData = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            llvm::Value* zeroLen = llvm::ConstantInt::get(impl_->i64Type, 0);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_from_literal"], {nullData, zeroLen}, "bytesempty");
            return true;
        }
        if (node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            if (arg->getType() == impl_->i64Type) {
                llvm::Value* nullData = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_from_literal"], {nullData, arg}, "bytesfill");
            } else {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_from_list"], {arg}, "bytesfromlist");
            }
            return true;
        }
        return false;
    }

    if (name == "input") {
        llvm::Value* prompt;
        if (!node.args.empty()) {
            node.args[0]->accept(*this);
            prompt = impl_->lastValue;
        } else {
            prompt = impl_->builder->CreateGlobalString("");
        }
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_input"], {prompt}, "input");
        return true;
    }

    // range() is fused into for-loop/comprehension codegen and materialized
    // by list(range(...)); the checker rejects it in any other position.
    if (name == "range") {
        impl_->addError(
            "internal error: bare range() reached codegen; the front end "
            "should have rejected it",
            node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return true;
    }

    // min/max: one iterable arg goes to the runtime; two or more scalars fold
    // pairwise inline, so there is no runtime variadic on the hot path.
    if ((name == "min" || name == "max") && !node.args.empty()) {
        bool isMin = (name == "min");
        if (node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* mmArg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            Type::Kind elemKind = Type::Kind::Unknown;
            if (node.args[0]->type) {
                if (auto* lt = dynamic_cast<ListType*>(node.args[0]->type.get())) {
                    if (lt->elementType) elemKind = lt->elementType->kind();
                }
            }
            const char* fn;
            if (elemKind == Type::Kind::Float) {
                fn = isMin ? "dragon_min_list_f64" : "dragon_max_list_f64";
            } else if (elemKind == Type::Kind::Str) {
                fn = isMin ? "dragon_min_list_str" : "dragon_max_list_str";
            } else {
                fn = isMin ? "dragon_min_list" : "dragon_max_list";
            }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[fn], {mmArg}, name);
            if (elemKind == Type::Kind::Bool)
                impl_->lastValue = impl_->builder->CreateTrunc(impl_->lastValue, impl_->i1Type);
            return true;
        }
        std::vector<llvm::Value*> vals;
        bool anyFloat = false;
        for (auto& arg : node.args) {
            arg->accept(*this);
            llvm::Value* v = impl_->trackBorrowTempGuarded(arg.get(), impl_->lastValue, argTemps, argTempBases);
            if (v->getType() == impl_->i1Type) v = impl_->builder->CreateZExt(v, impl_->i64Type);
            if (v->getType() == impl_->f64Type) anyFloat = true;
            vals.push_back(v);
        }
        if (anyFloat) {
            for (auto& v : vals)
                if (v->getType() != impl_->f64Type) v = impl_->builder->CreateSIToFP(v, impl_->f64Type);
        }
        const char* fn = anyFloat ? (isMin ? "dragon_min_float" : "dragon_max_float")
                                   : (isMin ? "dragon_min_int"   : "dragon_max_int");
        llvm::Value* acc = vals[0];
        for (size_t k = 1; k < vals.size(); k++)
            acc = impl_->builder->CreateCall(impl_->runtimeFuncs[fn], {acc, vals[k]}, name);
        impl_->lastValue = acc;
        return true;
    }

    if (name == "sum" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* sumArg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        Type::Kind elemKind = Type::Kind::Unknown;
        if (node.args[0]->type) {
            if (auto* lt = dynamic_cast<ListType*>(node.args[0]->type.get())) {
                if (lt->elementType) elemKind = lt->elementType->kind();
            }
        }
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs[elemKind == Type::Kind::Float ? "dragon_sum_list_f64"
                                                              : "dragon_sum_list"],
            {sumArg}, "sum");
        return true;
    }

    if (name == "any" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* anyArg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        llvm::Value* result = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_any_list"], {anyArg}, "any");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            result, llvm::ConstantInt::get(impl_->i64Type, 0));
        return true;
    }

    if (name == "all" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* allArg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        llvm::Value* result = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_all_list"], {allArg}, "all");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            result, llvm::ConstantInt::get(impl_->i64Type, 0));
        return true;
    }

    if (name == "enumerate") {
        if (node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            llvm::Value* start = llvm::ConstantInt::get(impl_->i64Type, 0);
            if (node.args.size() >= 2) {
                node.args[1]->accept(*this);
                start = impl_->lastValue;
            }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_enumerate"], {list, start}, "enum");
            return true;
        }
    }

    if (name == "zip" && node.args.size() == 2) {
        node.args[0]->accept(*this);
        llvm::Value* a = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        node.args[1]->accept(*this);
        llvm::Value* b = impl_->trackBorrowTempGuarded(node.args[1].get(), impl_->lastValue, argTemps, argTempBases);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_zip"], {a, b}, "zip");
        return true;
    }

    // sorted(list[, reverse=]): plain dragon_sorted is cheaper, so reverse=
    // is what opts into the _ex form. key= is not implemented.
    if (name == "sorted" && node.args.size() == 1) {
        Expr* reverseArg = nullptr;
        for (auto& kw : node.kwArgs)
            if (kw.first == "reverse") reverseArg = kw.second.get();
        node.args[0]->accept(*this);  // evaluate the list first (Python order)
        llvm::Value* listv = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (listv->getType() == impl_->i64Type)
            listv = impl_->builder->CreateIntToPtr(listv, impl_->i8PtrType);
        if (reverseArg) {
            reverseArg->accept(*this);
            llvm::Value* rev = impl_->lastValue;
            if (rev->getType() == impl_->i1Type)
                rev = impl_->builder->CreateZExt(rev, impl_->i64Type);
            else if (rev->getType()->isPointerTy())
                rev = impl_->builder->CreatePtrToInt(rev, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_sorted_ex"], {listv, rev}, "sorted");
        } else {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_sorted"], {listv}, "sorted");
        }
        return true;
    }

    if (name == "reversed" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* revArg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_reversed"], {revArg}, "reversed");
        return true;
    }

    if (name == "hash" && node.args.size() == 1) {
        std::string hashClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!hashClassName.empty() && impl_->hasDunder(hashClassName, "__hash__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(hashClassName, "__hash__", arg);
            return true;
        }
        if (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy()) {
            // Default for an instance: id-based hash (the pointer as an int).
            if (!hashClassName.empty()) {
                impl_->lastValue = impl_->builder->CreatePtrToInt(arg, impl_->i64Type, "hash");
            } else {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_hash_str"], {arg}, "hash");
            }
        } else {
            if (arg->getType() == impl_->i1Type)
                arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_hash_int"], {arg}, "hash");
        }
        return true;
    }

    if (name == "id" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (arg->getType()->isPointerTy()) {
            arg = impl_->builder->CreatePtrToInt(arg, impl_->i64Type);
        } else if (arg->getType() == impl_->i1Type) {
            arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        }
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_id"], {arg}, "id");
        return true;
    }

    if (name == "repr" && node.args.size() == 1) {
        std::string reprClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (!reprClassName.empty() && impl_->hasDunder(reprClassName, "__repr__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(reprClassName, "__repr__", arg);
            return true;
        }
        if (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_repr_str"], {arg}, "repr");
        } else if (arg->getType() == impl_->f64Type) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_repr_float"], {arg}, "repr");
        } else if (arg->getType() == impl_->i1Type) {
            auto* ext = impl_->builder->CreateZExt(arg, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_repr_bool"], {ext}, "repr");
        } else {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_repr_int"], {arg}, "repr");
        }
        return true;
    }

    if (name == "ord" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        // ord borrows: `ord(s[i])` mallocs a fresh 1-char string that leaks
        // once per call without the argTemps drain.
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_ord"], {arg}, "ord");
        return true;
    }

    if (name == "chr" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (arg->getType() == impl_->i1Type)
            arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_chr"], {arg}, "chr");
        return true;
    }

    // __float_bits(f): reinterpret an f64's IEEE-754 bits as an i64 (a bitcast,
    // not a conversion) - the float half of the binary wire-codec bridge.
    if (name == "__float_bits" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* f = impl_->lastValue;
        if (f->getType() == impl_->f64Type)
            impl_->lastValue = impl_->builder->CreateBitCast(f, impl_->i64Type, "float.bits");
        else if (f->getType() == impl_->i1Type)
            impl_->lastValue = impl_->builder->CreateZExt(f, impl_->i64Type);
        else
            impl_->lastValue = f;  // already i64-shaped
        return true;
    }

    // __float_from_bits(i): reinterpret an i64's bit pattern as an f64, the
    // inverse of __float_bits.
    if (name == "__float_from_bits" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* i = impl_->lastValue;
        if (i->getType() == impl_->i1Type)
            i = impl_->builder->CreateZExt(i, impl_->i64Type);
        if (i->getType() == impl_->i64Type)
            impl_->lastValue = impl_->builder->CreateBitCast(i, impl_->f64Type, "float.frombits");
        else
            impl_->lastValue = i;  // already f64-shaped
        return true;
    }

    // __float32_bits(f): round to single precision and reinterpret those 32
    // bits in the low half of an int (struct 'f', 4-byte FLOAT columns).
    if (name == "__float32_bits" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* f = impl_->lastValue;
        if (f->getType() != impl_->f64Type) {
            if (f->getType() == impl_->i1Type)
                f = impl_->builder->CreateSIToFP(impl_->builder->CreateZExt(f, impl_->i64Type), impl_->f64Type);
            else if (f->getType() == impl_->i64Type)
                f = impl_->builder->CreateSIToFP(f, impl_->f64Type);
        }
        auto* i32Ty = llvm::Type::getInt32Ty(*impl_->context);
        auto* f32 = impl_->builder->CreateFPTrunc(f, llvm::Type::getFloatTy(*impl_->context), "f32");
        auto* i32 = impl_->builder->CreateBitCast(f32, i32Ty, "f32.bits");
        impl_->lastValue = impl_->builder->CreateZExt(i32, impl_->i64Type, "f32.bits.z");
        return true;
    }

    // __float32_from_bits(i): reinterpret the low 32 bits as a single-precision
    // float widened to f64, the inverse of __float32_bits.
    if (name == "__float32_from_bits" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* i = impl_->lastValue;
        if (i->getType() == impl_->i1Type)
            i = impl_->builder->CreateZExt(i, impl_->i64Type);
        auto* i32 = impl_->builder->CreateTrunc(i, llvm::Type::getInt32Ty(*impl_->context), "f32.lo");
        auto* f32 = impl_->builder->CreateBitCast(i32, llvm::Type::getFloatTy(*impl_->context), "f32.val");
        impl_->lastValue = impl_->builder->CreateFPExt(f32, impl_->f64Type, "f32.ext");
        return true;
    }

    // isinstance(obj, T) folds statically wherever the value's type is known.
    if (name == "isinstance" && node.args.size() == 2) {
        std::string typeName;
        if (auto* typeNameExpr = dynamic_cast<NameExpr*>(node.args[1].get())) {
            typeName = typeNameExpr->name;
        }

        // Classes are compile-time entities (D021): the 2nd arg must name a
        // class statically, never a variable holding a `: type` value.
        if (auto* typeNameExpr = dynamic_cast<NameExpr*>(node.args[1].get())) {
            if (impl_->lookupVarKind(typeNameExpr->name) == Impl::VarKind::Type) {
                impl_->addError(
                    "classes are not values: cannot use '" + typeNameExpr->name +
                    "' as the type in isinstance; the type must be a class name "
                    "known at compile time (e.g. isinstance(x, ClassName)).",
                    node.location());
                node.args[0]->accept(*this); // eval for side effects
                impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 0);
                return true;
            }
        }

        if (!typeName.empty()) {
            Impl::VarKind argKind = Impl::VarKind::Other;
            std::string argClassName;
            std::string argVarName;
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                argKind = impl_->lookupVarKind(argName->name);
                argVarName = argName->name;
                auto it = impl_->varClassNames.find(argName->name);
                if (it != impl_->varClassNames.end())
                    argClassName = it->second;
            }
            // Non-NameExpr receiver (call result, attribute, subscript, ...):
            // recover the class name from the static type so isinstance works.
            if (argClassName.empty())
                argClassName = impl_->resolveExprClassName(node.args[0].get());

            // Niche-pointer Optional (`T | None`, ptr-shaped T): no box, no tag, null
            // IS None - isinstance must test the pointer, never the static kind.
            if (const Type* argT = node.args[0]->type.get()) {
                const Type* nicheT = nullptr;
                if (argT->kind() == Type::Kind::Union) {
                    auto& ut = static_cast<const UnionType&>(*argT);
                    const Type* other = nullptr;
                    bool hasNone = false;
                    if (ut.types.size() == 2) {
                        for (auto& m : ut.types) {
                            if (!m) continue;
                            if (m->kind() == Type::Kind::None_) hasNone = true;
                            else other = m.get();
                        }
                    }
                    if (hasNone && other) {
                        switch (other->kind()) {
                            case Type::Kind::Str: case Type::Kind::Bytes:
                            case Type::Kind::List: case Type::Kind::Dict:
                            case Type::Kind::Tuple: case Type::Kind::Set:
                            case Type::Kind::Instance:
                                nicheT = other; break;
                            default: break;
                        }
                    }
                }
                if (nicheT) {
                    bool matches = false;
                    switch (nicheT->kind()) {
                        case Type::Kind::Str:   matches = (typeName == "str"); break;
                        case Type::Kind::Bytes: matches = (typeName == "bytes"); break;
                        case Type::Kind::List:  matches = (typeName == "list"); break;
                        case Type::Kind::Dict:  matches = (typeName == "dict"); break;
                        case Type::Kind::Tuple: matches = (typeName == "tuple"); break;
                        case Type::Kind::Set:   matches = (typeName == "set"); break;
                        case Type::Kind::Instance: {
                            // Ancestor walk, as the static class path below does.
                            auto& inst = static_cast<const InstanceType&>(*nicheT);
                            std::string c = inst.classType ? inst.classType->name : "";
                            while (!c.empty()) {
                                if (c == typeName) { matches = true; break; }
                                auto pit = impl_->classParentNamesBySym.find(impl_->classSym(c));
                                c = (pit != impl_->classParentNamesBySym.end()) ? pit->second
                                                                           : std::string();
                            }
                            break;
                        }
                        default: break;
                    }
                    node.args[0]->accept(*this);
                    llvm::Value* recv = impl_->lastValue;
                    if (!matches) {
                        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 0);
                        return true;
                    }
                    if (recv && recv->getType()->isPointerTy())
                        impl_->lastValue =
                            impl_->builder->CreateIsNotNull(recv, "isinstance.nn");
                    else if (recv && recv->getType() == impl_->i64Type)
                        impl_->lastValue = impl_->builder->CreateICmpNE(
                            recv, llvm::ConstantInt::get(impl_->i64Type, 0),
                            "isinstance.nn");
                    else
                        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 1);
                    return true;
                }
            }

            // D030: a Union variable compares against the box's runtime tag.
            if (argKind == Impl::VarKind::Union) {
                int64_t targetTag = -1;
                if (typeName == "int")        targetTag = 0;
                else if (typeName == "str")   targetTag = 1;
                else if (typeName == "float") targetTag = 2;
                else if (typeName == "bool")  targetTag = 3;
                else if (typeName == "list")  targetTag = 5;
                else if (typeName == "dict")  targetTag = 6;
                else if (typeName == "bytes") targetTag = 7;
                // TAG_CLASS shares slot 7 with TAG_BYTES; a Union cannot legally
                // hold both a class member and a bytes member.
                else if (impl_->classNames.count(typeName)) targetTag = 7;
                if (targetTag >= 0) {
                    // Local alloca first, then module global: without the
                    // fallback, module-level union vars fold to constant false.
                    llvm::Value* slotPtr = impl_->lookupVar(argVarName);
                    if (!slotPtr)
                        slotPtr = impl_->lookupModuleGlobal(argVarName);
                    if (slotPtr) {
                        node.args[0]->accept(*this); // eval for side effects
                        auto* box = impl_->builder->CreateLoad(
                            impl_->boxType, slotPtr, argVarName + ".box");
                        auto* tagVal = impl_->boxTag(box, argVarName + ".tag");
                        impl_->lastValue = impl_->builder->CreateICmpEQ(
                            tagVal, llvm::ConstantInt::get(impl_->i64Type, targetTag),
                            "isinstance");
                        return true;
                    }
                }
            }

            bool result = false;
            if (typeName == "int")
                result = (argKind == Impl::VarKind::Int);
            else if (typeName == "float")
                result = (argKind == Impl::VarKind::Float);
            else if (typeName == "bool")
                result = (argKind == Impl::VarKind::Bool);
            else if (typeName == "str")
                result = (argKind == Impl::VarKind::Str ||
                          argKind == Impl::VarKind::StrLiteral);
            else if (typeName == "list")
                result = (argKind == Impl::VarKind::List);
            else if (typeName == "dict")
                result = (argKind == Impl::VarKind::Dict);
            else if (typeName == "tuple")
                result = (argKind == Impl::VarKind::Tuple);
            else if (typeName == "set")
                result = (argKind == Impl::VarKind::Set);
            else if (typeName == "bytes") {
                // D030: bytes is identified by the static type alone - its slot
                // carries the generic-heap VarKind::List.
                result = node.args[0] && node.args[0]->type &&
                         node.args[0]->type->kind() == Type::Kind::Bytes;
                (void)argKind;
            }
            bool classCheck = false;
            if (typeName != "int" && typeName != "float" && typeName != "bool" &&
                typeName != "str" && typeName != "list" && typeName != "dict" &&
                typeName != "tuple" && typeName != "set" && typeName != "bytes") {
                // Walk the inheritance chain so an instance of a subclass IS an
                // instance of any ancestor.
                classCheck = true;
                std::string c = argClassName;
                while (!c.empty()) {
                    if (c == typeName) { result = true; break; }
                    auto pit = impl_->classParentNamesBySym.find(impl_->classSym(c));
                    c = (pit != impl_->classParentNamesBySym.end()) ? pit->second : std::string();
                }
            }
            // Evaluate args for side effects
            node.args[0]->accept(*this);
            // A static class match must still test the value: an Optional slot
            // holding None is a null instance, and a method call on it SEGVs.
            if (result && classCheck) {
                llvm::Value* recv = impl_->lastValue;
                if (recv && recv->getType()->isPointerTy()) {
                    impl_->lastValue = impl_->builder->CreateIsNotNull(recv, "isinstance.nn");
                    return true;
                }
                if (recv && recv->getType() == impl_->i64Type) {
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        recv, llvm::ConstantInt::get(impl_->i64Type, 0), "isinstance.nn");
                    return true;
                }
            }
            impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, result ? 1 : 0);
            return true;
        }
        impl_->addError(
            "internal error: isinstance type argument did not resolve at "
            "codegen; the front end should have rejected it",
            node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 0);
        return true;
    }

    // type(obj) - returns type name as string
    if (name == "type" && node.args.size() == 1) {
        Impl::VarKind argKind = Impl::VarKind::Other;
        std::string argClassName;
        if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
            argKind = impl_->lookupVarKind(argName->name);
            auto it = impl_->varClassNames.find(argName->name);
            if (it != impl_->varClassNames.end())
                argClassName = it->second;
        }

        // D030: a Union switches on the box tag for its type-name string.
        if (argKind == Impl::VarKind::Union) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                auto* alloca = impl_->lookupVar(argName->name);
                if (alloca) {
                    node.args[0]->accept(*this); // eval for side effects
                    auto* box = impl_->builder->CreateLoad(
                        impl_->boxType, alloca, "type.box");
                    auto* tag = impl_->boxTag(box, "type.tag");
                    auto* func2 = impl_->currentFunction;
                    auto* mergeBB = llvm::BasicBlock::Create(*impl_->context, "type.end", func2);
                    auto* result = impl_->createEntryAlloca(func2, "type.result", impl_->i8PtrType);
                    auto* defaultBB = llvm::BasicBlock::Create(*impl_->context, "type.default", func2);
                    auto* sw = impl_->builder->CreateSwitch(tag, defaultBB, 7);

                    auto emitTypeCase = [&](int64_t tagVal, const char* bbName, const char* typStr) {
                        auto* bb = llvm::BasicBlock::Create(*impl_->context, bbName, func2);
                        sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(impl_->i64Type, tagVal)), bb);
                        impl_->builder->SetInsertPoint(bb);
                        impl_->builder->CreateStore(impl_->builder->CreateGlobalString(typStr), result);
                        impl_->builder->CreateBr(mergeBB);
                    };
                    emitTypeCase(0, "type.int", "int");
                    emitTypeCase(1, "type.str", "str");
                    emitTypeCase(2, "type.float", "float");
                    emitTypeCase(3, "type.bool", "bool");
                    emitTypeCase(5, "type.list", "list");
                    emitTypeCase(6, "type.dict", "dict");
                    emitTypeCase(7, "type.bytes", "bytes");

                    impl_->builder->SetInsertPoint(defaultBB);
                    impl_->builder->CreateStore(impl_->builder->CreateGlobalString("object"), result);
                    impl_->builder->CreateBr(mergeBB);

                    impl_->builder->SetInsertPoint(mergeBB);
                    impl_->lastValue = impl_->builder->CreateLoad(impl_->i8PtrType, result, "type.name");
                    return true;
                }
            }
        }

        // The static type leads: a literal carries no VarKind, so `type(5)`
        // would otherwise report "object". VarKind is the instance fallback.
        std::string typeName;
        Type::Kind stKind = (node.args[0] && node.args[0]->type)
            ? node.args[0]->type->kind() : Type::Kind::Unknown;
        switch (stKind) {
            case Type::Kind::Int:   typeName = "int";   break;
            case Type::Kind::Float: typeName = "float"; break;
            case Type::Kind::Bool:  typeName = "bool";  break;
            case Type::Kind::Str:   typeName = "str";   break;
            case Type::Kind::Bytes: typeName = "bytes"; break;
            case Type::Kind::List:  typeName = "list";  break;
            case Type::Kind::Dict:  typeName = "dict";  break;
            case Type::Kind::Tuple: typeName = "tuple"; break;
            case Type::Kind::Set:   typeName = "set";   break;
            case Type::Kind::Instance: {
                // A non-NameExpr arg (`type(Dog())`) carries no VarKind, so the
                // class name comes from the static type.
                auto& inst = static_cast<InstanceType&>(*node.args[0]->type);
                if (inst.classType) typeName = inst.classType->name;
                break;
            }
            default: break;  // Class/Any/etc. -> resolve via VarKind
        }
        if (typeName.empty()) switch (argKind) {
            case Impl::VarKind::Int:   typeName = "int"; break;
            case Impl::VarKind::Float: typeName = "float"; break;
            case Impl::VarKind::Bool:  typeName = "bool"; break;
            case Impl::VarKind::Str:
            case Impl::VarKind::StrLiteral: typeName = "str"; break;
            case Impl::VarKind::List:  typeName = "list"; break;
            case Impl::VarKind::Dict:  typeName = "dict"; break;
            case Impl::VarKind::Tuple: typeName = "tuple"; break;
            case Impl::VarKind::Set:   typeName = "set"; break;
            case Impl::VarKind::File:  typeName = "file"; break;
            case Impl::VarKind::ClassInstance:
                // Classes are compile-time entities (D021): type(instance) is
                // the class name string, never a runtime descriptor.
                typeName = argClassName.empty() ? "object" : argClassName;
                break;
            default: typeName = "object"; break;
        }
        // Evaluate arg for side effects; an owned temp (`type(Dog("rex"))`
        // builds an instance nobody keeps) is drained by the argTemps tail.
        node.args[0]->accept(*this);
        impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        impl_->lastValue = impl_->builder->CreateGlobalString(typeName);
        return true;
    }

    if (name == "round" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (arg->getType() == impl_->i64Type) {
            impl_->lastValue = arg; // round of int is itself
        } else {
            if (arg->getType() == impl_->i1Type)
                arg = impl_->builder->CreateUIToFP(arg, impl_->f64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_round_int"], {arg}, "round");
        }
        return true;
    }

    // pow(base, exp): either operand being float selects the float form.
    if (name == "pow" && node.args.size() == 2) {
        node.args[0]->accept(*this);
        llvm::Value* base = impl_->lastValue;
        node.args[1]->accept(*this);
        llvm::Value* exp = impl_->lastValue;
        bool useFloat = base->getType() == impl_->f64Type || exp->getType() == impl_->f64Type;
        if (useFloat) {
            if (base->getType() != impl_->f64Type) base = impl_->builder->CreateSIToFP(base, impl_->f64Type);
            if (exp->getType() != impl_->f64Type) exp = impl_->builder->CreateSIToFP(exp, impl_->f64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_pow_float"], {base, exp}, "pow");
        } else {
            if (base->getType() == impl_->i1Type) base = impl_->builder->CreateZExt(base, impl_->i64Type);
            if (exp->getType() == impl_->i1Type) exp = impl_->builder->CreateZExt(exp, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_pow_int"], {base, exp}, "pow");
        }
        return true;
    }

    if (name == "divmod" && node.args.size() == 2) {
        node.args[0]->accept(*this);
        llvm::Value* a = impl_->lastValue;
        node.args[1]->accept(*this);
        llvm::Value* b = impl_->lastValue;
        if (a->getType() == impl_->i1Type) a = impl_->builder->CreateZExt(a, impl_->i64Type);
        if (b->getType() == impl_->i1Type) b = impl_->builder->CreateZExt(b, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_divmod"], {a, b}, "divmod");
        return true;
    }

    if (name == "hex" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (arg->getType() == impl_->i1Type) arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_hex"], {arg}, "hex");
        return true;
    }
    if (name == "oct" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (arg->getType() == impl_->i1Type) arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_oct"], {arg}, "oct");
        return true;
    }
    if (name == "bin" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        if (arg->getType() == impl_->i1Type) arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_bin"], {arg}, "bin");
        return true;
    }

    if (name == "list" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_new"],
            {llvm::ConstantInt::get(impl_->i64Type, 8)}, "list");
        return true;
    }

    // list(x) always copies. Sets are ListType in the checker but a distinct
    // runtime struct, so they are excluded here.
    if (name == "list" && node.args.size() == 1) {
        Expr* a = node.args[0].get();
        // list(range(...)) - range() is for-loop-fused (a bare range expr is
        // just i64 0), so materialize it explicitly into an int list.
        if (auto* rcall = dynamic_cast<CallExpr*>(a)) {
            if (auto* rcn = dynamic_cast<NameExpr*>(rcall->callee.get())) {
                if (rcn->name == "range" && !rcall->args.empty()) {
                    auto evalI64 = [&](Expr* e) -> llvm::Value* {
                        e->accept(*this);
                        llvm::Value* v = impl_->lastValue;
                        if (v->getType() == impl_->i1Type)
                            v = impl_->builder->CreateZExt(v, impl_->i64Type);
                        return v;
                    };
                    llvm::Value* start = llvm::ConstantInt::get(impl_->i64Type, 0);
                    llvm::Value* stop;
                    llvm::Value* step = llvm::ConstantInt::get(impl_->i64Type, 1);
                    if (rcall->args.size() == 1) {
                        stop = evalI64(rcall->args[0].get());
                    } else {
                        start = evalI64(rcall->args[0].get());
                        stop = evalI64(rcall->args[1].get());
                        if (rcall->args.size() >= 3) step = evalI64(rcall->args[2].get());
                    }
                    auto* fn = impl_->getOrDeclareRuntime("dragon_list_from_range",
                        llvm::FunctionType::get(impl_->i8PtrType,
                            {impl_->i64Type, impl_->i64Type, impl_->i64Type}, false));
                    impl_->lastValue = impl_->builder->CreateCall(
                        fn, {start, stop, step}, "rangelist");
                    return true;
                }
            }
        }
        bool isSet = impl_->resolveExprVarKind(a) == Impl::VarKind::Set;
        if (!isSet && a->type && a->type->kind() == Type::Kind::List) {
            a->accept(*this);
            llvm::Value* src = impl_->lastValue;
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_copy"], {src}, "listcopy");
            // The copy is independent, so an owned source (`list(filter(f, xs))`)
            // leaks the whole source list per call unless released here.
            if (impl_->options.gcMode == GCMode::RC &&
                !Impl::isBorrowedHeapExpr(a))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {src});
            return true;
        }
    }

    if (name == "dict" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_new"],
            {llvm::ConstantInt::get(impl_->i64Type, 8)}, "dict");
        return true;
    }

    if (name == "set" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_new"], {}, "set");
        return true;
    }

    // Sets report Kind::List too, so gate on VarKind::List to keep set(aSet)
    // from misrouting a DragonSet* into the list path.
    if (name == "set" && node.args.size() == 1) {
        Expr* a = node.args[0].get();
        bool isList = dynamic_cast<ListExpr*>(a) != nullptr ||
                      dynamic_cast<ListCompExpr*>(a) != nullptr ||
                      impl_->resolveExprVarKind(a) == Impl::VarKind::List;
        if (isList) {
            a->accept(*this);
            llvm::Value* lst = impl_->lastValue;
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_from_list"], {lst}, "setfromlist");
            // The runtime increfs the elements it keeps, so an owned source
            // (`set(filter(...))`) still leaks its list unless released.
            if (impl_->options.gcMode == GCMode::RC &&
                !Impl::isBorrowedHeapExpr(a))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {lst});
            return true;
        }
    }

    if (name == "tuple" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_tuple_new"],
            {llvm::ConstantInt::get(impl_->i64Type, 0)}, "tuple");
        return true;
    }
    // tuple(iterable): list and set share the DragonList layout and the element
    // tag travels with it, so one converter covers every element type.
    if (name == "tuple" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        // The converter copies the elements, so an owned list temp
        // (`tuple([1, 2, 3])`) drains at the argTemps tail.
        llvm::Value* arg = impl_->trackBorrowTempGuarded(
            node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
        auto* fn = impl_->getOrDeclareRuntime("dragon_tuple_from_list",
            llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType}, false));
        impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, "tuplefromlist");
        return true;
    }


    if (name == "Lock") {
        impl_->needsPthread = true;
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_lock_new"], {}, "lock");
        return true;
    }

    if (name == "SyncList") {
        impl_->needsPthread = true;
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_synclist_new"], {}, "synclist");
        return true;
    }

    if (name == "SyncDict") {
        impl_->needsPthread = true;
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_syncdict_new"], {}, "syncdict");
        return true;
    }

    // deque(), deque(iterable[, maxlen]) and maxlen=. The runtime enforces
    // maxlen: appending past the bound discards the far end.
    if (name == "deque") {
        llvm::Value* maxlen = llvm::ConstantInt::get(impl_->i64Type, -1);
        for (auto& kw : node.kwArgs) {
            if (kw.first == "maxlen" && kw.second) {
                kw.second->accept(*this);
                maxlen = impl_->coerceArg(impl_->lastValue, impl_->i64Type);
            }
        }
        if (node.args.size() >= 2) {
            node.args[1]->accept(*this);
            maxlen = impl_->coerceArg(impl_->lastValue, impl_->i64Type);
        }
        if (node.args.empty()) {
            // Seed the element tag from the static type when the call site
            // knows it (`d: deque[str] = deque()`); appends refresh it anyway.
            int64_t elemTag = 0;
            if (auto* lt = dynamic_cast<ListType*>(node.type.get())) {
                if (lt->elementType) {
                    int64_t t = impl_->typeKindToTag(lt->elementType->kind());
                    if (t > 0) elemTag = t;
                }
            }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_deque_new"],
                {maxlen, llvm::ConstantInt::get(impl_->i64Type, elemTag)},
                "deque");
        } else {
            // The runtime copies + increfs each element, so an owned source
            // (`deque([1,2,3])`) leaks a list + buffer per call undrained.
            node.args[0]->accept(*this);
            llvm::Value* listArg = impl_->lastValue;
            Impl::VarKind srcDrain =
                impl_->ownedTempDrainKind(node.args[0].get(), listArg);
            if (!listArg->getType()->isPointerTy())
                listArg = impl_->builder->CreateIntToPtr(listArg, impl_->i8PtrType);
            llvm::Value* dq = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_deque_from_list"],
                {listArg, maxlen}, "deque");
            if (srcDrain != Impl::VarKind::Other)
                impl_->emitDecrefByKind(listArg, srcDrain);
            impl_->lastValue = dq;
        }
        return true;
    }

    // super() yields self; the parent-method dispatch happens at the call site.
    if (name == "super" && node.args.empty()) {
        auto* selfAlloca = impl_->lookupVar("self");
        if (selfAlloca) {
            impl_->lastValue = impl_->builder->CreateLoad(
                impl_->i8PtrType, selfAlloca, "super.self");
        } else {
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
        }
        return true;
    }

    if (name == "hasattr" && node.args.size() == 2) {
        node.args[0]->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        if (obj->getType()->isPointerTy())
            obj = impl_->builder->CreatePtrToInt(obj, impl_->i64Type);
        node.args[1]->accept(*this);
        llvm::Value* attrName = impl_->lastValue;
        if (attrName->getType() == impl_->i64Type)
            attrName = impl_->builder->CreateIntToPtr(attrName, impl_->i8PtrType);
        auto* result = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_hasattr"], {obj, attrName}, "hasattr");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            result, llvm::ConstantInt::get(impl_->i64Type, 0));
        return true;
    }

    // __exc_matches(code) range-matches the currently-handled exception's type
    // code. Only valid inside an `except` handler; backs assertRaises.
    if (name == "__exc_matches" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* expected = impl_->lastValue;
        if (expected->getType()->isPointerTy())
            expected = impl_->builder->CreatePtrToInt(expected, impl_->i64Type);
        else if (expected->getType() != impl_->i64Type)
            expected = impl_->builder->CreateZExtOrTrunc(expected, impl_->i64Type);
        auto* raised = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type.cur");
        auto* matched = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_matches"], {raised, expected}, "exc.match");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            matched, llvm::ConstantInt::get(impl_->i64Type, 0), "exc.match.bool");
        return true;
    }

    // D033: dir(obj) / dir(Cls) -> sorted list[str] of attribute names. The
    // runtime's second arg is is_descriptor: 0 for an instance, 1 for a class.
    if (name == "dir" && node.args.size() == 1) {
        // dir(ClassName) folds to a load of the class descriptor global.
        if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
            if (impl_->classNames.count(argName->name)) {
                auto descIt = impl_->classDescriptorGlobalsBySym.find(impl_->classSym(argName->name));
                if (descIt != impl_->classDescriptorGlobalsBySym.end()) {
                    auto* descI64 = impl_->builder->CreateLoad(
                        impl_->i64Type, descIt->second, argName->name + "_desc");
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dir"],
                        {descI64, llvm::ConstantInt::get(impl_->i64Type, 1)},
                        "dir.cls");
                    return true;
                }
            }
        }
        node.args[0]->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        if (obj->getType()->isPointerTy())
            obj = impl_->builder->CreatePtrToInt(obj, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dir"],
            {obj, llvm::ConstantInt::get(impl_->i64Type, 0)},
            "dir");
        return true;
    }

    if (name == "getattr" && (node.args.size() == 2 || node.args.size() == 3)) {
        node.args[0]->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        if (obj->getType()->isPointerTy())
            obj = impl_->builder->CreatePtrToInt(obj, impl_->i64Type);
        node.args[1]->accept(*this);
        llvm::Value* attrName = impl_->lastValue;
        if (attrName->getType() == impl_->i64Type)
            attrName = impl_->builder->CreateIntToPtr(attrName, impl_->i8PtrType);
        if (node.args.size() == 3) {
            node.args[2]->accept(*this);
            llvm::Value* defaultVal = impl_->lastValue;
            if (defaultVal->getType() == impl_->i1Type)
                defaultVal = impl_->builder->CreateZExt(defaultVal, impl_->i64Type);
            else if (defaultVal->getType() == impl_->f64Type)
                defaultVal = impl_->builder->CreateBitCast(defaultVal, impl_->i64Type);
            else if (defaultVal->getType()->isPointerTy())
                defaultVal = impl_->builder->CreatePtrToInt(defaultVal, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_getattr_default"],
                {obj, attrName, defaultVal}, "getattr");
        } else {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_getattr"], {obj, attrName}, "getattr");
        }
        return true;
    }

    return false;
    }();
    impl_->popArgTempCleanups(argTempBases);
    if (builtinHandled) {
        impl_->drainBorrowTemps(argTemps);
        return true;
    }
    return false;
}

} // namespace dragon
