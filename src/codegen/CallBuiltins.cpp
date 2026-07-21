/// Dragon CodeGen - Builtin Function Call Dispatch
/// Handles: print, len, abs, int, float, str, bool, input, range, min, max,
///  sum, any, all, enumerate, zip, sorted, reversed, hash, id, repr,
///  ord, chr, isinstance, type, round, pow, divmod, hex, oct, bin,
///  list/dict/set/tuple constructors, open, Lock, SyncList, SyncDict, super.
#include "../CodeGenImpl.h"

namespace dragon {

// Emit code to print ONE argument with NO trailing newline, using the `_raw`
// runtime printers. This is the per-argument type dispatch shared by single-
// and multi-arg print(); the caller (emitBuiltinCall's print branch) inserts
// spaces between args and one trailing newline. Faithful to the prior
// single-arg dispatch - only the terminal printers became their `_raw` form.
void CodeGen::emitPrintArgRaw(Expr* argExpr) {
    argExpr->accept(*this);
    llvm::Value* arg = impl_->lastValue;
    llvm::Type* argType = arg->getType();

    // D039 Phase 3: box-typed arg (Union/Any local, dict/list box get) ->
    // dragon_print_box_raw tag-switches to the right per-type printer.
    if (argType == impl_->boxType) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_box_raw"], {arg});
        // Free an owned box temporary (anyA + anyB, anyVal[i], ...) once printed
        // - print borrows, so without this every print(<owned-box-expr>) leaks
        // the payload. Borrowed box reads (a box local, dict/list element) are
        // not owned, so isOwnedBoxResult rejects them. Same convention as the
        // owned-str decref below.
        if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedBoxResult(arg)) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_decref"], {arg});
        }
        return;
    }

    // A dict/list subscript (or dict dot-access) whose value has a statically
    // known container/instance type carries its full type - including element
    // kinds - on argExpr->type. Such values MUST go through the general
    // dispatch below (which picks dragon_print_list_*/dict_*/tuple/set or the
    // class __str__), not the tag/str special-cases that follow - otherwise a
    // e.g. dict[int, list[int]] value is misprinted (the pointer gets blindly
    // rendered as a C string -> blank output). The tag/scalar special handling
    // stays for scalar (int/float), str/bytes, and dynamically-typed (Any)
    // dict values, where the runtime tag (not a static type) drives the printer.
    bool staticContainerVal = false;
    if (argExpr->type) {
        auto svk = argExpr->type->kind();
        staticContainerVal = (svk == Type::Kind::List || svk == Type::Kind::Dict ||
                              svk == Type::Kind::Tuple || svk == Type::Kind::Set ||
                              svk == Type::Kind::Instance);
    }

    // Dict value printing: if argument is dict subscript or dict dot-access,
    // use dragon_print_tagged_raw with runtime tag lookup.
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
            bool intKeyedSub = impl_->dictKeyIsInt(subscript->object.get());
            if (!intKeyedSub && subscript->object->type &&
                subscript->object->type->kind() == Type::Kind::Dict) {
                if (auto* dt = dynamic_cast<DictType*>(subscript->object->type.get())) {
                    if (dt->keyType && dt->keyType->kind() == Type::Kind::Int)
                        intKeyedSub = true;
                }
            }
            subscript->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            subscript->index->accept(*this);
            llvm::Value* key = impl_->lastValue;
            if (intKeyedSub) {
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
        // ADR 025 removal: a bare class name (or a variable holding a class
        // value) is not a runtime value - classes are compile-time entities
        // (D021). `print(SomeClass)` / printing a class-alias variable is a
        // compile error. Exception class names are excluded here: they lower
        // to integer type codes via the preserved exception-value path, not a
        // class value, so they never carry VarKind::Type as a print arg.
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

    // D030 Phase 4: Union-typed argument - runtime switch on the box's tag,
    // payload extracted at the right native type per branch.
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
    // D030 §5: bytes detection leads - bytes-typed slots collapse onto
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
    // C4: inline set-method result (`a.union(b)`, `a.copy()`, ...) -> set repr.
    // Must precede the type-kind fallback, which would map copy()'s ListType to
    // isPrintList and render the set with list brackets.
    if (!isPrintSet && impl_->resolveExprVarKind(argExpr) == Impl::VarKind::Set)
        isPrintSet = true;
    // Deque: must precede the type-kind fallback too - a deque is typed as
    // ListType, and the list printers would read the DragonDeque header as a
    // list (raw-pointer garbage). Routed to the tag-aware deque repr.
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
        bool keyIsInt = impl_->dictKeyIsInt(argExpr);
        bool valIsNested = false;  // C5: dict value is itself a container
        if (auto* dt = dynamic_cast<DictType*>(argExpr->type.get())) {
            if (dt->keyType && dt->keyType->kind() == Type::Kind::Int)
                keyIsInt = true;
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
                    // C5: a list whose elements are themselves containers must
                    // render recursively (the int printer would show element
                    // pointers as integers). Route through the repr builder.
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
        } else if (impl_->userExcCodes.count(printClassName) > 0) {
            // Exception instance with no user-defined __str__/__repr__:
            // Python parity is `str(e) == args[0]` (via Exception.__str__).
            // Dragon stashes that string in the runtime msg slot at raise
            // time (see RaiseStmt's first-string-arg snapshot), so route
            // print() there. Only valid while a matching handler is on the
            // stack - printing an exception instance outside a handler
            // is undefined anyway, since the instance was constructed via
            // raise. (`AppError("x"); print(e)` outside try/except is not
            // an idiom in Dragon or Python.)
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
        // Free an owned-str temporary (str(), `a + b`, `s[i]`, `s.upper()`, ...)
        // once printed: print *borrows* its argument, so without this every
        // `print(<owned-str-expr>)` leaks the result - a real per-call leak
        // (a tight `while` loop over `print(a + b)` grows RSS unbounded). The
        // borrowed-returners (dragon_dict_get_str_ptr / _int_get_str /
        // exc_get_msg) are handled+returned by the dict-subscript / exception
        // paths above and never reach here; isOwnedStrResult also rejects bare
        // variable loads and string literals (non-CallInst), so only genuine
        // owned temporaries are decref'd. Same ownership convention used by the
        // concat-operand and assignment decref sites.
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

    // Free an owned container temp (list/set/tuple/dict) once printed - print
    // borrows, so `print(list(filter(f, xs)))`, `print([c for c in xs])`,
    // `print(d.copy())` etc. otherwise leak the whole container per call. The
    // box and str branches above already decref their own owned temps and
    // return early / fall through, so this only fires for the container and
    // class-instance pointer printers. A borrowed arg (a NameExpr/field/
    // element read) keeps its owner's reference and is rejected by
    // isOwnedPtrResult; instances built inline (`print(Foo())`) are owned and
    // correctly dropped here.
    if (impl_->options.gcMode == GCMode::RC && staticContainerVal &&
        arg->getType()->isPointerTy() && impl_->isOwnedPtrResult(arg)) {
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {arg});
    }
}

bool CodeGen::emitBuiltinCall(CallExpr& node, const std::string& name) {
    // Owned heap temporaries materialized for a BORROW-builtin arg slot
    // (len(a+b), int(s+t), str(x), sorted(make()), ...). These builtins read
    // their argument and return a fresh result without storing it, so an owned
    // temp must be released after the call or it leaks once per call. Released
    // once at the common tail. print and the list()/set() constructors manage
    // their own args (they never call trackBorrowTemp), so they stay out of this
    // sink. (#3 class A, builtin borrow site.)
    std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
    bool builtinHandled = [&]() -> bool {
    // print(*args) -- Python semantics: args printed via the _raw printers
    // (no per-arg newline), separated by a single space, with one trailing
    // newline (sep=' ', end='\n'). Per-arg type dispatch lives in
    // emitPrintArgRaw so single- and multi-arg formatting are identical.
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

    // issubclass(cls, base) - compile-time constant fold over the single-
    // inheritance class hierarchy (commandment #1: zero runtime cost). `object`
    // is the universal base; a class is a subclass of itself; otherwise walk the
    // parent chain. Builtin type names (int/str/...) aren't user classes, so
    // they only match themselves or `object` - matching Python for static args.
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
                auto pit = impl_->classParentNames.find(cur);
                if (pit == impl_->classParentNames.end()) break;
                cur = pit->second;
                if (cur == base) { result = true; break; }
            }
        }
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, result ? 1 : 0);
        return true;
    }
    // map(f, xs) - desugar to the list comprehension [f(__map_x) for __map_x in
    // xs] and reuse the (already typed + monomorphized) ListCompExpr lowering.
    // The element type comes from the callable's return type (TypeChecker typed
    // the whole map() call as list[returnType]); seeding the synthetic call's
    // type lets Comprehensions.cpp pick the right elemTag (int/f64/ptr). The
    // loop var carries the iterable's element type so f's argument coerces.
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

    // filter(f, xs) -> [x for x in xs if f(x)]. Desugared to a list
    // comprehension with a condition, mirroring map() above - same codegen,
    // no extra abstraction. The result element type is xs's element type
    // (filter never transforms), so a `list[T]` input yields `list[T]`.
    if (name == "filter" && node.args.size() == 2) {
        std::shared_ptr<Type> iterElemType;
        if (node.args[1]->type && node.args[1]->type->kind() == Type::Kind::List)
            iterElemType = static_cast<ListType&>(*node.args[1]->type).elementType;

        // The predicate is applied to each element to form the comp condition.
        auto predVar = std::make_unique<NameExpr>();
        predVar->name = "__filter_x";
        predVar->type = iterElemType;
        auto cond = std::make_unique<CallExpr>();
        cond->callee = std::move(node.args[0]);
        cond->args.push_back(std::move(predVar));
        // No static type needed: the comprehension condition codegen evaluates
        // the predicate call and tests its result for truthiness directly.

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

    // len()
    if (name == "len" && node.args.size() == 1) {
        // Check if the argument is a dict variable
        bool isDict = dynamic_cast<DictExpr*>(node.args[0].get()) != nullptr;
        if (!isDict) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isDict = impl_->lookupVarKind(argName->name) == Impl::VarKind::Dict;
            }
        }
        // Check if the argument is a list variable
        bool isList = dynamic_cast<ListExpr*>(node.args[0].get()) != nullptr;
        if (!isList) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isList = impl_->lookupVarKind(argName->name) == Impl::VarKind::List;
            }
        }
        // Check if the argument is a tuple variable
        bool isTuple = dynamic_cast<TupleExpr*>(node.args[0].get()) != nullptr;
        if (!isTuple) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isTuple = impl_->lookupVarKind(argName->name) == Impl::VarKind::Tuple;
            }
        }
        // Check if the argument is a set variable
        bool isSet = dynamic_cast<SetExpr*>(node.args[0].get()) != nullptr;
        if (!isSet) {
            if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                isSet = impl_->lookupVarKind(argName->name) == Impl::VarKind::Set;
            }
        }
        // C4: inline set-method result (`a.union(b)`, `a.copy()`, ...). Must win
        // over the type-kind fallback below, which maps copy()'s ListType result
        // to isList -> dragon_list_len (wrong header offset on a DragonSet*).
        if (!isSet && impl_->resolveExprVarKind(node.args[0].get()) == Impl::VarKind::Set)
            isSet = true;
        // Check class fields (self.field or instance.field) for len()
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
                    auto fkIt = impl_->classFieldKinds.find(className);
                    if (fkIt != impl_->classFieldKinds.end()) {
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
        // Fallback: typechecker-propagated argument type. Catches subscripts
        // (`len(a[i])` where a: list[list[T]]), function-call results
        // (`len(f())`), and any shape the heuristics above miss. Without this,
        // `len(nested[0])` falls through to dragon_str_len on a list ptr and
        // returns garbage (header bytes interpreted as a C string).
        if (!isList && !isDict && !isTuple && !isSet && node.args[0]->type) {
            switch (node.args[0]->type->kind()) {
                case Type::Kind::List:  isList  = true; break;
                case Type::Kind::Dict:  isDict  = true; break;
                case Type::Kind::Tuple: isTuple = true; break;
                case Type::Kind::Set:   isSet   = true; break;
                default: break;
            }
        }
        // Check if the argument is a deque
        bool isDeque = false;
        if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
            isDeque = impl_->lookupVarKind(argName->name) == Impl::VarKind::Deque;
        }
        // Check if the argument is a bytes expression
        bool isBytes = impl_->exprIsBytes(node.args[0].get());
        // __len__ dunder dispatch for class instances
        std::string lenClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        if (!lenClassName.empty() && impl_->hasDunder(lenClassName, "__len__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(lenClassName, "__len__", arg);
            return true;
        }
        if (arg->getType() == impl_->boxType) {
            // A box VALUE wins over every static hint (isinstance narrowing
            // can stamp the arg's static type `list` while the binding stays
            // a box): dragon_box_len dispatches on the tag and the payload
            // header, so either list representation sizes correctly.
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
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        }
        return true;
    }

    // abs()
    if (name == "abs" && node.args.size() == 1) {
        std::string absClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        // Dunder dispatch: __abs__
        if (!absClassName.empty() && impl_->hasDunder(absClassName, "__abs__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(absClassName, "__abs__", arg);
            return true;
        }
        // Dispatch by operand type: float abs goes to dragon_abs_float (fabs),
        // not dragon_abs_int - the latter takes i64 and fails LLVM verify on a
        // double. Bool widens to i64 for the int path.
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

    // int() conversion
    if (name == "int" && node.args.size() == 1) {
        // Class instance with __int__ - dispatch the dunder (Python parity),
        // mirroring str()'s __str__ path. Resolve the class before evaluating
        // the arg so the receiver class is known for dunder dispatch.
        std::string intClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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
            // String -> int via the runtime parser (Python parity for int("200")).
            // Without this, the i8* falls into the "as-is" branch below and the
            // resulting "int" is actually the raw string-data pointer - every
            // downstream use silently receives a pointer where it expects an
            // integer (the kind of bug that took an hour to track down because
            // the inner local printed "200" but every consumer saw garbage).
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_to_int",
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, "stoi");
        } else {
            impl_->lastValue = arg;
        }
        return true;
    }

    // float() conversion
    if (name == "float" && node.args.size() == 1) {
        // Class instance with __float__ - dispatch the dunder (Python parity),
        // mirroring str()'s __str__ path.
        std::string floatClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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
            // float("3.5") - parse the string (Python parity). Previously the
            // str pointer fell through unchanged and was reinterpreted as an
            // f64 bit pattern (garbage). str is the only pointer kind that is a
            // valid float() input. Mirrors how int(str) / json parse numbers.
            auto* fnTy = llvm::FunctionType::get(
                impl_->f64Type, {impl_->i8PtrType}, false);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_to_float", fnTy);
            impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, "stof");
        } else {
            impl_->lastValue = arg;  // already f64
        }
        return true;
    }

    // str() conversion
    if (name == "str" && node.args.size() == 1) {
        // Check for class instance with __str__
        std::string strClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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
            // D039: str(anyValue) - tag-dispatched conversion.
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_to_str"], {arg}, "btos.any");
        } else if (!strClassName.empty() &&
                   impl_->userExcCodes.count(strClassName) > 0) {
            // User-defined exception instance with no __str__/__repr__:
            // Python parity is `str(e) == args[0]` (Exception.__str__). The
            // raise-time snapshot stashes that string in the runtime msg slot,
            // so route str() there - same path print() uses for an exception
            // instance (see emitPrintArgRaw's userExcCodes branch). Without
            // this, the instance ptr falls through unchanged and str() yields
            // "" (containerReprFn returns "" for a non-container ptr).
            // Dup: str() results are OWNED by convention (consumers store
            // without incref / decref after use); handing out the slot's
            // borrowed pointer here would let a store steal the slot's +1
            // and double-free on the next raise.
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
                // VarKind check included: `except E as e` binds e to the
                // message string (VarKind::Str) while its STATIC type is the
                // exception class - str(e) must still retain.
                // str(s) of an already-str value is identity (Python parity,
                // no copy) - but a CallExpr result is OWNED by convention
                // (isBorrowedHeapExpr), so hand the consumer its own +1.
                // Returning the bare borrow made `msg = str(e)` steal the
                // exception slot's reference: scope cleanup then over-released
                // it and the slot dangled (UAF on the next raise).
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_retain"], {arg}, "stos");
            }
        }
        // Non-str pointer with no repr (class instance without __str__):
        // left as-is, matching previous behavior.
        return true;
    }

    // bool() conversion
    if (name == "bool" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        // bool() only READS its arg - an owned temp (`bool([])`,
        // `bool(s.strip())`) is drained by the common argTemps tail.
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(),
                                                  impl_->lastValue, argTemps);
        // Single source of truth: identical truthiness to if/while conditions
        // (numeric != 0; container/string len != 0; __bool__/__len__; else
        // non-null). Previously this returned constant-true for every pointer,
        // so bool("")/bool([]) were wrongly True.
        impl_->lastValue = impl_->toBool(arg, node.args[0].get());
        return true;
    }

    // bytes(list[int]) - construct DragonBytes from a list of int byte values
    // bytes(int) - fresh zero-filled buffer of length n
    // bytes() - empty bytes
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
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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

    // input()
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

    // range() - not a real call; handled by for-loop codegen
    // For other uses, produce 0
    if (name == "range") {
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return true;
    }

    // --- Phase G: Builtin functions ---

    // G.1: min(a, b) or min(list)
    // min/max: one iterable arg -> dragon_{min,max}_list; OR >= 2 scalar args
    // (Python varargs) folded pairwise inline - no runtime variadic, so the
    // comparisons inline and stay branch-predictable.
    if ((name == "min" || name == "max") && !node.args.empty()) {
        bool isMin = (name == "min");
        if (node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* mmArg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[isMin ? "dragon_min_list" : "dragon_max_list"],
                {mmArg}, name);
            return true;
        }
        std::vector<llvm::Value*> vals;
        bool anyFloat = false;
        for (auto& arg : node.args) {
            arg->accept(*this);
            llvm::Value* v = impl_->trackBorrowTemp(arg.get(), impl_->lastValue, argTemps);
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

    // sum(list)
    if (name == "sum" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* sumArg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_sum_list"], {sumArg}, "sum");
        return true;
    }

    // any(list)
    if (name == "any" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* anyArg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        llvm::Value* result = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_any_list"], {anyArg}, "any");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            result, llvm::ConstantInt::get(impl_->i64Type, 0));
        return true;
    }

    // all(list)
    if (name == "all" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* allArg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        llvm::Value* result = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_all_list"], {allArg}, "all");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            result, llvm::ConstantInt::get(impl_->i64Type, 0));
        return true;
    }

    // G.2: enumerate(list) or enumerate(list, start)
    if (name == "enumerate") {
        if (node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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

    // zip(list1, list2)
    if (name == "zip" && node.args.size() == 2) {
        node.args[0]->accept(*this);
        llvm::Value* a = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        node.args[1]->accept(*this);
        llvm::Value* b = impl_->trackBorrowTemp(node.args[1].get(), impl_->lastValue, argTemps);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_zip"], {a, b}, "zip");
        return true;
    }

    // sorted(list) / sorted(list, reverse=...). The optional reverse= keyword
    // selects descending order; without it we keep the cheaper dragon_sorted.
    // (key= is a separate, unimplemented feature - left untouched here.)
    if (name == "sorted" && node.args.size() == 1) {
        Expr* reverseArg = nullptr;
        for (auto& kw : node.kwArgs)
            if (kw.first == "reverse") reverseArg = kw.second.get();
        node.args[0]->accept(*this);  // evaluate the list first (Python order)
        llvm::Value* listv = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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

    // reversed(list)
    if (name == "reversed" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* revArg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_reversed"], {revArg}, "reversed");
        return true;
    }

    // G.3: hash(x)
    if (name == "hash" && node.args.size() == 1) {
        std::string hashClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        // Class instance with __hash__
        if (!hashClassName.empty() && impl_->hasDunder(hashClassName, "__hash__") &&
            (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy())) {
            impl_->lastValue = impl_->callDunder(hashClassName, "__hash__", arg);
            return true;
        }
        if (arg->getType() == impl_->i8PtrType || arg->getType()->isPointerTy()) {
            // Default for class instances: id-based hash (pointer as int)
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

    // id(x)
    if (name == "id" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        if (arg->getType()->isPointerTy()) {
            arg = impl_->builder->CreatePtrToInt(arg, impl_->i64Type);
        } else if (arg->getType() == impl_->i1Type) {
            arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        }
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_id"], {arg}, "id");
        return true;
    }

    // repr(x)
    if (name == "repr" && node.args.size() == 1) {
        std::string reprClassName = impl_->resolveExprClassName(node.args[0].get());
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        // Class instance with __repr__
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

    // G.4: ord(char)
    if (name == "ord" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        // ord borrows its arg (reads the code point, returns an int). An owned
        // heap-string temp - notably ord(s[i]), which mallocs a fresh 1-char
        // string - must be released after the call or it leaks once per call
        // Mirrors chr below; the common tail drains argTemps
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_ord"], {arg}, "ord");
        return true;
    }

    // chr(code)
    if (name == "chr" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        if (arg->getType() == impl_->i1Type)
            arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_chr"], {arg}, "chr");
        return true;
    }

    // __float_bits(f) - reinterpret an f64's raw IEEE-754 bits as an i64
    // (a register-level bitcast, ~0 instructions; NOT a numeric conversion).
    // The user-reachable half of the float<->bytes bridge that struct.pack /
    // unpack and every binary wire codec (msgpack >d, BSON <d, Postgres/MySQL
    // binary, CBOR, .npy) need; the int side is already expressible by
    // arithmetic. Inverse: __float_from_bits.
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

    // __float_from_bits(i) - reinterpret an i64's bit pattern as an f64. The
    // inverse of __float_bits; same zero-cost bitcast.
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

    // __float32_bits(f) - round the f64 to single precision and reinterpret its
    // 32 IEEE-754 bits as an int (in the low 32 bits). For struct's 'f' format
    // and MySQL FLOAT (4-byte) columns. fptrunc + bitcast + zext.
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

    // __float32_from_bits(i) - reinterpret the low 32 bits as a single-precision
    // float, widened to f64. Inverse of __float32_bits.
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

    // isinstance(obj, type_name) - compile-time type check
    if (name == "isinstance" && node.args.size() == 2) {
        // Get the type name from the second argument
        std::string typeName;
        if (auto* typeNameExpr = dynamic_cast<NameExpr*>(node.args[1].get())) {
            typeName = typeNameExpr->name;
        }

        // ADR 025 removal: classes are compile-time entities (D021). The 2nd
        // arg of isinstance must name a class statically (a literal class name,
        // handled above). A variable holding a VarKind::Type value (e.g. a
        // `: type` parameter) is not a class value and cannot be used here -
        // there are no class values or aliases.
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
            // Determine the VarKind of the first argument
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

            // D030 Phase 4: Union-typed variable - runtime tag comparison
            // against the box's tag field.
            if (argKind == Impl::VarKind::Union) {
                int64_t targetTag = -1;
                if (typeName == "int")        targetTag = 0;
                else if (typeName == "str")   targetTag = 1;
                else if (typeName == "float") targetTag = 2;
                else if (typeName == "bool")  targetTag = 3;
                else if (typeName == "list")  targetTag = 5;
                else if (typeName == "dict")  targetTag = 6;
                else if (typeName == "bytes") targetTag = 7;
                // User-defined class type - TAG_CLASS=7 (same slot as TAG_BYTES;
                // both are refcount-managed heap objects). Safe because a Union
                // can't legally hold both a class member and a bytes member at
                // the same time without a typechecker change.
                else if (impl_->classNames.count(typeName)) targetTag = 7;
                if (targetTag >= 0) {
                    // Find the storage for `r`: local alloca first, then module
                    // global. Without the global fallback, module-level union
                    // vars (the common case for `const x: T | None = ...`)
                    // skip the runtime tag check and fall through to the
                    // constant-false path.
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
                // D030 §5: prefer the static type - `bytes` is a heap ptr
                // at the LLVM ABI, so VarKind alone can't disambiguate it
                // from List/Dict/Tuple/Set without the type tag. Fall back
                // D030 §5: bytes is identified solely by the static type
                // now - VarKind::Bytes has been deleted (slots use the
                // generic-heap VarKind::List).
                result = node.args[0] && node.args[0]->type &&
                         node.args[0]->type->kind() == Type::Kind::Bytes;
                (void)argKind;
            }
            bool classCheck = false;
            if (typeName != "int" && typeName != "float" && typeName != "bool" &&
                typeName != "str" && typeName != "list" && typeName != "dict" &&
                typeName != "tuple" && typeName != "set" && typeName != "bytes") {
                // User-defined class: walk the inheritance chain so an instance
                // of a subclass IS an instance of any ancestor (Python parity).
                // Previously this was an exact-name match, so isinstance(dog,
                // Animal) was wrongly False.
                classCheck = true;
                std::string c = argClassName;
                while (!c.empty()) {
                    if (c == typeName) { result = true; break; }
                    auto pit = impl_->classParentNames.find(c);
                    c = (pit != impl_->classParentNames.end()) ? pit->second : std::string();
                }
            }
            // Evaluate args for side effects
            node.args[0]->accept(*this);
            // A statically-matching CLASS check must still test the runtime
            // value: an Optional[T] slot holding None is a null instance and
            // `isinstance(none_value, T)` must be False, not a compile-time
            // True that sends a later method call through a null receiver
            // (that was a real SEGV: ODB.close() on a lock-less handle).
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
        // Fallback: evaluate both args, return false
        node.args[0]->accept(*this);
        node.args[1]->accept(*this);
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

        // D030 Phase 4: Union - runtime switch on the box's tag to return
        // the correct type name string.
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

        // The arg's STATIC type (set by the TypeChecker) is the source of
        // truth and is checked first: it covers literals, which carry no
        // VarKind (`type(5)` has no NameExpr, so argKind stays Other and the
        // old VarKind-only path wrongly produced "object"). VarKind and the
        // class name are the fallback for instances.
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
                // An instance-valued arg - variable OR expression (`type(Dog())`).
                // The class name comes from the static type, since a non-NameExpr
                // arg carries no VarKind / argClassName.
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
                // ADR 025 removal: classes are compile-time entities (D021).
                // type(instance) returns the class NAME STRING ("Dog"), never
                // a runtime class descriptor.
                typeName = argClassName.empty() ? "object" : argClassName;
                break;
            default: typeName = "object"; break;
        }
        // Evaluate arg for side effects; an owned temp (`type(Dog("rex"))`
        // builds an instance nobody keeps) is drained by the argTemps tail.
        node.args[0]->accept(*this);
        impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        impl_->lastValue = impl_->builder->CreateGlobalString(typeName);
        return true;
    }

    // round(x)
    if (name == "round" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
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

    // pow(base, exp) - upgrade to float version when needed
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

    // divmod(a, b) -> tuple
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

    // hex(x), oct(x), bin(x)
    if (name == "hex" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        if (arg->getType() == impl_->i1Type) arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_hex"], {arg}, "hex");
        return true;
    }
    if (name == "oct" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        if (arg->getType() == impl_->i1Type) arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_oct"], {arg}, "oct");
        return true;
    }
    if (name == "bin" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
        if (arg->getType() == impl_->i1Type) arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_bin"], {arg}, "bin");
        return true;
    }

    // G.5: list() constructor (empty or from iterable - only empty for now)
    if (name == "list" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_new"],
            {llvm::ConstantInt::get(impl_->i64Type, 8)}, "list");
        return true;
    }

    // list(iterable) - materialize a fresh list (Python parity: list(x) always
    // copies). Gated to list-typed args (sets are also ListType in the checker
    // but are a distinct runtime struct, so exclude them).
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
            // dragon_list_copy makes an independent +1 copy. If the source is
            // an OWNED temp (a comprehension, map()/filter(), or a fresh-list
            // call result - `list(filter(f, xs))`), it has a +1 nobody else
            // holds; release it or it leaks the whole source list each call.
            // A borrowed source (a NameExpr/field) keeps its owner's ref.
            if (impl_->options.gcMode == GCMode::RC &&
                !Impl::isBorrowedHeapExpr(a))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {src});
            return true;
        }
    }

    // dict() constructor (empty)
    if (name == "dict" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_new"],
            {llvm::ConstantInt::get(impl_->i64Type, 8)}, "dict");
        return true;
    }

    // set() constructor (empty)
    if (name == "set" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_new"], {}, "set");
        return true;
    }

    // set(list) constructor - seed a set from a list's elements. Sets are typed
    // as ListType in the checker, so a set arg would also report Kind::List;
    // gate on VarKind::List (and list literals) so set(aSet) doesn't misroute a
    // DragonSet* into the list path. Other iterables (str/tuple/range) fall
    // through to the generic unknown-call error for now.
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
            // dragon_set_from_list increfs the elements it keeps; an owned
            // source-list temp (`set([c for c in xs])`, `set(filter(...))`)
            // must still be released or the source list leaks. Borrowed
            // sources keep their owner's reference.
            if (impl_->options.gcMode == GCMode::RC &&
                !Impl::isBorrowedHeapExpr(a))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {lst});
            return true;
        }
    }

    // tuple() constructor (empty)
    if (name == "tuple" && node.args.empty()) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_tuple_new"],
            {llvm::ConstantInt::get(impl_->i64Type, 0)}, "tuple");
        return true;
    }
    // tuple(iterable) - build a tuple from a list/set (both lower to the
    // DragonList layout). The element tag travels with the list, so one
    // runtime converter handles every element type.
    if (name == "tuple" && node.args.size() == 1) {
        node.args[0]->accept(*this);
        // The converter COPIES the elements into a fresh tuple - an owned
        // list temp (`tuple([1, 2, 3])`) is drained by the argTemps tail.
        llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(),
                                                  impl_->lastValue, argTemps);
        auto* fn = impl_->getOrDeclareRuntime("dragon_tuple_from_list",
            llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType}, false));
        impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, "tuplefromlist");
        return true;
    }


    // Lock() constructor - returns a new mutex handle
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

    // deque() constructor - deque(), deque(iterable), deque(iterable, maxlen)
    // and the maxlen= keyword all supported; maxlen is ENFORCED by the runtime
    // (append past the bound discards the far end, Python semantics).
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
            // deque(iterable) - construct from list (elem tag copied from it).
            // dragon_deque_from_list COPIES + increfs each element, so the
            // source list is BORROWED: an owned list-literal temp
            // (`deque([1,2,3])`) must be drained here or it leaks one list +
            // buffer per call (deque source-list). A borrowed
            // list var (`deque(xs)`) is a Name and is left untouched.
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

    // H.2: super() -> returns self pointer (for simple single-inheritance super dispatch)
    if (name == "super" && node.args.empty()) {
        // In a method body, super() returns self - the dispatch happens at call site
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

    // hasattr(obj, "name") -> bool
    if (name == "hasattr" && node.args.size() == 2) {
        node.args[0]->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // Ensure obj is i64 (tagged pointer)
        if (obj->getType()->isPointerTy())
            obj = impl_->builder->CreatePtrToInt(obj, impl_->i64Type);
        node.args[1]->accept(*this);
        llvm::Value* attrName = impl_->lastValue;
        // attrName is a string (i8*)
        if (attrName->getType() == impl_->i64Type)
            attrName = impl_->builder->CreateIntToPtr(attrName, impl_->i8PtrType);
        auto* result = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_hasattr"], {obj, attrName}, "hasattr");
        impl_->lastValue = impl_->builder->CreateICmpNE(
            result, llvm::ConstantInt::get(impl_->i64Type, 0));
        return true;
    }

    // Internal intrinsic: __exc_matches(expected_code) -> bool. Reads the
    // currently-handled exception's type code (dragon_exc_get_type) and
    // range-matches it against the expected exception-type code via
    // dragon_exc_matches. Must be called inside an `except` handler (the
    // raised code stays set until the next raise). Backs assertRaises with
    // the fast integer-code + [lo,hi] model - no descriptor walk.
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

    // D033 Phase 2: dir(obj) / dir(Cls) -> list[str] of attribute names.
    // Routes to dragon_dir which walks the class MRO for fields + methods
    // (and __init__ if present), sorts, and returns a refcounted list[str].
    // The second runtime arg is the `is_descriptor` flag - 0 for an instance,
    // 1 for a bare class name (D025-style first-class class descriptor).
    if (name == "dir" && node.args.size() == 1) {
        // Detect dir(ClassName) - arg is a NameExpr referring to a known
        // class. Constant-fold to the descriptor global load + is_descriptor=1.
        if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
            if (impl_->classNames.count(argName->name)) {
                auto descIt = impl_->classDescriptorGlobals.find(argName->name);
                if (descIt != impl_->classDescriptorGlobals.end()) {
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
        // dir(instance) - evaluate arg, coerce to i64, call with is_descriptor=0.
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

    // getattr(obj, "name") or getattr(obj, "name", default) -> value
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
    if (builtinHandled) {
        for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
        return true;
    }
    return false;
}

} // namespace dragon
