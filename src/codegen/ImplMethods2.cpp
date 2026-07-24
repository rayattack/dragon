/// CodeGen::Impl out-of-line methods (part 2): RC stores, union box RC, var
/// binding, type-expr mapping, fire/vthread trampolines.
#include "../CodeGenImpl.h"

namespace dragon {

CodeGen::Impl::VarKind CodeGen::Impl::inferYieldKind(const std::vector<std::unique_ptr<Stmt>>& body) {
        struct YieldKindFinder : public DefaultASTVisitor {
            VarKind kind = VarKind::Int;
            bool found = false;
            Impl* self;
            YieldKindFinder(Impl* s) : self(s) {}
            void visit(YieldExpr& y) override {
                if (found) return;
                if (!y.value) return;
                Type::Kind tk = y.value->type ? y.value->type->kind() : Type::Kind::Unknown;
                kind = typeKindToVarKind(tk);
                found = true;
            }
            void visit(FunctionDecl&) override {}
            void visit(ClassDecl&) override {}
        };
        YieldKindFinder finder(this);
        for (auto& stmt : body) {
            if (finder.found) break;
            stmt->accept(finder);
        }
        return finder.kind;
    }

void CodeGen::Impl::fillDefaultArgs(const std::string& funcName, llvm::Function* func,
                     std::vector<llvm::Value*>& args, CodeGen& cg,
                     std::vector<std::pair<llvm::Value*, VarKind>>* defaultTemps) {
        auto it = funcParamDefaults.find(funcName);
        if (it == funcParamDefaults.end()) return;
        auto& defaults = it->second;
        auto funcType = func->getFunctionType();
        unsigned numParams = funcType->getNumParams();
        // Size args to numParams with nullptr placeholders (no-op when equal).
        if (args.size() < numParams)
            args.resize(numParams, nullptr);
        // Cross-module defaults: a default expr resolves in the function's DEFINING
        // module, so swap currentModuleName for the eval (RAII-restored).
        struct ModuleScope {
            std::string& slot;
            std::string saved;
            ModuleScope(std::string& s, const std::string& newName)
                : slot(s), saved(s) { s = newName; }
            ~ModuleScope() { slot = saved; }
        };
        auto mIt = funcDefiningModule.find(funcName);
        const bool needSwap = mIt != funcDefiningModule.end() &&
                              mIt->second != currentModuleName;
        for (size_t i = 0; i < numParams && i < defaults.size(); ++i) {
            if (args[i] != nullptr) continue;
            if (!defaults[i]) continue;  // missing-required-arg surfaces later
            if (needSwap) {
                ModuleScope ms(currentModuleName, mIt->second);
                defaults[i]->accept(cg);
            } else {
                defaults[i]->accept(cg);
            }
            llvm::Value* val = lastValue;
            // A default that made an owned heap temp (= [10,20,30]) leaks unless
            // released; classify by the param kind (the default expr has no ->type here).
            if (defaultTemps) {
                auto pkIt = funcParamKinds.find(funcName);
                if (pkIt != funcParamKinds.end() && i < pkIt->second.size()) {
                    VarKind dk = argTempDecrefKind(defaults[i], pkIt->second[i], val);
                    if (dk != VarKind::Other) defaultTemps->emplace_back(val, dk);
                }
            }
            val = coerceArg(val, funcType->getParamType((unsigned)i));
            args[i] = val;
        }
    }

void CodeGen::Impl::emitIncrefByKind(llvm::Value* val, VarKind kind) {
        if (options.gcMode != GCMode::RC) return;
        if (!isHeapKind(kind)) return;
        if (kind == VarKind::Union) {
            // Incref a Union box's payload by runtime tag (retain half of the Any
            // borrow contract); without it a retained box is freed twice (UAF, A/B-proven).
            if (val && val->getType() == boxType)
                emitUnionIncref(boxPayloadI64(val, "u.inc.p"),
                                boxTag(val, "u.inc.t"));
            return;
        }
        auto* ptr = toI8Ptr(val);
        if (!ptr) return;
        if (kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_incref_str"], {ptr});
        } else if (kind == VarKind::Closure) {
            // A Callable slot may hold a DragonClosure or a bare fn ptr; the
            // tag-gated dragon_incref_callable no-ops on the headerless bare fn.
            builder->CreateCall(runtimeFuncs["dragon_incref_callable"], {ptr});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_incref"], {ptr});
        }
    }

llvm::Value* CodeGen::Impl::cellI64ToNative(llvm::Value* i64Val, VarKind kind) {
        switch (kind) {
            case VarKind::Bool:
                return builder->CreateICmpNE(i64Val,
                    llvm::ConstantInt::get(i64Type, 0), "cell.tobool");
            case VarKind::Float:
                return builder->CreateBitCast(i64Val, f64Type, "cell.tofloat");
            case VarKind::Str:
            case VarKind::StrLiteral:
            case VarKind::List:
            case VarKind::Dict:
            case VarKind::Tuple:
            case VarKind::Set:
            case VarKind::File:
            case VarKind::ClassInstance:
            case VarKind::Generator:
            case VarKind::Closure:
                return builder->CreateIntToPtr(i64Val, i8PtrType, "cell.toptr");
            default:
                return i64Val;  // Int / Other - keep as i64
        }
    }

void CodeGen::Impl::emitUnionDecref(llvm::Value* val, llvm::Value* tag) {
        if (options.gcMode != GCMode::RC) return;
        auto* func = currentFunction;
        auto* isStrBB = llvm::BasicBlock::Create(*context, "union.decref.str", func);
        auto* isOtherHeapBB = llvm::BasicBlock::Create(*context, "union.decref.heap", func);
        auto* endBB = llvm::BasicBlock::Create(*context, "union.decref.end", func);

        // Check if tag == 1 (STR) -> decref_str
        auto* isStr = builder->CreateICmpEQ(tag, llvm::ConstantInt::get(i64Type, 1), "is.str");
        auto* notStrBB = llvm::BasicBlock::Create(*context, "union.decref.notstr", func);
        builder->CreateCondBr(isStr, isStrBB, notStrBB);

        builder->SetInsertPoint(isStrBB);
        auto* strPtr = builder->CreateIntToPtr(val, i8PtrType);
        builder->CreateCall(runtimeFuncs["dragon_decref_str"], {strPtr});
        builder->CreateBr(endBB);

        builder->SetInsertPoint(notStrBB);
        // tag==10 (TAG_CLOSURE) -> tag-gated decref_callable, before the >=5 heap
        // branch: it frees a real closure (+env) and no-ops a bare fn ptr.
        auto* isClosure = builder->CreateICmpEQ(tag, llvm::ConstantInt::get(i64Type, 10), "is.clos");
        auto* closBB = llvm::BasicBlock::Create(*context, "union.decref.clos", func);
        auto* notClosBB = llvm::BasicBlock::Create(*context, "union.decref.notclos", func);
        builder->CreateCondBr(isClosure, closBB, notClosBB);

        builder->SetInsertPoint(closBB);
        auto* closPtr = builder->CreateIntToPtr(val, i8PtrType);
        builder->CreateCall(runtimeFuncs["dragon_decref_callable"], {closPtr});
        builder->CreateBr(endBB);

        builder->SetInsertPoint(notClosBB);
        // Check if tag >= 5 (LIST/DICT/BYTES/...) -> decref
        auto* isHeap = builder->CreateICmpSGE(tag, llvm::ConstantInt::get(i64Type, 5), "is.heap");
        builder->CreateCondBr(isHeap, isOtherHeapBB, endBB);

        builder->SetInsertPoint(isOtherHeapBB);
        auto* heapPtr = builder->CreateIntToPtr(val, i8PtrType);
        builder->CreateCall(runtimeFuncs["dragon_decref"], {heapPtr});
        builder->CreateBr(endBB);

        builder->SetInsertPoint(endBB);
    }

void CodeGen::Impl::emitUnionIncref(llvm::Value* val, llvm::Value* tag) {
        if (options.gcMode != GCMode::RC) return;
        auto* func = currentFunction;
        auto* isStrBB = llvm::BasicBlock::Create(*context, "union.incref.str", func);
        auto* isOtherHeapBB = llvm::BasicBlock::Create(*context, "union.incref.heap", func);
        auto* endBB = llvm::BasicBlock::Create(*context, "union.incref.end", func);

        auto* isStr = builder->CreateICmpEQ(tag, llvm::ConstantInt::get(i64Type, 1), "is.str");
        auto* notStrBB = llvm::BasicBlock::Create(*context, "union.incref.notstr", func);
        builder->CreateCondBr(isStr, isStrBB, notStrBB);

        builder->SetInsertPoint(isStrBB);
        auto* strPtr = builder->CreateIntToPtr(val, i8PtrType);
        builder->CreateCall(runtimeFuncs["dragon_incref_str"], {strPtr});
        builder->CreateBr(endBB);

        builder->SetInsertPoint(notStrBB);
        // tag == 10 (TAG_CLOSURE) -> tag-gated dragon_incref_callable (must
        // precede the generic `>= 5` branch). Mirrors emitUnionDecref.
        auto* isClosure = builder->CreateICmpEQ(tag, llvm::ConstantInt::get(i64Type, 10), "is.clos");
        auto* closBB = llvm::BasicBlock::Create(*context, "union.incref.clos", func);
        auto* notClosBB = llvm::BasicBlock::Create(*context, "union.incref.notclos", func);
        builder->CreateCondBr(isClosure, closBB, notClosBB);

        builder->SetInsertPoint(closBB);
        auto* closPtr = builder->CreateIntToPtr(val, i8PtrType);
        builder->CreateCall(runtimeFuncs["dragon_incref_callable"], {closPtr});
        builder->CreateBr(endBB);

        builder->SetInsertPoint(notClosBB);
        auto* isHeap = builder->CreateICmpSGE(tag, llvm::ConstantInt::get(i64Type, 5), "is.heap");
        builder->CreateCondBr(isHeap, isOtherHeapBB, endBB);

        builder->SetInsertPoint(isOtherHeapBB);
        auto* heapPtr = builder->CreateIntToPtr(val, i8PtrType);
        builder->CreateCall(runtimeFuncs["dragon_incref"], {heapPtr});
        builder->CreateBr(endBB);

        builder->SetInsertPoint(endBB);
    }

bool CodeGen::Impl::consumeBorrowedSlot(const std::string& name) {
        if (name.empty()) return false;
        // Find the owning scope (the innermost scope that binds `name`, mirroring
        // setVar's reverse walk) and consult/clear its borrowed mark there.
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            bool hasVar = it->vars.count(name) != 0;
            bool isBorrowed = it->borrowed.count(name) != 0;
            if (hasVar || isBorrowed) {
                if (isBorrowed) {
                    it->borrowed.erase(name);
                    return true;
                }
                return false;  // owned binding shadows any outer borrowed mark
            }
        }
        return false;
    }

void CodeGen::Impl::storeWithRCOverwrite(llvm::Value* slotPtr, llvm::Type* slotValueType,
                          llvm::Value* newVal,
                          VarKind oldKind, VarKind newKind,
                          bool newIsBorrowed,
                          const std::string& name) {
        bool didIncrefNew = (options.gcMode == GCMode::RC &&
                             isHeapKind(newKind) &&
                             newIsBorrowed);
        if (didIncrefNew) {
            emitIncrefByKind(newVal, newKind);
        }

        // A borrowed slot (param/loop-var/capture/self) holds a caller-owned value:
        // the first reassignment must NOT decref it (would steal a caller's ref).
        bool slotWasBorrowed =
            (options.gcMode == GCMode::RC && llvm::isa<llvm::AllocaInst>(slotPtr) &&
             consumeBorrowedSlot(name));

        if (options.gcMode == GCMode::RC && isHeapKind(oldKind) && !slotWasBorrowed) {
            auto* oldVal = builder->CreateLoad(
                slotValueType, slotPtr, name.empty() ? "old.rc" : (name + ".oldrc"));
            llvm::Value* oldToDrop = oldVal;

            auto* oldPtr = toI8Ptr(oldVal);
            auto* newPtr = toI8Ptr(newVal);
            if (oldPtr && newPtr) {
                auto* same = builder->CreateICmpEQ(oldPtr, newPtr, "rc.same");
                // Same-pointer overwrite (x = x): decref once if new was incref'd,
                // else skip to avoid dropping the only live reference.
                if (!didIncrefNew) {
                    auto* nullPtr = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(i8PtrType));
                    oldToDrop = builder->CreateSelect(same, nullPtr, oldPtr, "rc.drop");
                }
            }

            emitDecrefByKind(oldToDrop, oldKind);
        }

        builder->CreateStore(newVal, slotPtr);

        // Register/refresh the unwind snapshot for owned heap locals (alloca only):
        // heap oldKind = reassignment (refresh), else fresh declaration (push).
        if (options.gcMode == GCMode::RC && !name.empty() &&
            isHeapKind(newKind) && newKind != VarKind::Union &&
            llvm::isa<llvm::AllocaInst>(slotPtr)) {
            int ck = cleanupKindFor(newKind);
            // A previously-borrowed slot has no prior unwind entry, so push a fresh one.
            if (isHeapKind(oldKind) && !slotWasBorrowed)
                emitCleanupUpdate(name, newVal);
            else
                emitCleanupPush(name, newVal, ck);
        }
    }

void CodeGen::Impl::emitStrAppendInplace(llvm::Value* slotPtr, llvm::Value* cur,
                          llvm::Value* rhs, const std::string& name) {
        // A str operand can flow as i64 (a field loaded as int); coerce to ptr
        // before the call, like dragon_str_concat.
        if (cur->getType() == i64Type) cur = builder->CreateIntToPtr(cur, i8PtrType);
        if (rhs->getType() == i64Type) rhs = builder->CreateIntToPtr(rhs, i8PtrType);
        llvm::Value* result = builder->CreateCall(
            runtimeFuncs["dragon_str_append_inplace"], {cur, rhs}, "strapp");
        builder->CreateStore(result, slotPtr);
        if (options.gcMode == GCMode::RC && isOwnedStrResult(rhs)) {
            builder->CreateCall(runtimeFuncs["dragon_decref_str"], {rhs});
        }
        if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(slotPtr)) {
            setVar(name, ai, VarKind::Str);
            // append_inplace consumed the old value; refresh the unwind entry, else
            // a later raise double-frees the realloc'd pointer.
            emitCleanupUpdate(name, result);
        } else {
            moduleGlobalKinds[name] = VarKind::Str;
        }
    }

void CodeGen::Impl::setVar(const std::string& name, llvm::AllocaInst* alloca,
             VarKind kind) {
        if (scopes.empty()) return;
        // If this exact slot already lives in an outer scope, refresh it there (a
        // reassignment), not shadow it; a fresh alloca for the name is a real shadow.
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->vars.find(name);
            if (found != it->vars.end()) {
                if (found->second == alloca) {
                    it->varKinds[name] = kind;
                    return;
                }
                break;  // same name, different slot -> genuine shadow
            }
        }
        scopes.back().vars[name] = alloca;
        scopes.back().varKinds[name] = kind;
    }

bool CodeGen::Impl::exprIsBytes(Expr* expr) {
        if (expr && expr->type && expr->type->kind() == Type::Kind::Bytes)
            return true;
        if (auto* lit = dynamic_cast<StringLiteral*>(expr)) {
            return lit->isBytes;
        }
        if (dynamic_cast<NameExpr*>(expr)) {
            // bytes-typed slots collapse to VarKind::List; the static-type check
            // above already handles resolved NameExprs.
            return false;
        }
        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            // bytes[int] -> int (byte value), bytes[slice] -> bytes; treating a
            // single-byte read as bytes fails the verifier.
            if (dynamic_cast<SliceExpr*>(sub->index.get())) {
                return exprIsBytes(sub->object.get());
            }
            return false;
        }
        if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
            return exprIsBytes(bin->left.get()) || exprIsBytes(bin->right.get());
        }
        // Class field access (self.x): the AttributeExpr's static type is primary;
        // classFieldKinds is the same-class-body fallback.
        if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
            std::string className;
            if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
                if (objName->name == "self" && !currentClassName.empty())
                    className = currentClassName;
                else {
                    auto vit = varClassNames.find(objName->name);
                    if (vit != varClassNames.end()) className = vit->second;
                }
            }
            // bytes-vs-list disambiguation flows through the static type above;
            // classFieldKinds tags bytes fields as List, so it can't match here.
            (void)className;
            return false;
        }
        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            if (auto* attr = dynamic_cast<AttributeExpr*>(call->callee.get())) {
                // bytes.fromhex("...") - static call
                if (auto* calleeName = dynamic_cast<NameExpr*>(attr->object.get())) {
                    if (calleeName->name == "bytes") return true;
                }
                std::string method = attr->attribute;
                // str.encode() returns bytes
                if (method == "encode") return !exprIsBytes(attr->object.get());
                // bytes.decode()/hex() return str, not bytes
                if (method == "decode" || method == "hex") return false;
                // Other bytes methods return bytes
                return exprIsBytes(attr->object.get());
            }
            // Direct name call: bytes(...) or user fn whose return type is bytes-shaped
            if (auto* calleeName = dynamic_cast<NameExpr*>(call->callee.get())) {
                if (calleeName->name == "bytes") return true;
                // User function: a return type of `ptr` doesn't tell us bytes vs list,
                // so fall back to AST type info if the typechecker propagated it.
            }
            // Typechecker propagation
            if (call->type && call->type->kind() == Type::Kind::Bytes) return true;
        }
        return false;
    }

llvm::AllocaInst* CodeGen::Impl::createEntryAlloca(llvm::Function* func,
                                     const std::string& name,
                                     llvm::Type* type) {
        llvm::IRBuilder<> tmpBuilder(&func->getEntryBlock(),
                                     func->getEntryBlock().begin());
        auto* alloca = tmpBuilder.CreateAlloca(type, nullptr, name);
        // Zero-init pointer allocas so scope-exit cleanup of a slot never written
        // (an early return) sees NULL (decref/incref no-op) instead of garbage.
        if (type->isPointerTy()) {
            auto* nullPtr = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(type));
            tmpBuilder.CreateStore(nullPtr, alloca);
        } else if (type == boxType) {
            // Same for a box slot: zero-init to {tag=0, payload=0} so cleanup of an
            // unwritten Union/Any local sees TAG_INT (no-op), not garbage.
            tmpBuilder.CreateStore(
                llvm::ConstantAggregateZero::get(boxType), alloca);
        }
        return alloca;
    }

std::string CodeGen::Impl::typeExprCanonicalName(TypeExpr* t) const {
        if (auto* n = dynamic_cast<NamedTypeExpr*>(t)) {
            if (n->name == "intc") return "int";       // TypeChecker aliases intc->int
            if (n->name == "object") return "Any";     // object->Any
            return n->name;
        }
        if (auto* g = dynamic_cast<GenericTypeExpr*>(t)) {
            auto* base = dynamic_cast<NamedTypeExpr*>(g->base.get());
            std::string b = base ? base->name : "";
            if ((b == "list" || b == "List") && g->typeArgs.size() == 1)
                return "list[" + typeExprCanonicalName(g->typeArgs[0].get()) + "]";
            if ((b == "set" || b == "Set") && g->typeArgs.size() == 1)  // set aliased to list[T]
                return "list[" + typeExprCanonicalName(g->typeArgs[0].get()) + "]";
            if ((b == "dict" || b == "Dict") && g->typeArgs.size() == 2)
                return "dict[" + typeExprCanonicalName(g->typeArgs[0].get()) + ", " +
                       typeExprCanonicalName(g->typeArgs[1].get()) + "]";
            if (b == "tuple" || b == "Tuple") {
                std::string s = "tuple[";
                for (size_t i = 0; i < g->typeArgs.size(); ++i) {
                    if (i) s += ", ";
                    s += typeExprCanonicalName(g->typeArgs[i].get());
                }
                return s + "]";
            }
            if (b == "Task" && g->typeArgs.size() == 1)
                return "Task[" + typeExprCanonicalName(g->typeArgs[0].get()) + "]";
            // User-defined generic class - mangleInstantiation form (comma join).
            std::string s = b + "[";
            for (size_t i = 0; i < g->typeArgs.size(); ++i) {
                if (i) s += ",";
                s += typeExprCanonicalName(g->typeArgs[i].get());
            }
            return s + "]";
        }
        return "";
    }

Type::Kind CodeGen::Impl::typeExprToTypeKind(TypeExpr* typeExpr) {
        if (!typeExpr) return Type::Kind::Int;
        if (auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr)) {
            if (named->name == "int") return Type::Kind::Int;
            if (named->name == "float") return Type::Kind::Float;
            if (named->name == "bool") return Type::Kind::Bool;
            if (named->name == "str") return Type::Kind::Str;
            if (named->name == "bytes") return Type::Kind::Bytes;
            if (named->name == "list" || named->name == "List") return Type::Kind::List;
            if (named->name == "dict" || named->name == "Dict") return Type::Kind::Dict;
            if (named->name == "tuple" || named->name == "Tuple") return Type::Kind::Tuple;
            if (named->name == "set" || named->name == "Set") return Type::Kind::Set;
            // Any maps to Kind::Any (was falling to Int, breaking dict[str, Any] reads).
            if (named->name == "Any" || named->name == "object") return Type::Kind::Any;
            if (typedDictClasses.count(named->name)) return Type::Kind::Dict;
            if (!resolveAnnotationClassName(named->name).empty())
                return Type::Kind::Instance;
            return Type::Kind::Int;
        }
        if (auto* generic = dynamic_cast<GenericTypeExpr*>(typeExpr)) {
            if (auto* base = dynamic_cast<NamedTypeExpr*>(generic->base.get())) {
                if (base->name == "list" || base->name == "List")  return Type::Kind::List;
                if (base->name == "dict" || base->name == "Dict")  return Type::Kind::Dict;
                if (base->name == "tuple" || base->name == "Tuple") return Type::Kind::Tuple;
                if (base->name == "set" || base->name == "Set")    return Type::Kind::Set;
            }
            // D044 - a user generic-class instantiation (`Box[int]`) is an instance.
            if (!genericInstanceClassName(typeExpr).empty()) return Type::Kind::Instance;
            return Type::Kind::Int;
        }
        if (dynamic_cast<TupleTypeExpr*>(typeExpr)) return Type::Kind::Tuple;
        // Callable[...] is a refcounted DragonClosure ptr; without this list[Callable]
        // tracked it as Int and leaked overwritten closures.
        if (dynamic_cast<CallableTypeExpr*>(typeExpr)) return Type::Kind::Function;
        if (auto* unionType = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
            if (TypeExpr* niche = unionNicheMember(typeExpr))
                return typeExprToTypeKind(niche);
            return Type::Kind::Int;
        }
        return Type::Kind::Int;
    }

CodeGen::Impl::VarKind CodeGen::Impl::typeExprToKind(TypeExpr* typeExpr) {
        if (!typeExpr) return VarKind::Other;
        if (auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr)) {
            if (named->name == "int") return VarKind::Int;
            if (named->name == "float") return VarKind::Float;
            if (named->name == "bool") return VarKind::Bool;
            if (named->name == "str") return VarKind::Str;
            if (named->name == "bytes") return VarKind::List;  // D030 §5: bytes uses generic-heap VarKind; bytes-ness flows via Type::Kind
            if (named->name == "type") return VarKind::Type;
            if (named->name == "list" || named->name == "List") return VarKind::List;
            if (named->name == "dict" || named->name == "Dict") return VarKind::Dict;
            if (named->name == "tuple" || named->name == "Tuple") return VarKind::Tuple;
            if (named->name == "set" || named->name == "Set") return VarKind::Set;
            // Any reuses the Union box infrastructure; its unionMemberKinds is left
            // empty (any tag valid at runtime).
            if (named->name == "Any" || named->name == "object") return VarKind::Union;
            // Lock is an intrinsic ptr handle (VarKind::Other), so calls dispatch via
            // the __Lock fast path, not class-instance dispatch.
            if (named->name == "Lock") return VarKind::Other;
            // TypedDict classes are dicts at runtime
            if (typedDictClasses.count(named->name))
                return VarKind::Dict;
            // GC Phase 3: user-defined class types get ClassInstance kind
            if (!resolveAnnotationClassName(named->name).empty())
                return VarKind::ClassInstance;
            return VarKind::Other;
        }
        if (auto* generic = dynamic_cast<GenericTypeExpr*>(typeExpr)) {
            // list[int], dict[str, int], etc. - check base type name
            if (auto* base = dynamic_cast<NamedTypeExpr*>(generic->base.get())) {
                if (base->name == "list" || base->name == "List")
                    return VarKind::List;
                if (base->name == "dict" || base->name == "Dict")
                    return VarKind::Dict;
                if (base->name == "tuple" || base->name == "Tuple")
                    return VarKind::Tuple;
                if (base->name == "set" || base->name == "Set")
                    return VarKind::Set;
            }
            // D044 - a user generic-class instantiation (`Box[int]`) is a class
            // instance (a heap ptr to the stamped struct), not an opaque ptr.
            if (!genericInstanceClassName(typeExpr).empty())
                return VarKind::ClassInstance;
            return VarKind::Other;
        }
        if (auto* tupleType = dynamic_cast<TupleTypeExpr*>(typeExpr)) {
            return VarKind::Tuple;
        }
        // A `: Callable[...]` slot holds a refcounted DragonClosure (or bare fn ptr):
        // a scope-cleaned heap kind, safe because closure RC is tag-gated everywhere.
        if (dynamic_cast<CallableTypeExpr*>(typeExpr)) {
            return VarKind::Closure;
        }
        if (auto* unionType = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
            // Niche-pointer: `Ptr | None` reads at the pointer's VarKind; a null
            // ptr encodes None, no boxing.
            if (TypeExpr* niche = unionNicheMember(typeExpr))
                return typeExprToKind(niche);
            return VarKind::Union;
        }
        return VarKind::Other;
    }

TypeExpr* CodeGen::Impl::unionNicheMember(TypeExpr* typeExpr) {
        auto* ut = dynamic_cast<UnionTypeExpr*>(typeExpr);
        if (!ut || ut->types.size() != 2) return nullptr;
        TypeExpr* noneSide = nullptr;
        TypeExpr* otherSide = nullptr;
        for (auto& t : ut->types) {
            auto* nm = dynamic_cast<NamedTypeExpr*>(t.get());
            if (nm && nm->name == "None")
                noneSide = t.get();
            else
                otherSide = t.get();
        }
        if (!noneSide || !otherSide) return nullptr;
        // Other side must be a pointer-shaped type. Non-pointer members
        // (int, float, bool) need tag tracking - keep boxed for those.
        VarKind k = typeExprToKind(otherSide);
        bool isPtrShaped =
            k == VarKind::Str        || k == VarKind::StrLiteral  ||
            k == VarKind::List       || k == VarKind::Dict        ||
            k == VarKind::Tuple      || k == VarKind::Set         ||
            k == VarKind::ClassInstance;
        // Raw `ptr` (FFI) can also be nullable - extern returns of NULL are
        // already used this way in the runtime layer.
        if (auto* nm = dynamic_cast<NamedTypeExpr*>(otherSide))
            if (nm->name == "ptr") isPtrShaped = true;
        // Callable[[...], R] is a function pointer - also pointer-shaped.
        if (dynamic_cast<CallableTypeExpr*>(otherSide)) isPtrShaped = true;
        return isPtrShaped ? otherSide : nullptr;
    }

llvm::Value* CodeGen::Impl::boxPayloadAsKind(llvm::Value* box, VarKind k) {
        llvm::Value* p = boxPayloadI64(box);
        switch (k) {
            case VarKind::Float: return builder->CreateBitCast(p, f64Type, "p.f");
            case VarKind::Bool:  return builder->CreateICmpNE(
                p, llvm::ConstantInt::get(i64Type, 0), "p.b");
            case VarKind::Str:
            case VarKind::StrLiteral:
            case VarKind::List:
            case VarKind::Dict:
            case VarKind::Tuple:
            case VarKind::Set:
            case VarKind::ClassInstance:
            case VarKind::Generator:
            case VarKind::Closure:
            case VarKind::File:
                return builder->CreateIntToPtr(p, i8PtrType, "p.p");
            default:  // Int / Type / Other
                return p;
        }
    }

llvm::Value* CodeGen::Impl::unboxBoxResultChecked(llvm::Value* box, llvm::Type* targetType,
                                   VarKind vk, int64_t wantListElemTag) {
        if (targetType == boxType) return box;
        int64_t expectedTag = -1;
        const char* tagName = "value";
        if (targetType == i64Type)        { expectedTag = 0; tagName = "int"; }
        else if (targetType == f64Type)   { expectedTag = 2; tagName = "float"; }
        else if (targetType == i1Type)    { expectedTag = 3; tagName = "bool"; }
        else if (targetType == i8PtrType) {
            switch (vk) {
                case VarKind::Str: case VarKind::StrLiteral:
                    expectedTag = 1; tagName = "str"; break;
                case VarKind::List: expectedTag = 5; tagName = "list"; break;
                case VarKind::Dict: expectedTag = 6; tagName = "dict"; break;
                case VarKind::ClassInstance: expectedTag = 7; tagName = "class"; break;
                default: break;
            }
        }
        if (expectedTag < 0) return box;  // unknown target - caller stores box
        auto* func = currentFunction;
        auto* tagV = boxTag(box, "ub.tag");
        auto* match = builder->CreateICmpEQ(
            tagV, llvm::ConstantInt::get(i64Type, expectedTag), "ub.match");
        auto* okBB = llvm::BasicBlock::Create(*context, "ub.ok", func);
        auto* failBB = llvm::BasicBlock::Create(*context, "ub.fail", func);
        builder->CreateCondBr(match, okBB, failBB);
        builder->SetInsertPoint(failBB);
        std::string msg = std::string("TypeError: expected ") + tagName +
                          " but got value with different runtime type";
        builder->CreateCall(runtimeFuncs["dragon_raise_exc_cstr"],
            {llvm::ConstantInt::get(i64Type, 80),
             builder->CreateGlobalString(msg)});
        builder->CreateUnreachable();
        builder->SetInsertPoint(okBB);
        llvm::Value* out = boxPayloadAsKind(box, vk);
        // The list tag (5) can't tell a monomorphized DragonList from a DragonListBox;
        // verify the payload header matches the target's element rep first.
        if (expectedTag == 5 && wantListElemTag != kNoListElemCheck)
            builder->CreateCall(runtimeFuncs["dragon_list_view_check"],
                {out, llvm::ConstantInt::get(i64Type, wantListElemTag)});
        return out;
    }

int64_t CodeGen::Impl::listViewWantElemTag(TypeExpr* ann) {
        auto* g = dynamic_cast<GenericTypeExpr*>(ann);
        if (!g || g->typeArgs.size() != 1) return kNoListElemCheck;
        auto* base = dynamic_cast<NamedTypeExpr*>(g->base.get());
        if (!base || (base->name != "list" && base->name != "List"))
            return kNoListElemCheck;
        // `list[type]` holds native i64 class-descriptor handles - not a
        // checkable runtime list shape.
        if (auto* n = dynamic_cast<NamedTypeExpr*>(g->typeArgs[0].get()))
            if (n->name == "type") return kNoListElemCheck;
        if (dynamic_cast<UnionTypeExpr*>(g->typeArgs[0].get()))
            return -1;  // list[A | B] uses the box representation
        Type::Kind k = typeExprToTypeKind(g->typeArgs[0].get());
        switch (k) {
            case Type::Kind::Any:
            case Type::Kind::Union:
            case Type::Kind::Optional:
                return -1;  // box representation
            case Type::Kind::Int:
            case Type::Kind::Str:
            case Type::Kind::Bool:
            case Type::Kind::Float:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Bytes:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
            case Type::Kind::Function:
                return typeKindToElemTag(k);
            default:
                return kNoListElemCheck;
        }
    }

std::pair<llvm::Value*, llvm::Value*> CodeGen::Impl::boxArgTagPayload(
        Expr* argExpr, llvm::Value* val, bool takesOwnership) {
        llvm::Value* tagV;
        llvm::Value* payloadV;
        if (val->getType() == boxType) {
            tagV = boxTag(val, "tag");
            payloadV = boxPayloadI64(val, "payload");
            if (takesOwnership && options.gcMode == GCMode::RC)
                emitUnionIncref(payloadV, tagV);
        } else {
            tagV = emitTagForExprNoCG(argExpr);
            if (val->getType()->isPointerTy()) {
                int64_t litTag = -1;
                if (auto* cT = llvm::dyn_cast<llvm::ConstantInt>(tagV))
                    litTag = cT->getSExtValue();
                // Heap-dup a str literal only when the consumer adopts it; a
                // borrow-only box keeps the rodata pointer (a dup would leak).
                if (litTag == 1 && takesOwnership)
                    val = ensureHeapString(val, argExpr);
                if (takesOwnership && options.gcMode == GCMode::RC &&
                    isBorrowedHeapExpr(argExpr)) {
                    if (litTag == 1)
                        builder->CreateCall(
                            runtimeFuncs["dragon_incref_str"], {val});
                    else if (litTag == 5 || litTag == 6 || litTag == 7)
                        builder->CreateCall(
                            runtimeFuncs["dragon_incref"], {val});
                }
            }
            payloadV = nativeToPayloadI64(val);
            // Niche Optional (X | None): pick the tag at runtime - TAG_NONE when the
            // ptr is null, else X's tag (the static derivation can't see null-ness).
            if (argExpr && argExpr->type &&
                argExpr->type->kind() == Type::Kind::Union &&
                val->getType()->isPointerTy()) {
                auto& u = static_cast<UnionType&>(*argExpr->type);
                if (u.types.size() == 2) {
                    Type* inner = nullptr;
                    bool hasNone = false;
                    for (auto& t : u.types) {
                        if (t->kind() == Type::Kind::None_) hasNone = true;
                        else inner = t.get();
                    }
                    int64_t innerTag = inner ? typeKindToTag(inner->kind()) : -1;
                    if (hasNone && innerTag >= 0) {
                        auto* nullp = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(val->getType()));
                        auto* isNull = builder->CreateICmpEQ(val, nullp, "opt.isnull");
                        tagV = builder->CreateSelect(isNull,
                            llvm::ConstantInt::get(i64Type, 4),   // TAG_NONE
                            llvm::ConstantInt::get(i64Type, innerTag), "opt.tag");
                    }
                }
            }
        }
        return {tagV, payloadV};
    }

llvm::Value* CodeGen::Impl::emitTagForExpr(Expr* expr, CodeGen& cg) {
        // The static type is the source of truth for tag derivation when the
        // TypeChecker propagated it; the literal/name fallbacks below cover the rest.
        if (expr && expr->type) {
            int64_t tag = typeKindToTag(expr->type->kind());
            if (tag >= 0)
                return llvm::ConstantInt::get(i64Type, tag);
        }
        // Literals -> constant tag (fallback when type wasn't propagated).
        if (dynamic_cast<IntegerLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 0); // TAG_INT
        if (auto* sl = dynamic_cast<StringLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, sl->isBytes ? 7 : 1); // TAG_BYTES / TAG_STR
        if (dynamic_cast<FloatLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 2); // TAG_FLOAT
        if (dynamic_cast<BooleanLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 3); // TAG_BOOL
        if (dynamic_cast<NoneLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 4); // TAG_NONE
        // Class constructor call: `Foo(...)` -> TAG_CLASS (7).
        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            if (auto* nm = dynamic_cast<NameExpr*>(call->callee.get())) {
                if (classNames.count(nm->name))
                    return llvm::ConstantInt::get(i64Type, 7); // TAG_CLASS
            }
        }
        // Named variable -> check VarKind
        if (auto* nameExpr = dynamic_cast<NameExpr*>(expr)) {
            VarKind vk = lookupVarKind(nameExpr->name);
            if (vk == VarKind::Union) {
                // D030 Phase 4: extract tag from the box load.
                auto* alloca = lookupVar(nameExpr->name);
                if (alloca) {
                    auto* box = builder->CreateLoad(boxType, alloca, nameExpr->name + ".box");
                    return boxTag(box, nameExpr->name + ".tag");
                }
            }
            int64_t tag = varKindToTag(vk);
            if (tag >= 0)
                return llvm::ConstantInt::get(i64Type, tag);
        }
        // Fallback: assume int (0)
        return llvm::ConstantInt::get(i64Type, 0);
    }

llvm::Type* CodeGen::Impl::typeExprToLLVM(TypeExpr* typeExpr) {
        if (!typeExpr) return i64Type; // default to int
        if (auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr)) {
            if (named->name == "int") return i64Type;
            if (named->name == "intc") return intcType;  // C int for FFI
            if (named->name == "float") return f64Type;
            if (named->name == "bool") return i1Type;
            if (named->name == "str") return i8PtrType;
            if (named->name == "bytes") return i8PtrType;
            if (named->name == "type") return i64Type;  // class descriptor (i64-tagged ptr)
            if (named->name == "None") return voidType;
            // D039 Phase 1: Any is the maximally-wide non-niche union -
            // {i64 tag, i64 payload} box, same as `int | str` etc. (line below).
            if (named->name == "Any" || named->name == "object") return boxType;
            if (named->name == "list" || named->name == "List") return i8PtrType;
            if (named->name == "dict" || named->name == "Dict") return i8PtrType;
            if (named->name == "tuple" || named->name == "Tuple") return i8PtrType;
            if (named->name == "set" || named->name == "Set") return i8PtrType;
            if (named->name == "ptr") return i8PtrType;  // Raw C pointer
            if (named->name == "Task") return i8PtrType;  // vthread handle (D016)
            if (named->name == "Lock") return i8PtrType;  // pthread_mutex_t* handle
            // TypedDict classes are dicts (pointers) at runtime
            if (typedDictClasses.count(named->name)) return i8PtrType;
            // Class types are struct pointers (also resolves dotted mod.Foo to the
            // bare struct type).
            if (classStructTypes.count(named->name) || classNames.count(named->name)) return i8PtrType;
            if (!resolveAnnotationClassName(named->name).empty()) return i8PtrType;
            return i64Type; // fallback
        }
        if (dynamic_cast<GenericTypeExpr*>(typeExpr)) {
            // list[int], dict[str,int], etc. are all pointer types
            return i8PtrType;
        }
        if (dynamic_cast<UnionTypeExpr*>(typeExpr)) {
            // Niche-pointer: `Ptr | None` is a single nullable pointer (null = None),
            // no box.
            if (TypeExpr* niche = unionNicheMember(typeExpr))
                return typeExprToLLVM(niche);
            // D030 Phase 4: non-niche unions (e.g. `int | str`) still use the
            // {i64 tag, i64 payload} box.
            return boxType;
        }
        if (dynamic_cast<CallableTypeExpr*>(typeExpr)) {
            // Callable[[A, B], R] is a bare function pointer at runtime.
            return i8PtrType;
        }
        return i64Type; // fallback
    }

llvm::Value* CodeGen::Impl::emitTagForExprNoCG(Expr* expr) {
        if (expr && expr->type) {
            int64_t tag = typeKindToTag(expr->type->kind());
            if (tag >= 0)
                return llvm::ConstantInt::get(i64Type, tag);
        }
        if (dynamic_cast<IntegerLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 0);
        if (auto* sl = dynamic_cast<StringLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, sl->isBytes ? 7 : 1);
        if (dynamic_cast<FloatLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 2);
        if (dynamic_cast<BooleanLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 3);
        if (dynamic_cast<NoneLiteral*>(expr))
            return llvm::ConstantInt::get(i64Type, 4);
        if (dynamic_cast<ListExpr*>(expr) || dynamic_cast<ListCompExpr*>(expr))
            return llvm::ConstantInt::get(i64Type, 5);
        if (dynamic_cast<DictExpr*>(expr) || dynamic_cast<DictCompExpr*>(expr))
            return llvm::ConstantInt::get(i64Type, 6);
        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            if (auto* nm = dynamic_cast<NameExpr*>(call->callee.get())) {
                if (classNames.count(nm->name))
                    return llvm::ConstantInt::get(i64Type, 7);
            }
        }
        if (auto* nameExpr = dynamic_cast<NameExpr*>(expr)) {
            VarKind vk = lookupVarKind(nameExpr->name);
            if (vk == VarKind::Union) {
                auto* alloca = lookupVar(nameExpr->name);
                if (alloca) {
                    auto* box = builder->CreateLoad(boxType, alloca, nameExpr->name + ".box");
                    return boxTag(box, nameExpr->name + ".tag");
                }
            }
            int64_t tag = varKindToTag(vk);
            if (tag >= 0)
                return llvm::ConstantInt::get(i64Type, tag);
        }
        return llvm::ConstantInt::get(i64Type, 0);
    }

llvm::Value* CodeGen::Impl::coerceArg(llvm::Value* arg, llvm::Type* paramType) {
        if (arg->getType() == paramType) return arg;
        llvm::Type* at = arg->getType();
        // A box flowing into a native param: unbox at the param's shape (else the
        // verifier rejects a box where a ptr/scalar is wanted). Payload crosses borrowed.
        if (at == boxType && paramType != boxType) {
            VarKind vk = paramType == f64Type       ? VarKind::Float :
                         paramType == i1Type        ? VarKind::Bool  :
                         paramType->isPointerTy()   ? VarKind::Str   :
                                                      VarKind::Int;
            return boxPayloadAsKind(arg, vk);
        }
        // int->float
        if (paramType == f64Type && at == i64Type)
            return builder->CreateSIToFP(arg, f64Type);
        // bool->int
        if (paramType == i64Type && at == i1Type)
            return builder->CreateZExt(arg, i64Type);
        // int->bool (truncate: 0->false, nonzero->true)
        if (paramType == i1Type && at == i64Type)
            return builder->CreateICmpNE(arg, llvm::ConstantInt::get(i64Type, 0));
        // bool->float
        if (paramType == f64Type && at == i1Type)
            return builder->CreateUIToFP(arg, f64Type);
        // int->ptr (load-bearing for class fields holding ptr-returning RHS values)
        if (paramType->isPointerTy() && at == i64Type)
            return builder->CreateIntToPtr(arg, paramType);
        // ptr->int (load-bearing for the inverse direction)
        if (paramType == i64Type && at->isPointerTy())
            return builder->CreatePtrToInt(arg, i64Type);
        // int->intc (i64->intc truncate)
        if (paramType == intcType && at == i64Type)
            return builder->CreateTrunc(arg, intcType);
        // intc->int (intc->i64 sign-extend)
        if (paramType == i64Type && at == intcType)
            return builder->CreateSExt(arg, i64Type);
        // bool->intc
        if (paramType == intcType && at == i1Type)
            return builder->CreateZExt(arg, intcType);
        // intc->float
        if (paramType == f64Type && at == intcType)
            return builder->CreateSIToFP(arg, f64Type);
        // intc->ptr - FFI: C int treated as opaque pointer payload
        if (paramType->isPointerTy() && at == intcType)
            return builder->CreateIntToPtr(builder->CreateSExt(arg, i64Type), paramType);
        // ptr->intc - FFI: opaque pointer truncated to C int
        if (paramType == intcType && at->isPointerTy())
            return builder->CreateTrunc(builder->CreatePtrToInt(arg, i64Type), intcType);
        return arg;
    }

llvm::Value* CodeGen::Impl::taskResultFromI64(llvm::Value* rawI64, Type* resultType) {
        if (!resultType) return rawI64;
        switch (resultType->kind()) {
            case Type::Kind::Float:
                return builder->CreateBitCast(rawI64, f64Type, "task.res.f64");
            case Type::Kind::Bool:
                return builder->CreateICmpNE(
                    rawI64, llvm::ConstantInt::get(i64Type, 0), "task.res.bool");
            case Type::Kind::Str:
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
            case Type::Kind::None_:
            case Type::Kind::Ptr:
                return builder->CreateIntToPtr(rawI64, i8PtrType, "task.res.ptr");
            default:
                return rawI64;  // Int / Unknown / Any / Union -> i64 slot as-is
        }
    }

llvm::Function* CodeGen::Impl::buildFireTrampoline(
    llvm::Function* targetFn,
    llvm::StructType* argsStructType,
    const std::vector<VarKind>& argKinds,
    const std::string& siteName) {
        // Build trampoline function: void (mco_coro*)
        auto* trampType = llvm::FunctionType::get(voidType, {i8PtrType}, false);
        auto* tramp = llvm::Function::Create(
            trampType, llvm::Function::InternalLinkage,
            "__dragon_fire_tramp_" + siteName, module.get());

        auto* prevFunc = currentFunction;
        auto* prevBlock = builder->GetInsertBlock();
        currentFunction = tramp;

        auto* entry = llvm::BasicBlock::Create(*context, "entry", tramp);
        builder->SetInsertPoint(entry);

        llvm::Value* coArg = &*tramp->arg_begin();
        coArg->setName("co");
        // user_data = malloced args buffer
        auto* udRaw = builder->CreateCall(
            runtimeFuncs["mco_get_user_data"], {coArg}, "args.raw");
        auto* ud = builder->CreateBitCast(
            udRaw, llvm::PointerType::getUnqual(*context), "args.typed");

        // Load vthread handle (field 0)
        auto* vtAddr = builder->CreateStructGEP(argsStructType, ud, 0, "vt.addr");
        auto* vt = builder->CreateLoad(i8PtrType, vtAddr, "vt");

        // Load each user arg (fields 1..N) at native type, call target.
        std::vector<llvm::Value*> callArgs;
        unsigned numUserArgs = argsStructType->getNumElements() - 1;
        for (unsigned i = 0; i < numUserArgs; i++) {
            auto* fieldType = argsStructType->getElementType(i + 1);
            auto* slot = builder->CreateStructGEP(argsStructType, ud, i + 1);
            auto* v = builder->CreateLoad(fieldType, slot);
            callArgs.push_back(v);
        }

        // setjmp barrier: an uncaught raise longjmps here instead of exit(1)
        // (which would kill the worker); log, clear, set result 0, and clean up.
        auto* setRes = runtimeFuncs["dragon_vthread_set_result"];
        auto* jmpbufPtr = builder->CreateCall(
            runtimeFuncs["dragon_exc_push_frame"], {}, "tramp.jmpbuf");
        auto* setjmpRes = builder->CreateCall(
            runtimeFuncs["setjmp"], {jmpbufPtr}, "tramp.setjmp");
        auto* normalBB = llvm::BasicBlock::Create(*context, "tramp.body", tramp);
        auto* caughtBB = llvm::BasicBlock::Create(*context, "tramp.uncaught", tramp);
        auto* cleanupBB = llvm::BasicBlock::Create(*context, "tramp.cleanup", tramp);
        auto* isNormal = builder->CreateICmpEQ(
            setjmpRes,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0),
            "tramp.normal");
        builder->CreateCondBr(isNormal, normalBB, caughtBB);

        // Normal: call target, set result, pop the trampoline's frame.
        builder->SetInsertPoint(normalBB);
        bool returnsVoid = targetFn->getReturnType() == voidType;
        auto* res = builder->CreateCall(
            targetFn, callArgs, returnsVoid ? "" : "fire.res");
        auto* resI64 = resultToI64(res);
        builder->CreateCall(setRes, {vt, resI64});
        builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
        builder->CreateBr(cleanupBB);

        // Uncaught longjmp: free the heap locals it skipped, pop the frame, log +
        // clear the exception, write result 0 for joiners.
        builder->SetInsertPoint(caughtBB);
        builder->CreateCall(runtimeFuncs["dragon_exc_cleanup_unwind"], {});
        builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
        builder->CreateCall(runtimeFuncs["dragon_vthread_log_uncaught"], {});
        builder->CreateCall(setRes, {vt, llvm::ConstantInt::get(i64Type, 0)});
        builder->CreateBr(cleanupBB);

        // Cleanup: balance spawn-site atomic-incref of heap args, free the
        // args buffer, and return to mco_resume.
        builder->SetInsertPoint(cleanupBB);

        // Atomic-decref heap args to balance the spawn-site incref. The
        // trampoline runs on the worker thread, so atomic variants are required.
        for (unsigned i = 0; i < numUserArgs; i++) {
            VarKind k = (i < argKinds.size()) ? argKinds[i] : VarKind::Other;
            if (!isHeapKind(k) || k == VarKind::Union) continue;
            // Reload as ptr (fields 1..N store as their native type already)
            auto* slot = builder->CreateStructGEP(argsStructType, ud, i + 1);
            auto* v = builder->CreateLoad(argsStructType->getElementType(i + 1), slot);
            llvm::Value* p = v->getType()->isPointerTy()
                ? v
                : builder->CreateIntToPtr(v, i8PtrType);
            const char* fn = (k == VarKind::Str)
                ? "dragon_decref_str_atomic"
                : "dragon_decref_atomic";
            builder->CreateCall(runtimeFuncs[fn], {p});
        }

        builder->CreateCall(runtimeFuncs["free"], {udRaw});
        builder->CreateRetVoid();

        currentFunction = prevFunc;
        if (prevBlock) builder->SetInsertPoint(prevBlock);
        return tramp;
    }

llvm::Function* CodeGen::Impl::buildDeferThunk(llvm::Function* targetFn,
                                               const std::string& siteName,
                                               int vtableIndex) {
        // Defer thunk: load each param from the snapshot at native type, call,
        // discard. vtableIndex >= 0 dispatches self through its vtable (D026 override).
        auto* i64PtrTy = llvm::PointerType::getUnqual(*context);
        auto* thunkType = llvm::FunctionType::get(voidType, {i64PtrTy}, false);
        auto* thunk = llvm::Function::Create(
            thunkType, llvm::Function::InternalLinkage,
            "__dragon_defer_" + siteName, module.get());

        auto* prevFunc = currentFunction;
        auto* prevBlock = builder->GetInsertBlock();
        currentFunction = thunk;

        auto* entry = llvm::BasicBlock::Create(*context, "entry", thunk);
        builder->SetInsertPoint(entry);

        llvm::Value* argsPtr = &*thunk->arg_begin();
        argsPtr->setName("args");

        auto* targetTy = targetFn->getFunctionType();
        std::vector<llvm::Value*> callArgs;
        for (unsigned i = 0; i < targetTy->getNumParams(); ++i) {
            auto* slot = builder->CreateConstInBoundsGEP1_64(
                i64Type, argsPtr, i, "defer.slot");
            llvm::Value* raw =
                builder->CreateLoad(i64Type, slot, "defer.raw");
            auto* pty = targetTy->getParamType(i);
            llvm::Value* v = raw;
            if (pty->isPointerTy())
                v = builder->CreateIntToPtr(raw, pty, "defer.ptr");
            else if (pty->isDoubleTy())
                v = builder->CreateBitCast(raw, pty, "defer.f64");
            else if (pty->isIntegerTy() && pty != i64Type)
                v = builder->CreateTrunc(raw, pty, "defer.trunc");
            callArgs.push_back(v);
        }
        if (vtableIndex >= 0 && !callArgs.empty() &&
            callArgs[0]->getType()->isPointerTy()) {
            // Same header shape as CallMethods.cpp D026: {refcount, tag, vt*}.
            auto* headerTy = llvm::StructType::get(
                *context, {i64Type, i64Type, i8PtrType});
            auto* vtSlot =
                builder->CreateStructGEP(headerTy, callArgs[0], 2, "vt_slot");
            auto* vtPtr = builder->CreateLoad(i8PtrType, vtSlot, "vtable");
            auto* vtArrTy = llvm::ArrayType::get(i8PtrType, 0);
            auto* mSlot = builder->CreateGEP(
                vtArrTy, vtPtr,
                {builder->getInt64(0), builder->getInt64((int64_t)vtableIndex)},
                "method_slot");
            auto* methodPtr =
                builder->CreateLoad(i8PtrType, mSlot, "method_ptr");
            builder->CreateCall(targetTy, methodPtr, callArgs);
        } else {
            builder->CreateCall(targetFn, callArgs);
        }
        builder->CreateRetVoid();

        currentFunction = prevFunc;
        if (prevBlock) builder->SetInsertPoint(prevBlock);
        return thunk;
    }

llvm::Function* CodeGen::Impl::buildGeneratorTrampoline(
    llvm::Function* bodyFn,
    llvm::StructType* argsStructType,
    const std::string& siteName) {
        auto* trampType = llvm::FunctionType::get(voidType, {i8PtrType}, false);
        auto* tramp = llvm::Function::Create(
            trampType, llvm::Function::InternalLinkage,
            "__dragon_gen_tramp_" + siteName, module.get());

        auto* prevFunc = currentFunction;
        auto* prevBlock = builder->GetInsertBlock();
        currentFunction = tramp;

        auto* entry = llvm::BasicBlock::Create(*context, "entry", tramp);
        builder->SetInsertPoint(entry);

        llvm::Value* coArg = &*tramp->arg_begin();
        coArg->setName("co");
        auto* udRaw = builder->CreateCall(
            runtimeFuncs["mco_get_user_data"], {coArg}, "args.raw");
        auto* ud = builder->CreateBitCast(
            udRaw, llvm::PointerType::getUnqual(*context), "args.typed");

        // Load the generator ptr (field 0) into an alloca so it survives a longjmp
        // back into the barrier (pre-setjmp SSA values may be clobbered).
        auto* genAddr = builder->CreateStructGEP(argsStructType, ud, 0, "gen.addr");
        auto* gen = builder->CreateLoad(i8PtrType, genAddr, "gen");
        auto* genSlot = builder->CreateAlloca(i8PtrType, nullptr, "gen.slot");
        builder->CreateStore(gen, genSlot);

        // setjmp barrier at the bottom of the generator's exc context: an uncaught
        // body exception unwinds here to re-raise in the caller, not across the coroutine.
        auto* jmpbufPtr = builder->CreateCall(
            runtimeFuncs["dragon_exc_push_frame"], {}, "gen.barrier.jmpbuf");
        auto* setjmpResult = builder->CreateCall(
            runtimeFuncs["setjmp"], {jmpbufPtr}, "gen.barrier.sj");
        auto* isNormal = builder->CreateICmpEQ(
            setjmpResult,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0),
            "gen.barrier.normal");
        auto* normalBB = llvm::BasicBlock::Create(*context, "gen.body.normal", tramp);
        auto* caughtBB = llvm::BasicBlock::Create(*context, "gen.body.caught", tramp);
        builder->CreateCondBr(isNormal, normalBB, caughtBB);

        // Normal path: run the body, pop the barrier, mark exhausted.
        builder->SetInsertPoint(normalBB);
        // Body signature: void body(ptr gen, native_args...)
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(gen);
        unsigned numUserArgs = argsStructType->getNumElements() - 1;
        for (unsigned i = 0; i < numUserArgs; i++) {
            auto* fieldType = argsStructType->getElementType(i + 1);
            auto* slot = builder->CreateStructGEP(argsStructType, ud, i + 1);
            auto* v = builder->CreateLoad(fieldType, slot);
            callArgs.push_back(v);
        }
        builder->CreateCall(bodyFn, callArgs);
        builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
        builder->CreateCall(
            runtimeFuncs["dragon_generator_set_exhausted"], {gen});
        builder->CreateRetVoid();

        // Caught path: an uncaught body exception unwound here. Pop the frame,
        // flag the pending exception, mark exhausted, return.
        builder->SetInsertPoint(caughtBB);
        auto* genReload = builder->CreateLoad(i8PtrType, genSlot, "gen.reload");
        builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
        builder->CreateCall(
            runtimeFuncs["dragon_generator_set_raised"], {genReload});
        builder->CreateCall(
            runtimeFuncs["dragon_generator_set_exhausted"], {genReload});
        builder->CreateRetVoid();

        currentFunction = prevFunc;
        if (prevBlock) builder->SetInsertPoint(prevBlock);
        return tramp;
    }

llvm::Function* CodeGen::Impl::buildGeneratorDecrefFn(
    llvm::StructType* argsStructType,
    const std::vector<VarKind>& argKinds,
    const std::string& siteName) {
        // Skip if no heap args
        bool anyHeap = false;
        for (auto k : argKinds) {
            if (isHeapKind(k) && k != VarKind::Union) { anyHeap = true; break; }
        }
        if (!anyHeap) return nullptr;

        auto* fnType = llvm::FunctionType::get(voidType, {i8PtrType}, false);
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::InternalLinkage,
            "__dragon_gen_decref_" + siteName, module.get());

        auto* prevFunc = currentFunction;
        auto* prevBlock = builder->GetInsertBlock();
        currentFunction = fn;

        auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
        builder->SetInsertPoint(entry);

        llvm::Value* udRaw = &*fn->arg_begin();
        udRaw->setName("args");
        auto* ud = builder->CreateBitCast(
            udRaw, llvm::PointerType::getUnqual(*context), "args.typed");

        unsigned numUserArgs = argsStructType->getNumElements() - 1;
        for (unsigned i = 0; i < numUserArgs && i < argKinds.size(); i++) {
            VarKind k = argKinds[i];
            if (!isHeapKind(k) || k == VarKind::Union) continue;
            auto* slot = builder->CreateStructGEP(argsStructType, ud, i + 1);
            auto* v = builder->CreateLoad(argsStructType->getElementType(i + 1), slot);
            llvm::Value* p = v->getType()->isPointerTy()
                ? v
                : builder->CreateIntToPtr(v, i8PtrType);
            const char* fname = (k == VarKind::Str)
                ? "dragon_decref_str_atomic"
                : "dragon_decref_atomic";
            builder->CreateCall(runtimeFuncs[fname], {p});
        }

        builder->CreateRetVoid();

        currentFunction = prevFunc;
        if (prevBlock) builder->SetInsertPoint(prevBlock);
        return fn;
    }

void CodeGen::Impl::populateSpawnArgs(
    llvm::Value* argsAlloca,
    llvm::StructType* argsStructType,
    const std::vector<llvm::Value*>& userArgs) {
        // Field 0 zero-init: alloca is calloc'd? No - alloca is uninitialized.
        // Explicitly store null so the runtime sees a clean field-0 value.
        auto* f0 = builder->CreateStructGEP(argsStructType, argsAlloca, 0);
        builder->CreateStore(
            llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(i8PtrType)),
            f0);
        for (size_t i = 0; i < userArgs.size(); i++) {
            auto* fieldType = argsStructType->getElementType((unsigned)(i + 1));
            llvm::Value* v = coerceToFieldType(userArgs[i], fieldType);
            auto* slot = builder->CreateStructGEP(
                argsStructType, argsAlloca, (unsigned)(i + 1));
            builder->CreateStore(v, slot);
        }
    }

llvm::AllocaInst* CodeGen::Impl::bindListElemTyped(
    llvm::Function* func,
    llvm::Value* listVal,
    llvm::Value* idx,
    const std::string& varName,
    VarKind loopKind) {
        llvm::Value* val;
        llvm::Type* allocaType;
        if (loopKind == VarKind::Float) {
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_get_f64"],
                {listVal, idx}, varName + ".f");
            allocaType = f64Type;
        } else if (loopKind == VarKind::Str ||
                   loopKind == VarKind::List ||
                   loopKind == VarKind::Dict ||
                   loopKind == VarKind::Tuple ||
                   loopKind == VarKind::Set ||
                   loopKind == VarKind::ClassInstance) {
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_get_ptr"],
                {listVal, idx}, varName + ".p");
            allocaType = i8PtrType;
        } else if (loopKind == VarKind::Bool) {
            auto* i64Val = builder->CreateCall(
                runtimeFuncs["dragon_list_get"], {listVal, idx}, varName);
            val = builder->CreateICmpNE(
                i64Val, llvm::ConstantInt::get(i64Type, 0), varName + ".b");
            allocaType = i1Type;
        } else {
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_get"], {listVal, idx}, varName);
            allocaType = i64Type;
        }
        auto* alloca = createEntryAlloca(func, varName, allocaType);
        builder->CreateStore(val, alloca);
        return alloca;
    }

llvm::AllocaInst* CodeGen::Impl::bindListElemByTypeKind(
    llvm::Function* func,
    llvm::Value* listVal,
    llvm::Value* idx,
    const std::string& varName,
    Type::Kind elemKind) {
        llvm::Value* val;
        llvm::Type* allocaType = typeKindToLLVM(elemKind);
        if (elemKind == Type::Kind::Any) {
            // list[Any] iteration: load each element as the full {tag,payload} box so
            // the loop body sees the right runtime type.
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_box_get"],
                {listVal, idx}, varName + ".box");
        } else if (elemKind == Type::Kind::Float) {
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_get_f64"],
                {listVal, idx}, varName + ".f");
        } else if (allocaType == i8PtrType) {
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_get_ptr"],
                {listVal, idx}, varName + ".p");
        } else if (elemKind == Type::Kind::Bool) {
            auto* i64Val = builder->CreateCall(
                runtimeFuncs["dragon_list_get"], {listVal, idx}, varName);
            val = builder->CreateICmpNE(
                i64Val, llvm::ConstantInt::get(i64Type, 0), varName + ".b");
        } else {
            val = builder->CreateCall(
                runtimeFuncs["dragon_list_get"], {listVal, idx}, varName);
        }
        auto* alloca = createEntryAlloca(func, varName, allocaType);
        builder->CreateStore(val, alloca);
        return alloca;
    }

llvm::Value* CodeGen::Impl::emitStringLiteralBytes(const std::string& bytes,
                                    const llvm::Twine& twine) {
        bool hasNonAscii = false;
        for (unsigned char c : bytes) {
            if (c >= 0x80) { hasNonAscii = true; break; }
        }
        if (!hasNonAscii) {
            // Emit the literal as an IMMORTAL DragonString constant (real header) so
            // is_heap_string can't OOB-misread the 32 bytes before a bare C global.
            auto& ctx = builder->getContext();
            auto* i8Ty  = llvm::Type::getInt8Ty(ctx);
            auto* i16Ty = llvm::Type::getInt16Ty(ctx);
            auto* i32Ty = llvm::Type::getInt32Ty(ctx);
            auto* i64Ty = llvm::Type::getInt64Ty(ctx);
            const int64_t n = (int64_t)bytes.size();
            auto* padTy  = llvm::ArrayType::get(i8Ty, 3);
            auto* dataTy = llvm::ArrayType::get(i8Ty, n + 1);  // bytes + NUL
            // Layout MUST mirror DragonString (runtime_internal.h): header(16) +
            // len@16 + kind@24 + _pad[3] + cap@28 + data@32.
            auto* strTy = llvm::StructType::get(ctx, {
                i64Ty, i8Ty, i8Ty, i16Ty, i32Ty,   // DragonObjectHeader
                i64Ty, i8Ty, padTy, i32Ty,         // len, kind, _pad, cap
                dataTy                             // data[]
            }, /*isPacked=*/false);

            auto it = asciiLiteralGlobals.find(bytes);
            llvm::GlobalVariable* gv;
            if (it != asciiLiteralGlobals.end()) {
                gv = it->second;
            } else {
                const int64_t IMMORTAL = (int64_t)0x4000000000000000LL;
                auto* init = llvm::ConstantStruct::get(strTy, {
                    llvm::ConstantInt::get(i64Ty, IMMORTAL),   // refcount (immortal)
                    llvm::ConstantInt::get(i8Ty, 1),           // type_tag = DRAGON_TAG_STR
                    llvm::ConstantInt::get(i8Ty, 0x80),        // gc_flags = GC_FLAG_HEAP_OBJ
                    llvm::ConstantInt::get(i16Ty, 0),          // class_id
                    llvm::ConstantInt::get(i32Ty, -1),         // gc_track_idx
                    llvm::ConstantInt::get(i64Ty, n),          // len (ASCII: cp == byte count)
                    llvm::ConstantInt::get(i8Ty, 1),           // kind = 1
                    llvm::ConstantAggregateZero::get(padTy),   // _pad
                    llvm::ConstantInt::get(i32Ty, (int32_t)n), // cap
                    llvm::ConstantDataArray::getString(
                        ctx, llvm::StringRef(bytes.data(), bytes.size()), /*AddNull=*/true)
                });
                std::string name = "dragon.str.lit." +
                                   std::to_string(asciiLiteralGlobals.size());
                gv = new llvm::GlobalVariable(*module, strTy, /*isConstant=*/false,
                                              llvm::GlobalVariable::PrivateLinkage, init, name);
                gv->setAlignment(llvm::Align(8));
                asciiLiteralGlobals[bytes] = gv;
            }
            // Pointer to the `data` field (struct element 9, byte 0) - constant.
            llvm::Constant* idx[] = {
                llvm::ConstantInt::get(i32Ty, 0),
                llvm::ConstantInt::get(i32Ty, 9),
                llvm::ConstantInt::get(i64Ty, 0),
            };
            return llvm::ConstantExpr::getInBoundsGetElementPtr(strTy, gv, idx);
        }
        auto it = utf8LiteralGlobals.find(bytes);
        if (it == utf8LiteralGlobals.end()) {
            std::string name = "dragon.str.utf8.lit." +
                               std::to_string(utf8LiteralOrder.size());
            auto* gv = new llvm::GlobalVariable(
                *module, i8PtrType, /*isConstant=*/false,
                llvm::GlobalVariable::InternalLinkage,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(i8PtrType)),
                name);
            utf8LiteralGlobals[bytes] = gv;
            utf8LiteralOrder.push_back(bytes);
            it = utf8LiteralGlobals.find(bytes);
        }
        return builder->CreateLoad(i8PtrType, it->second,
                                   twine.isTriviallyEmpty() ? "utf8lit" : twine);
    }

std::string CodeGen::Impl::processEscapes(const std::string& raw, bool isRaw) {
        if (isRaw) return raw;
        std::string result;
        result.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char next = raw[i + 1];
                switch (next) {
                    case 'n': result += '\n'; i++; break;
                    case 't': result += '\t'; i++; break;
                    case 'r': result += '\r'; i++; break;
                    case '\\': result += '\\'; i++; break;
                    case '\'': result += '\''; i++; break;
                    case '"': result += '"'; i++; break;
                    case '0': result += '\0'; i++; break;
                    case 'a': result += '\a'; i++; break;
                    case 'b': result += '\b'; i++; break;
                    case 'f': result += '\f'; i++; break;
                    case 'v': result += '\v'; i++; break;
                    case 'x': {
                        if (i + 3 < raw.size()) {
                            char h1 = raw[i + 2], h2 = raw[i + 3];
                            auto hexval = [](char c) -> int {
                                if (c >= '0' && c <= '9') return c - '0';
                                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                                return -1;
                            };
                            int v1 = hexval(h1), v2 = hexval(h2);
                            if (v1 >= 0 && v2 >= 0) {
                                result += (char)((v1 << 4) | v2);
                                i += 3;
                            } else {
                                result += raw[i];
                            }
                        } else {
                            result += raw[i];
                        }
                        break;
                    }
                    default: result += raw[i]; result += next; i++; break;
                }
            } else {
                result += raw[i];
            }
        }
        return result;
    }

llvm::Function* CodeGen::Impl::getOrDeclareRuntime(const std::string& name,
                                     llvm::FunctionType* funcType) {
        auto it = runtimeFuncs.find(name);
        if (it != runtimeFuncs.end()) return it->second;
        // A user `extern "C" def` registers the symbol directly (not in runtimeFuncs);
        // adopt the existing function so we don't create a `name.1` uniquified dup.
        if (auto* existing = module->getFunction(name)) {
            runtimeFuncs[name] = existing;
            return existing;
        }
        auto* func = llvm::Function::Create(
            funcType, llvm::Function::ExternalLinkage, name, module.get());
        runtimeFuncs[name] = func;
        return func;
    }

void CodeGen::Impl::runOptimizationPasses() {
        if (options.optimizationLevel == 0) return;

        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;

        llvm::PassBuilder PB;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        llvm::OptimizationLevel optLevel;
        switch (options.optimizationLevel) {
            case 1: optLevel = llvm::OptimizationLevel::O1; break;
            case 2: optLevel = llvm::OptimizationLevel::O2; break;
            case 3: optLevel = llvm::OptimizationLevel::O3; break;
            default: optLevel = llvm::OptimizationLevel::O1; break;
        }

        llvm::ModulePassManager MPM =
            PB.buildPerModuleDefaultPipeline(optLevel);
        MPM.run(*module, MAM);

        // Hot-loop alignment: tag every loop llvm.loop.align=32 after the pipeline so
        // a small body never straddles a 64B L1i line (~12% on fib). DRAGON_LOOP_ALIGN overrides.
        unsigned loopAlign = 32;
        if (const char* e = std::getenv("DRAGON_LOOP_ALIGN"))
            loopAlign = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
        if (options.optimizationLevel >= 2 && loopAlign > 1) {
            for (llvm::Function& F : *module) {
                if (F.isDeclaration()) continue;
                llvm::DominatorTree DT(F);
                llvm::LoopInfo LI(DT);
                for (llvm::Loop* L : LI.getLoopsInPreorder())
                    llvm::addStringMetadataToLoop(L, "llvm.loop.align",
                                                  loopAlign);
            }
        }
    }

llvm::Type* CodeGen::Impl::inferExprLLVMType(Expr* expr) {
        if (!expr) return i64Type;
        if (dynamic_cast<IntegerLiteral*>(expr)) return i64Type;
        if (dynamic_cast<FloatLiteral*>(expr)) return f64Type;
        if (dynamic_cast<StringLiteral*>(expr)) return i8PtrType;
        if (dynamic_cast<BooleanLiteral*>(expr)) return i1Type;
        if (dynamic_cast<NoneLiteral*>(expr)) return i8PtrType;

        if (auto* name = dynamic_cast<NameExpr*>(expr)) {
            auto* alloca = lookupVar(name->name);
            if (alloca) return alloca->getAllocatedType();
            return i64Type;
        }

        if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
            auto op = bin->op.type();
            if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL ||
                op == TokenType::LESS || op == TokenType::LESS_EQUAL ||
                op == TokenType::GREATER || op == TokenType::GREATER_EQUAL ||
                op == TokenType::AND || op == TokenType::OR) {
                return i1Type;
            }
            if (op == TokenType::SLASH) return f64Type;
            auto lt = inferExprLLVMType(bin->left.get());
            auto rt = inferExprLLVMType(bin->right.get());
            if (lt == f64Type || rt == f64Type) return f64Type;
            if (lt == i8PtrType && rt == i8PtrType && op == TokenType::PLUS)
                return i8PtrType;
            return i64Type;
        }

        if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
            if (unary->op.type() == TokenType::NOT) return i1Type;
            return inferExprLLVMType(unary->operand.get());
        }

        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            if (auto* callee = dynamic_cast<NameExpr*>(call->callee.get())) {
                if (callee->name == "len" || callee->name == "int" ||
                    callee->name == "abs" || callee->name == "ord")
                    return i64Type;
                if (callee->name == "float") return f64Type;
                if (callee->name == "str" || callee->name == "input" ||
                    callee->name == "chr" || callee->name == "repr")
                    return i8PtrType;
                if (callee->name == "bool" || callee->name == "isinstance")
                    return i1Type;
                // User-defined function
                auto* func = module->getFunction(callee->name);
                if (func) return func->getReturnType();
            }
            return i64Type;
        }

        if (auto* ifExpr = dynamic_cast<IfExpr*>(expr)) {
            return inferExprLLVMType(ifExpr->thenExpr.get());
        }

        return i64Type;
    }

} // namespace dragon
