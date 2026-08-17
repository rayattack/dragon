#include "../CodeGenImpl.h"

namespace dragon {

Type::Kind CodeGen::Impl::elemVarKindToTypeKind(VarKind ek) {
        switch (ek) {
            case VarKind::Str:           return Type::Kind::Str;
            case VarKind::Float:         return Type::Kind::Float;
            case VarKind::Bool:          return Type::Kind::Bool;
            case VarKind::List:          return Type::Kind::List;
            case VarKind::Dict:          return Type::Kind::Dict;
            case VarKind::Tuple:         return Type::Kind::Tuple;
            case VarKind::Set:           return Type::Kind::Set;
            case VarKind::ClassInstance: return Type::Kind::Instance;
            case VarKind::Closure:       return Type::Kind::Function;
            case VarKind::Union:         return Type::Kind::Any;
            default:                     return Type::Kind::Int;
        }
    }

void CodeGen::Impl::trackPtrParam(const std::string& paramName, TypeExpr* typeExpr) {
        if (auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr)) {
            if (named->name == "ptr") varIsPtrCallable.insert(paramName);
            return;
        }
        if (auto* callable = dynamic_cast<CallableTypeExpr*>(typeExpr)) {
            callableTypes[paramName] = callableTypeExprToFnType(callable);
            varIsPtrCallable.insert(paramName);
            return;
        }
        if (auto* generic = dynamic_cast<GenericTypeExpr*>(typeExpr)) {
            auto* base = dynamic_cast<NamedTypeExpr*>(generic->base.get());
            if (!base) return;
            if ((base->name == "list" || base->name == "List") &&
                !generic->typeArgs.empty()) {
                TypeExpr* elemTy = generic->typeArgs[0].get();
                VarKind ek = typeExprToKind(elemTy);
                varListElemKinds[paramName] = elemVarKindToTypeKind(ek);
                if (ek == VarKind::Type)
                    varListElemIsType.insert(paramName);
                if (auto* nt = dynamic_cast<NamedTypeExpr*>(elemTy)) {
                    if (classFieldKindsBySym.count(classSym(nt->name)) ||
                        classNames.count(nt->name))
                        varListElemClassName[paramName] = nt->name;
                }
                if (auto* cte = dynamic_cast<CallableTypeExpr*>(elemTy)) {
                    varListElemCallableType[paramName] =
                        callableTypeExprToFnType(cte);
                }
            } else if ((base->name == "dict" || base->name == "Dict") &&
                       generic->typeArgs.size() == 2) {
                TypeExpr* keyTy = generic->typeArgs[0].get();
                TypeExpr* valTy = generic->typeArgs[1].get();
                VarKind kk = typeExprToKind(keyTy);
                VarKind vk = typeExprToKind(valTy);
                varDictKeyKinds[paramName] = elemVarKindToTypeKind(kk);
                varDictValueKinds[paramName] = elemVarKindToTypeKind(vk);
                if (vk == VarKind::Type)
                    varDictValueIsType.insert(paramName);
            }
            return;
        }
        if (TypeExpr* niche = unionNicheMember(typeExpr)) {
            if (auto* callable = dynamic_cast<CallableTypeExpr*>(niche)) {
                callableTypes[paramName] = callableTypeExprToFnType(callable);
                varIsPtrCallable.insert(paramName);
            }
        }
    }

int64_t CodeGen::Impl::excTypeCode(const std::string& name) {
        if (name == "BaseException")          return 0;
        if (name == "SystemExit")             return 1;
        if (name == "KeyboardInterrupt")      return 2;
        if (name == "GeneratorExit")          return 3;
        if (name == "Exception")              return 10;
        if (name == "StopIteration")          return 11;
        if (name == "ArithmeticError")        return 20;
        if (name == "FloatingPointError")     return 21;
        if (name == "OverflowError")          return 22;
        if (name == "ZeroDivisionError")      return 23;
        if (name == "AssertionError")         return 24;
        if (name == "AttributeError")         return 25;
        if (name == "BufferError")            return 26;
        if (name == "EOFError")               return 27;
        if (name == "ImportError")            return 30;
        if (name == "ModuleNotFoundError")    return 31;
        if (name == "LookupError")            return 40;
        if (name == "IndexError")             return 41;
        if (name == "KeyError")               return 42;
        if (name == "MemoryError")            return 43;
        if (name == "NameError")              return 44;
        if (name == "UnboundLocalError")      return 45;
        if (name == "OSError")                return 50;
        if (name == "IOError")                return 50;
        if (name == "FileNotFoundError")      return 51;
        if (name == "FileExistsError")        return 52;
        if (name == "IsADirectoryError")      return 53;
        if (name == "NotADirectoryError")     return 54;
        if (name == "PermissionError")        return 55;
        if (name == "TimeoutError")           return 56;
        if (name == "ConnectionError")        return 57;
        if (name == "BrokenPipeError")        return 58;
        if (name == "ConnectionAbortedError") return 59;
        if (name == "ConnectionRefusedError") return 60;
        if (name == "ConnectionResetError")   return 61;
        if (name == "RuntimeError")           return 70;
        if (name == "NotImplementedError")    return 71;
        if (name == "RecursionError")         return 72;
        if (name == "StopAsyncIteration")     return 73;
        if (name == "SyntaxError")            return 74;
        if (name == "TypeError")              return 80;
        if (name == "ValueError")             return 90;
        if (name == "UnicodeError")           return 91;
        if (name == "UnicodeDecodeError")     return 92;
        if (name == "UnicodeEncodeError")     return 93;
        if (name == "UnicodeTranslateError")  return 94;
        if (name == "Warning")                return 100;
        if (name == "DeprecationWarning")     return 101;
        if (name == "FutureWarning")          return 102;
        if (name == "ResourceWarning")        return 103;
        if (name == "RuntimeWarning")         return 104;
        if (name == "UserWarning")            return 105;
        auto uit = userExcCodesBySym.find(classSym(name));
        if (uit != userExcCodesBySym.end()) return uit->second;
        return 10;
    }

bool CodeGen::Impl::isBuiltinExcName(const std::string& name) {
        static const std::unordered_set<std::string> names = {
            "BaseException", "SystemExit", "KeyboardInterrupt", "GeneratorExit",
            "Exception", "StopIteration",
            "ArithmeticError", "FloatingPointError", "OverflowError", "ZeroDivisionError",
            "AssertionError", "AttributeError", "BufferError", "EOFError",
            "ImportError", "ModuleNotFoundError",
            "LookupError", "IndexError", "KeyError",
            "MemoryError", "NameError", "UnboundLocalError",
            "OSError", "IOError", "FileNotFoundError", "FileExistsError", "IsADirectoryError",
            "NotADirectoryError", "PermissionError", "TimeoutError",
            "ConnectionError", "BrokenPipeError", "ConnectionAbortedError",
            "ConnectionRefusedError", "ConnectionResetError",
            "RuntimeError", "NotImplementedError", "RecursionError",
            "StopAsyncIteration", "SyntaxError", "TypeError",
            "ValueError", "UnicodeError", "UnicodeDecodeError",
            "UnicodeEncodeError", "UnicodeTranslateError",
            "Warning", "DeprecationWarning", "FutureWarning",
            "ResourceWarning", "RuntimeWarning", "UserWarning"
        };
        return names.count(name) > 0;
    }

bool CodeGen::Impl::methodIsOverridden(const std::string& baseClass,
                        const std::string& method) const {
        const std::string baseSym = classSym(baseClass);
        for (const auto& [cls, parent] : classParentNamesBySym) {
            std::string c = cls;
            bool descendant = false;
            int guard = 0;
            while (guard++ < 256) {
                auto pit = classParentNamesBySym.find(c);
                if (pit == classParentNamesBySym.end()) break;
                c = pit->second;
                if (c == baseSym) { descendant = true; break; }
            }
            if (!descendant) continue;
            if (module->getFunction(cls + "_" + method))
                return true;
        }
        return false;
    }

llvm::Function* CodeGen::Impl::resolveMethodFunction(
    const std::string& owningModule,
    const std::string& className,
    const std::string& methodName,
    std::string* resolvedSymbol) const {
        std::string leafSym = mangleClass(owningModule, className);
        std::string sym = leafSym + "_" + methodName;
        auto* fn = module->getFunction(sym);
        std::string resolvedLeaf = classSym(className);
        if (!fn && resolvedLeaf != leafSym) {
            std::string s2 = resolvedLeaf + "_" + methodName;
            if (auto* f2 = module->getFunction(s2)) {
                fn = f2;
                sym = s2;
            }
        }
        if (!fn) {
            for (const std::string& start : {leafSym, resolvedLeaf}) {
                std::string cur = start;
                while (!fn) {
                    auto pit = classParentNamesBySym.find(cur);
                    if (pit == classParentNamesBySym.end()) break;
                    cur = pit->second;
                    sym = cur + "_" + methodName;
                    fn = module->getFunction(sym);
                }
                if (fn) break;
            }
        }
        if (fn && resolvedSymbol) *resolvedSymbol = sym;
        return fn;
    }

bool CodeGen::Impl::isExprDefinitelyNonNeg(Expr* e) const {
        if (!e) return false;
        if (auto* il = dynamic_cast<IntegerLiteral*>(e))
            return il->value >= 0;
        if (auto* ne = dynamic_cast<NameExpr*>(e))
            return knownNonNeg.count(ne->name) > 0;
        if (auto* be = dynamic_cast<BinaryExpr*>(e)) {
            auto op = be->op.type();
            if (op == TokenType::PLUS || op == TokenType::STAR ||
                op == TokenType::POWER || op == TokenType::DOUBLE_SLASH ||
                op == TokenType::PERCENT) {
                return isExprDefinitelyNonNeg(be->left.get()) &&
                       isExprDefinitelyNonNeg(be->right.get());
            }
        }
        if (auto* ce = dynamic_cast<CallExpr*>(e)) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(ce->callee.get())) {
                if (nameExpr->name == "len") return true;
                if (nameExpr->name == "abs") return true;
            }
        }
        return false;
    }

bool CodeGen::Impl::isLockExpr(Expr* e) {
    if (auto* nm = dynamic_cast<NameExpr*>(e)) {
        auto it = varClassNames.find(nm->name);
        return it != varClassNames.end() && it->second == "__Lock";
    }
    if (auto* at = dynamic_cast<AttributeExpr*>(e)) {
        std::string owner;
        if (auto* on = dynamic_cast<NameExpr*>(at->object.get())) {
            if (on->name == "self" && !currentClassName.empty())
                owner = currentClassName;
        }
        if (owner.empty()) owner = resolveExprClassName(at->object.get());
        if (owner.empty()) return false;
        auto cit = classFieldClassNameBySym.find(classSym(owner));
        if (cit == classFieldClassNameBySym.end()) return false;
        auto fit = cit->second.find(at->attribute);
        return fit != cit->second.end() && fit->second == "__Lock";
    }
    return false;
}

std::string CodeGen::Impl::resolveExprClassName(Expr* expr) {
        if (auto* nameExpr = dynamic_cast<NameExpr*>(expr)) {
            if (const auto* gb = globalClassBindingFor(nameExpr->name)) {
                if (classNames.count(gb->className)) return gb->className;
            }
            auto it = varClassNames.find(nameExpr->name);
            if (it != varClassNames.end() && classNames.count(it->second)) {
                VarKind vk = lookupVarKind(nameExpr->name);
                bool staleNonInstance =
                    vk == VarKind::Str  || vk == VarKind::StrLiteral ||
                    vk == VarKind::Int  || vk == VarKind::Float || vk == VarKind::Bool ||
                    vk == VarKind::List || vk == VarKind::Dict  || vk == VarKind::Tuple ||
                    vk == VarKind::Set;
                if (!staleNonInstance)
                    return it->second;
            }
        }
        if (auto* callExpr = dynamic_cast<CallExpr*>(expr)) {
            if (auto* cn = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                if (classNames.count(cn->name))
                    return cn->name;
                std::string sym = resolveCalleeSymbol(cn->name);
                auto fit = funcReturnClassNames.find(sym);
                if (fit != funcReturnClassNames.end())
                    return fit->second;
            }
            if (auto* attrCallee = dynamic_cast<AttributeExpr*>(callExpr->callee.get())) {
                if (attrCallee->object && attrCallee->object->type &&
                    attrCallee->object->type->kind() == Type::Kind::Module &&
                    classNames.count(attrCallee->attribute)) {
                    return attrCallee->attribute;
                }
                std::string objClass;
                if (auto* on = dynamic_cast<NameExpr*>(attrCallee->object.get())) {
                    if (classNames.count(on->name)) objClass = on->name;
                }
                if (objClass.empty())
                    objClass = resolveExprClassName(attrCallee->object.get());
                if (!objClass.empty()) {
                    std::string objMod;
                    if (auto* on = dynamic_cast<NameExpr*>(attrCallee->object.get())) {
                        if (const auto* gb = globalClassBindingFor(on->name)) {
                            objMod = gb->owningModule;
                        } else {
                            auto rmIt = varClassOwningModule.find(on->name);
                            if (rmIt != varClassOwningModule.end()) objMod = rmIt->second;
                        }
                    }
                    if (objMod.empty()) objMod = resolveClassOwningModule(objClass);
                    std::string resolvedSym;
                    if (resolveMethodFunction(objMod, objClass,
                                              attrCallee->attribute,
                                              &resolvedSym)) {
                        auto mIt = methodReturnClassNames.find(resolvedSym);
                        if (mIt != methodReturnClassNames.end())
                            return mIt->second;
                    }
                }
            }
        }
        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            if (auto* nm = dynamic_cast<NameExpr*>(sub->object.get())) {
                bool staleElem = false;
                if (sub->object->type) {
                    Type::Kind rk = sub->object->type->kind();
                    if (rk == Type::Kind::List) {
                        auto* lt =
                            dynamic_cast<ListType*>(sub->object->type.get());
                        bool elemIsInstance =
                            lt && lt->elementType &&
                            lt->elementType->kind() == Type::Kind::Instance;
                        if (!elemIsInstance) staleElem = true;
                    } else if (rk != Type::Kind::Unknown &&
                               rk != Type::Kind::Any) {
                        staleElem = true;
                    }
                }
                if (!staleElem) {
                    auto eit = varListElemClassName.find(nm->name);
                    if (eit != varListElemClassName.end()) return eit->second;
                }
            }
            if (auto* attr = dynamic_cast<AttributeExpr*>(sub->object.get())) {
                std::string ownerCls;
                if (auto* on = dynamic_cast<NameExpr*>(attr->object.get())) {
                    if (on->name == "self" && !currentClassName.empty())
                        ownerCls = currentClassName;
                    else {
                        auto vit = varClassNames.find(on->name);
                        if (vit != varClassNames.end()) ownerCls = vit->second;
                    }
                }
                if (!ownerCls.empty()) {
                    auto cit = classFieldListElemClassNameBySym.find(classSym(ownerCls));
                    if (cit != classFieldListElemClassNameBySym.end()) {
                        auto fit = cit->second.find(attr->attribute);
                        if (fit != cit->second.end()) return fit->second;
                    }
                }
            }
            if (sub->object && sub->object->type &&
                sub->object->type->kind() == Type::Kind::List) {
                if (auto* lt = dynamic_cast<ListType*>(sub->object->type.get())) {
                    if (lt->elementType &&
                        lt->elementType->kind() == Type::Kind::Instance) {
                        if (auto* it = dynamic_cast<InstanceType*>(lt->elementType.get())) {
                            if (it->classType && classNames.count(it->classType->name))
                                return it->classType->name;
                        }
                    }
                }
            }
        }
        if (auto* binExpr = dynamic_cast<BinaryExpr*>(expr)) {
            return resolveExprClassName(binExpr->left.get());
        }
        if (auto* unaryExpr = dynamic_cast<UnaryExpr*>(expr)) {
            return resolveExprClassName(unaryExpr->operand.get());
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
            std::string ownerCls;
            if (auto* on = dynamic_cast<NameExpr*>(attr->object.get())) {
                if (on->name == "self" && !currentClassName.empty())
                    ownerCls = currentClassName;
                else {
                    auto vit = varClassNames.find(on->name);
                    if (vit != varClassNames.end()) ownerCls = vit->second;
                }
            }
            if (ownerCls.empty()) {
                ownerCls = resolveExprClassName(attr->object.get());
            }
            if (!ownerCls.empty()) {
                auto cit = classFieldClassNameBySym.find(classSym(ownerCls));
                if (cit != classFieldClassNameBySym.end()) {
                    auto fit = cit->second.find(attr->attribute);
                    if (fit != cit->second.end()) return fit->second;
                }
            }
        }
        if (expr->type && expr->type->kind() == Type::Kind::Instance) {
            if (auto* it = dynamic_cast<InstanceType*>(expr->type.get())) {
                if (it->classType && classNames.count(it->classType->name))
                    return it->classType->name;
            }
        }
        return "";
    }

CodeGen::Impl::VarKind CodeGen::Impl::resolveExprVarKind(Expr* expr) {
        if (!expr) return VarKind::Other;
        if (auto* cast = dynamic_cast<AsCastExpr*>(expr))
            return resolveExprVarKind(cast->operand.get());
        if (auto* nameExpr = dynamic_cast<NameExpr*>(expr))
            return lookupVarKind(nameExpr->name);
        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            auto kindFromTypeKind = [](Type::Kind k) -> VarKind {
                switch (k) {
                    case Type::Kind::Str:      return VarKind::Str;
                    case Type::Kind::Float:    return VarKind::Float;
                    case Type::Kind::Bool:     return VarKind::Bool;
                    case Type::Kind::Bytes:    return VarKind::List;
                    case Type::Kind::List:     return VarKind::List;
                    case Type::Kind::Dict:     return VarKind::Dict;
                    case Type::Kind::Tuple:    return VarKind::Tuple;
                    case Type::Kind::Set:      return VarKind::Set;
                    case Type::Kind::Instance: return VarKind::ClassInstance;
            case Type::Kind::Contract: return VarKind::ClassInstance;
                    case Type::Kind::Int:      return VarKind::Int;
                    default:                   return VarKind::Other;
                }
            };
            if (auto* nm = dynamic_cast<NameExpr*>(sub->object.get())) {
                if (lookupVarKind(nm->name) == VarKind::List) {
                    auto it = varListElemKinds.find(nm->name);
                    if (it != varListElemKinds.end())
                        return kindFromTypeKind(it->second);
                }
            }
            if (auto* attr = dynamic_cast<AttributeExpr*>(sub->object.get())) {
                std::string cls;
                if (auto* on = dynamic_cast<NameExpr*>(attr->object.get())) {
                    if (on->name == "self" && !currentClassName.empty())
                        cls = currentClassName;
                    else {
                        auto vit = varClassNames.find(on->name);
                        if (vit != varClassNames.end()) cls = vit->second;
                    }
                }
                if (!cls.empty()) {
                    auto cit = classFieldListElemKindsBySym.find(classSym(cls));
                    if (cit != classFieldListElemKindsBySym.end()) {
                        auto fit = cit->second.find(attr->attribute);
                        if (fit != cit->second.end())
                            return kindFromTypeKind(fit->second);
                    }
                }
            }
            // dict[K,V] subscript -> value kind. Without this a borrowed ternary read
            // (`d["k"] if cond else ""`) resolves Other, skips the IfExpr incref, and double-frees on teardown.
            if (auto* nm = dynamic_cast<NameExpr*>(sub->object.get())) {
                if (lookupVarKind(nm->name) == VarKind::Dict) {
                    auto it = varDictValueKinds.find(nm->name);
                    if (it != varDictValueKinds.end())
                        return kindFromTypeKind(it->second);
                }
            }
            if (auto* attr = dynamic_cast<AttributeExpr*>(sub->object.get())) {
                std::string cls;
                if (auto* on = dynamic_cast<NameExpr*>(attr->object.get())) {
                    if (on->name == "self" && !currentClassName.empty())
                        cls = currentClassName;
                    else {
                        auto vit = varClassNames.find(on->name);
                        if (vit != varClassNames.end()) cls = vit->second;
                    }
                }
                if (!cls.empty()) {
                    auto cit = classFieldDictValueKindsBySym.find(classSym(cls));
                    if (cit != classFieldDictValueKindsBySym.end()) {
                        auto fit = cit->second.find(attr->attribute);
                        if (fit != cit->second.end())
                            return kindFromTypeKind(fit->second);
                    }
                }
            }
            // Fallback: subscript's own result type. A field declared only via ctor
            // assignment (`self.cookies = parse(...)`) has no side-map entry, so without this it double-frees on teardown.
            if (sub->type) {
                VarKind k = kindFromTypeKind(sub->type->kind());
                if (k != VarKind::Other) return k;
            }
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
            std::string cls;
            if (auto* on = dynamic_cast<NameExpr*>(attr->object.get())) {
                if (on->name == "self" && !currentClassName.empty())
                    cls = currentClassName;
                else {
                    auto vit = varClassNames.find(on->name);
                    if (vit != varClassNames.end()) cls = vit->second;
                }
            }
            if (!cls.empty()) {
                auto cit = classFieldKindsBySym.find(classSym(cls));
                if (cit != classFieldKindsBySym.end()) {
                    auto fit = cit->second.find(attr->attribute);
                    if (fit != cit->second.end()) return fit->second;
                }
            }
        }
        if (dynamic_cast<StringLiteral*>(expr)) return VarKind::Str;
        if (dynamic_cast<IntegerLiteral*>(expr)) return VarKind::Int;
        if (dynamic_cast<FloatLiteral*>(expr)) return VarKind::Float;
        if (dynamic_cast<BooleanLiteral*>(expr)) return VarKind::Bool;
        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            if (auto* attr = dynamic_cast<AttributeExpr*>(call->callee.get())) {
                static const std::set<std::string> setResultMethods = {
                    "union", "intersection", "difference",
                    "symmetric_difference", "copy"};
                if (setResultMethods.count(attr->attribute)) {
                    Expr* recv = attr->object.get();
                    bool recvIsSet = dynamic_cast<SetExpr*>(recv) ||
                                     dynamic_cast<SetCompExpr*>(recv) ||
                                     resolveExprVarKind(recv) == VarKind::Set;
                    if (recvIsSet) return VarKind::Set;
                }
            }
            if (auto* cn = dynamic_cast<NameExpr*>(call->callee.get())) {
                if ((cn->name == "set" || cn->name == "frozenset") &&
                    call->args.size() == 1)
                    return VarKind::Set;
            }
        }
        return VarKind::Other;
    }

llvm::Value* CodeGen::Impl::callDunder(const std::string& className, const std::string& dunder,
                        llvm::Value* self, const std::vector<llvm::Value*>& extraArgs) {
        std::string defClass = findDunderClass(className, dunder);
        if (defClass.empty()) return nullptr;
        std::string funcName = defClass + "_" + dunder;
        auto* func = module->getFunction(funcName);
        if (!func) return nullptr;
        return emitDunderCall(func, dunder, self, extraArgs);
    }

llvm::Value* CodeGen::Impl::emitDunderCall(llvm::Function* func, const std::string& dunder,
                        llvm::Value* self, const std::vector<llvm::Value*>& extraArgs) {
        std::vector<llvm::Value*> args = {self};
        args.insert(args.end(), extraArgs.begin(), extraArgs.end());
        auto* fty = func->getFunctionType();
        for (unsigned p = static_cast<unsigned>(args.size());
             p < fty->getNumParams(); ++p)
            args.push_back(llvm::Constant::getNullValue(fty->getParamType(p)));
        if (func->getReturnType()->isVoidTy())
            return builder->CreateCall(func, args);
        return builder->CreateCall(func, args, dunder);
    }

llvm::Value* CodeGen::Impl::toBool(llvm::Value* val, Expr* exprNode) {
        if (val->getType() == i1Type) return val;
        if (val->getType() == i64Type) {
            return builder->CreateICmpNE(val, llvm::ConstantInt::get(i64Type, 0), "tobool");
        }
        if (val->getType() == f64Type) {
            return builder->CreateFCmpONE(val, llvm::ConstantFP::get(f64Type, 0.0), "tobool");
        }
        if (val->getType() == i8PtrType || val->getType()->isPointerTy()) {
            std::string cls;
            if (exprNode) cls = resolveExprClassName(exprNode);
            if (!cls.empty() && hasDunder(cls, "__bool__")) {
                auto* result = callDunder(cls, "__bool__", val);
                if (result->getType() == i64Type)
                    return builder->CreateICmpNE(result, llvm::ConstantInt::get(i64Type, 0), "tobool");
                return result;
            }
            const char* lenFn = nullptr;
            if (exprNode && exprNode->type) {
                switch (exprNode->type->kind()) {
                    case Type::Kind::Str:   lenFn = "dragon_str_len";   break;
                    case Type::Kind::List:  lenFn = "dragon_list_len";  break;
                    case Type::Kind::Dict:  lenFn = "dragon_dict_len";  break;
                    case Type::Kind::Set:   lenFn = "dragon_set_len";   break;
                    case Type::Kind::Tuple: lenFn = "dragon_tuple_len"; break;
                    case Type::Kind::Bytes: lenFn = "dragon_bytes_len"; break;
                    default: break;
                }
            }
            if (lenFn) {
                auto* fn = getOrDeclareRuntime(lenFn,
                    llvm::FunctionType::get(i64Type, {i8PtrType}, false));
                llvm::Value* len = builder->CreateCall(fn, {val}, "len");
                return builder->CreateICmpNE(len, llvm::ConstantInt::get(i64Type, 0), "tobool");
            }
            if (!cls.empty() && hasDunder(cls, "__len__")) {
                llvm::Value* len = callDunder(cls, "__len__", val);
                if (len->getType() != i64Type)
                    len = builder->CreateZExtOrTrunc(len, i64Type, "lenext");
                return builder->CreateICmpNE(len, llvm::ConstantInt::get(i64Type, 0), "tobool");
            }
            return builder->CreateICmpNE(
                val, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(val->getType())), "tobool");
        }
        return val;
    }

bool CodeGen::Impl::isOwnedStrResult(llvm::Value* v) {
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            if (phi->getType() != i8PtrType || phi->getNumIncomingValues() == 0)
                return false;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* inc = phi->getIncomingValue(i);
                if (llvm::isa<llvm::PHINode>(inc) || !isOwnedStrResult(inc))
                    return false;
            }
            return true;
        }
        auto* call = llvm::dyn_cast<llvm::CallInst>(v);
        if (!call) return false;
        if (call->getType() != i8PtrType) return false;
        auto* fn = call->getCalledFunction();
        if (!fn) return true;
        return !isBorrowedStrReturnerName(fn->getName().str());
    }

bool CodeGen::Impl::isBorrowedStrReturnerName(const std::string& name) {
        static const std::unordered_set<std::string> kBorrowedStrReturners = {
            "dragon_exc_get_msg",
            "dragon_dict_get_str_ptr",
            "sqlite3_column_text",
            "sqlite3_column_name",
            "sqlite3_errmsg",
        };
        return kBorrowedStrReturners.count(name) != 0;
    }

bool CodeGen::Impl::isOwnedPtrResult(llvm::Value* v) {
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            if (phi->getType() != i8PtrType || phi->getNumIncomingValues() == 0)
                return false;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* inc = phi->getIncomingValue(i);
                if (llvm::isa<llvm::PHINode>(inc) || !isOwnedPtrResult(inc))
                    return false;
            }
            return true;
        }
        auto* call = llvm::dyn_cast<llvm::CallInst>(v);
        if (!call) return false;
        if (!call->getType()->isPointerTy()) return false;
        auto* fn = call->getCalledFunction();
        if (!fn) return true;
        static const std::unordered_set<std::string> kBorrowedPtrReturners = {
            "dragon_list_get_ptr",
            "dragon_dict_int_get_ptr",
            "dragon_dict_get_str_ptr",
            "dragon_exc_get_msg",
            "dragon_exc_get_obj",
        };
        return kBorrowedPtrReturners.count(fn->getName().str()) == 0;
    }

bool CodeGen::Impl::isOwnedBoxResult(llvm::Value* v) {
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            if (phi->getType() != boxType || phi->getNumIncomingValues() == 0)
                return false;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* inc = phi->getIncomingValue(i);
                if (llvm::isa<llvm::PHINode>(inc) || !isOwnedBoxResult(inc))
                    return false;
            }
            return true;
        }
        auto* call = llvm::dyn_cast<llvm::CallInst>(v);
        if (!call) return false;
        if (call->getType() != boxType) return false;
        auto* fn = call->getCalledFunction();
        if (!fn) return true;
        static const std::unordered_set<std::string> kBorrowedBoxReturners = {
            "dragon_dict_get_box",
            "dragon_dict_int_get_box",
            "dragon_list_box_get",
            "dragon_tuple_box_get",
        };
        return kBorrowedBoxReturners.count(fn->getName().str()) == 0;
    }

int64_t CodeGen::Impl::typeKindToTag(Type::Kind k) {
        switch (k) {
            case Type::Kind::Int:      return TAG_INT;
            case Type::Kind::Str:      return TAG_STR;
            case Type::Kind::Float:    return TAG_FLOAT;
            case Type::Kind::Bool:     return TAG_BOOL;
            case Type::Kind::List:     return TAG_LIST;
            case Type::Kind::Dict:     return TAG_DICT;
            case Type::Kind::Bytes:    return TAG_BYTES;
            case Type::Kind::Instance: return 7;
            case Type::Kind::Contract: return 7;
            case Type::Kind::Tuple:    return TAG_LIST;
            case Type::Kind::Set:      return TAG_LIST;
            case Type::Kind::Function: return TAG_CALLABLE;
            default:                   return -1;
        }
    }

int64_t CodeGen::Impl::varKindToTag(VarKind vk) {
        switch (vk) {
            case VarKind::Int:           return TAG_INT;
            case VarKind::Str:
            case VarKind::StrLiteral:    return TAG_STR;
            case VarKind::Float:         return TAG_FLOAT;
            case VarKind::Bool:          return TAG_BOOL;
            case VarKind::List:          return TAG_LIST;
            case VarKind::Dict:          return TAG_DICT;
            case VarKind::ClassInstance: return 7;
            case VarKind::Generator:     return 8;
            case VarKind::Type:          return 9;
            case VarKind::Closure:       return TAG_CALLABLE;
            case VarKind::Union:         return -1;
            default:                     return -1;
        }
    }

Type::Kind CodeGen::Impl::resolveDictKeyKind(Expr* expr) {
        if (!expr) return Type::Kind::Unknown;
        if (expr->type) {
            if (auto* dt = dynamic_cast<DictType*>(expr->type.get())) {
                if (dt->keyType && dt->keyType->kind() != Type::Kind::Unknown)
                    return dt->keyType->kind();
            }
        }
        if (auto* name = dynamic_cast<NameExpr*>(expr)) {
            auto it = varDictKeyKinds.find(name->name);
            if (it != varDictKeyKinds.end()) return it->second;
            return Type::Kind::Unknown;
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
            std::string cls;
            if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
                if (objName->name == "self" && !currentClassName.empty())
                    cls = currentClassName;
                else {
                    auto vit = varClassNames.find(objName->name);
                    if (vit != varClassNames.end()) cls = vit->second;
                }
            } else {
                cls = resolveExprClassName(attr->object.get());
            }
            if (!cls.empty()) {
                auto cit = classFieldDictKeyKindsBySym.find(classSym(cls));
                if (cit != classFieldDictKeyKindsBySym.end()) {
                    auto fit = cit->second.find(attr->attribute);
                    if (fit != cit->second.end()) return fit->second;
                }
            }
        }
        return Type::Kind::Unknown;
    }

Type::Kind CodeGen::Impl::resolveDictValueKind(Expr* expr) {
        if (!expr) return Type::Kind::Unknown;
        if (expr->type) {
            if (auto* dt = dynamic_cast<DictType*>(expr->type.get())) {
                if (dt->valueType && dt->valueType->kind() != Type::Kind::Unknown)
                    return dt->valueType->kind();
            }
        }
        if (auto* name = dynamic_cast<NameExpr*>(expr)) {
            auto it = varDictValueKinds.find(name->name);
            if (it != varDictValueKinds.end()) return it->second;
            return Type::Kind::Unknown;
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
            std::string cls;
            if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
                if (objName->name == "self" && !currentClassName.empty())
                    cls = currentClassName;
                else {
                    auto vit = varClassNames.find(objName->name);
                    if (vit != varClassNames.end()) cls = vit->second;
                }
            } else {
                cls = resolveExprClassName(attr->object.get());
            }
            if (!cls.empty()) {
                auto cit = classFieldDictValueKindsBySym.find(classSym(cls));
                if (cit != classFieldDictValueKindsBySym.end()) {
                    auto fit = cit->second.find(attr->attribute);
                    if (fit != cit->second.end()) return fit->second;
                }
            }
        }
        return Type::Kind::Unknown;
    }

std::string CodeGen::Impl::recordVarClassFromValue(const std::string& varName,
                                                   Expr* value) {
    if (varClassNames.count(varName)) return "";
    auto cls = resolveExprClassName(value);
    if (cls.empty()) return "";
    varClassNames[varName] = cls;
    std::string owningMod;
    if (auto* call = dynamic_cast<CallExpr*>(value)) {
        if (auto* attrCallee = dynamic_cast<AttributeExpr*>(call->callee.get())) {
            if (attrCallee->object && attrCallee->object->type &&
                attrCallee->object->type->kind() == Type::Kind::Module) {
                owningMod = static_cast<ModuleType&>(*attrCallee->object->type).name;
            } else if (auto* recvName = dynamic_cast<NameExpr*>(attrCallee->object.get())) {
                auto rmIt = varClassOwningModule.find(recvName->name);
                if (rmIt != varClassOwningModule.end()) owningMod = rmIt->second;
            }
        }
    }
    if (owningMod.empty()) owningMod = resolveClassOwningModule(cls);
    varClassOwningModule[varName] = owningMod;
    return cls;
}

int64_t CodeGen::Impl::typeKindToElemTag(dragon::Type::Kind k) {
        switch (k) {
            case Type::Kind::Str:      return TAG_STR;
            case Type::Kind::Float:    return TAG_FLOAT;
            case Type::Kind::Bool:     return TAG_BOOL;
            case Type::Kind::List:     return TAG_LIST;
            case Type::Kind::Dict:     return TAG_DICT;
            case Type::Kind::Bytes:    return TAG_BYTES;
            case Type::Kind::Tuple:    return TAG_LIST;
            case Type::Kind::Set:      return TAG_LIST;
            case Type::Kind::Instance: return TAG_LIST;
            case Type::Kind::Function: return TAG_CALLABLE;
            case Type::Kind::None_:    return TAG_NONE;
            case Type::Kind::Int:
            case Type::Kind::Any:
            case Type::Kind::Unknown:
            default:                   return TAG_INT;
        }
    }

std::string CodeGen::Impl::containerReprFn(Expr* e) {
        if (!e) return "";
        VarKind vk = resolveExprVarKind(e);
        if (vk == VarKind::List || dynamic_cast<ListExpr*>(e) ||
            dynamic_cast<ListCompExpr*>(e)) {
            if (getIterableElementKind(e) == Type::Kind::Any)
                return "dragon_list_box_to_str";
            return "dragon_list_to_str";
        }
        if (vk == VarKind::Deque) return "dragon_deque_to_str";
        if (vk == VarKind::Set || dynamic_cast<SetExpr*>(e) ||
            dynamic_cast<SetCompExpr*>(e))
            return "dragon_set_to_str";
        if (vk == VarKind::Dict || dynamic_cast<DictExpr*>(e) ||
            dynamic_cast<DictCompExpr*>(e))
            return dictKeyUsesIntEngine(e) ? "dragon_dict_int_to_str" : "dragon_dict_to_str";
        if (vk == VarKind::Tuple || dynamic_cast<TupleExpr*>(e))
            return "dragon_tuple_to_str";
        if (e->type) {
            if (e->type->kind() == Type::Kind::Dict)
                return dictKeyUsesIntEngine(e) ? "dragon_dict_int_to_str" : "dragon_dict_to_str";
            if (e->type->kind() == Type::Kind::Tuple) return "dragon_tuple_to_str";
        }
        return "";
    }

CodeGen::Impl::VarKind CodeGen::Impl::typeKindToVarKind(Type::Kind k) {
        switch (k) {
            case Type::Kind::Int:      return VarKind::Int;
            case Type::Kind::Float:    return VarKind::Float;
            case Type::Kind::Bool:     return VarKind::Bool;
            case Type::Kind::Str:      return VarKind::Str;
            case Type::Kind::Bytes:    return VarKind::List;
            case Type::Kind::List:     return VarKind::List;
            case Type::Kind::Dict:     return VarKind::Dict;
            case Type::Kind::Tuple:    return VarKind::Tuple;
            case Type::Kind::Set:      return VarKind::Set;
            case Type::Kind::Instance: return VarKind::ClassInstance;
            case Type::Kind::Contract: return VarKind::ClassInstance;
            case Type::Kind::Function: return VarKind::Closure;
            case Type::Kind::Any:      return VarKind::Union;
            default:                   return VarKind::Int;
        }
    }

llvm::Type* CodeGen::Impl::typeKindToLLVM(Type::Kind k) const {
        switch (k) {
            case Type::Kind::Int:      return i64Type;
            case Type::Kind::Float:    return f64Type;
            case Type::Kind::Bool:     return i1Type;
            case Type::Kind::Str:
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Tuple:
            case Type::Kind::Set:
            case Type::Kind::Instance:
            case Type::Kind::Contract:
            case Type::Kind::Module:
            case Type::Kind::Class:    return i8PtrType;
            case Type::Kind::Any:      return boxType;
            default:                   return i64Type;
        }
    }

bool CodeGen::Impl::isHeapTypeKind(Type::Kind k) {
        switch (k) {
            case Type::Kind::Str:
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Tuple:
            case Type::Kind::Set:
            case Type::Kind::Instance:
            // A `for f in list[Callable]` elem must be BORROWED (the list owns it),
            // else per-iteration cleanup frees a closure the list still holds (UAF).
            case Type::Kind::Function:
            // list[Any] iteration elements are BORROWED boxes (the list keeps the
            // +1); else per-iteration cleanup double-frees on list destroy (the JSON UAF).
            case Type::Kind::Any:
                return true;
            default:
                return false;
        }
    }

bool CodeGen::Impl::isBareDictIterable(Expr* expr) {
        if (!expr) return false;
        if (auto* name = dynamic_cast<NameExpr*>(expr))
            return lookupVarKind(name->name) == VarKind::Dict;
        if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
            std::string cls;
            if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
                if (objName->name == "self" && !currentClassName.empty())
                    cls = currentClassName;
                else {
                    auto vit = varClassNames.find(objName->name);
                    if (vit != varClassNames.end()) cls = vit->second;
                }
            }
            if (!cls.empty()) {
                auto cit = classFieldDictKeyKindsBySym.find(classSym(cls));
                if (cit != classFieldDictKeyKindsBySym.end() &&
                    cit->second.count(attr->attribute))
                    return true;
            }
        }
        if (expr->type && expr->type->kind() == Type::Kind::Dict)
            return true;
        return false;
    }

int64_t CodeGen::Impl::inferPtrValueTag(Expr* expr) {
        if (expr && expr->type) {
            if (expr->type->kind() == Type::Kind::Instance) return 7;
            return typeKindToElemTag(expr->type->kind());
        }
        if (auto* sl = dynamic_cast<StringLiteral*>(expr))
            return sl->isBytes ? TAG_BYTES : TAG_STR;
        if (dynamic_cast<ListExpr*>(expr) || dynamic_cast<ListCompExpr*>(expr))
            return TAG_LIST;
        if (dynamic_cast<DictExpr*>(expr) || dynamic_cast<DictCompExpr*>(expr))
            return TAG_DICT;
        if (dynamic_cast<TupleExpr*>(expr))
            return TAG_LIST;
        if (dynamic_cast<SetExpr*>(expr) || dynamic_cast<SetCompExpr*>(expr))
            return TAG_LIST;
        if (auto* nameExpr = dynamic_cast<NameExpr*>(expr)) {
            VarKind vk = lookupVarKind(nameExpr->name);
            switch (vk) {
                case VarKind::Str:
                case VarKind::StrLiteral: return TAG_STR;
                case VarKind::List:       return 5;
                case VarKind::Dict:       return 6;
                case VarKind::Tuple:      return 5;
                case VarKind::Set:        return 5;
                case VarKind::ClassInstance: return 7;
                default: break;
            }
        }
        if (auto* callExpr = dynamic_cast<CallExpr*>(expr)) {
            if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                if (classNames.count(calleeName->name))
                    return 7;
            }
        }
        return 1;
    }

void CodeGen::Impl::emitCleanupPush(const std::string& name, llvm::Value* value,
                     int cleanupKind, llvm::Value* tagVal) {
        if (options.gcMode != GCMode::RC || cleanupKind == 0 || name.empty()) return;
        if (scopes.empty()) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* func = currentFunction;
        auto* slotAlloca = createEntryAllocaI32(func, name + ".clslot", -1);
        scopes.back().cleanupSlots[name] = slotAlloca;
        if (!scopes.back().cleanupBaseAlloca)
            scopes.back().cleanupBaseAlloca = createEntryAllocaI32(func, "clbase", -1);
        auto* baseAlloca = scopes.back().cleanupBaseAlloca;

        auto* valI64 = cleanupValToI64(value);
        auto* kindC = llvm::ConstantInt::get(i32Ty, cleanupKind);
        auto* tagC = tagVal ? builder->CreateTrunc(tagVal, i32Ty, "clean.tag")
                            : llvm::ConstantInt::get(i32Ty, 0);
        auto* doBB   = llvm::BasicBlock::Create(*context, "clpush.do", func);
        auto* contBB = llvm::BasicBlock::Create(*context, "clpush.cont", func);
        builder->CreateCondBr(emitActiveFramesNonZero(), doBB, contBB);

        builder->SetInsertPoint(doBB);
        auto* slot = builder->CreateCall(runtimeFuncs["dragon_cleanup_push"],
                                         {valI64, kindC, tagC}, "clean.slot");
        builder->CreateStore(slot, slotAlloca);
        auto* curBase = builder->CreateLoad(i32Ty, baseAlloca, "clbase.cur");
        auto* isFirst = builder->CreateICmpEQ(
            curBase, llvm::ConstantInt::get(i32Ty, -1), "clbase.first");
        builder->CreateStore(builder->CreateSelect(isFirst, slot, curBase, "clbase.new"),
                             baseAlloca);
        builder->CreateBr(contBB);

        builder->SetInsertPoint(contBB);
    }

void CodeGen::Impl::emitCleanupUpdate(const std::string& name, llvm::Value* value,
                       llvm::Value* tagVal) {
        if (options.gcMode != GCMode::RC || name.empty()) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        auto* slotAlloca = findCleanupSlot(name);
        if (!slotAlloca) return;
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* func = currentFunction;
        auto* slot = builder->CreateLoad(i32Ty, slotAlloca, name + ".clslot.l");
        auto* valI64 = cleanupValToI64(value);
        auto* tagC = tagVal ? builder->CreateTrunc(tagVal, i32Ty, "clean.tag")
                            : llvm::ConstantInt::get(i32Ty, 0);
        auto* pushed = builder->CreateICmpSGE(
            slot, llvm::ConstantInt::get(i32Ty, 0), "clslot.pushed");
        auto* doBB   = llvm::BasicBlock::Create(*context, "clupd.do", func);
        auto* contBB = llvm::BasicBlock::Create(*context, "clupd.cont", func);
        builder->CreateCondBr(pushed, doBB, contBB);
        builder->SetInsertPoint(doBB);
        builder->CreateCall(runtimeFuncs["dragon_cleanup_update"], {slot, valI64, tagC});
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
    }

llvm::Value* CodeGen::Impl::emitCleanupPushTemp(llvm::Value* ptr, int cleanupKind) {
        if (options.gcMode != GCMode::RC) return nullptr;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return nullptr;
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* func = currentFunction;
        auto* baseAlloca = createEntryAllocaI32(func, "tmp.clbase.slot", -1);
        auto* valI64 = cleanupValToI64(ptr);
        auto* doBB   = llvm::BasicBlock::Create(*context, "cltmp.do", func);
        auto* contBB = llvm::BasicBlock::Create(*context, "cltmp.cont", func);
        builder->CreateCondBr(emitActiveFramesNonZero(), doBB, contBB);
        builder->SetInsertPoint(doBB);
        auto* base = builder->CreateCall(runtimeFuncs["dragon_cleanup_depth"], {}, "tmp.clbase");
        builder->CreateStore(base, baseAlloca);
        builder->CreateCall(runtimeFuncs["dragon_cleanup_push"],
            {valI64, llvm::ConstantInt::get(i32Ty, cleanupKind),
             llvm::ConstantInt::get(i32Ty, 0)});
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
        return baseAlloca;
    }

void CodeGen::Impl::emitCleanupPopTemp(llvm::Value* baseAlloca) {
        if (!baseAlloca || options.gcMode != GCMode::RC) return;
        auto* bb = builder->GetInsertBlock();
        if (!bb || bb->getTerminator()) return;
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* func = currentFunction;
        auto* base = builder->CreateLoad(i32Ty, baseAlloca, "tmp.clbase.l");
        auto* pushed = builder->CreateICmpSGE(
            base, llvm::ConstantInt::get(i32Ty, 0), "tmp.clbase.pushed");
        auto* doBB   = llvm::BasicBlock::Create(*context, "cltmppop.do", func);
        auto* contBB = llvm::BasicBlock::Create(*context, "cltmppop.cont", func);
        builder->CreateCondBr(pushed, doBB, contBB);
        builder->SetInsertPoint(doBB);
        builder->CreateCall(runtimeFuncs["dragon_cleanup_reset"], {base});
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
    }

std::vector<llvm::Value*> CodeGen::Impl::pushArgTempCleanups(
        const std::vector<std::pair<llvm::Value*, VarKind>>& argTemps) {
        std::vector<llvm::Value*> bases;
        if (options.gcMode != GCMode::RC) return bases;
        for (auto& [v, k] : argTemps) {
            int ck = cleanupKindFor(k);
            if (ck == DCLEAN_STR || ck == DCLEAN_CALLABLE || ck == DCLEAN_OBJ)
                bases.push_back(emitCleanupPushTemp(v, ck));
        }
        return bases;
    }

void CodeGen::Impl::popArgTempCleanups(const std::vector<llvm::Value*>& bases) {
        for (auto it = bases.rbegin(); it != bases.rend(); ++it)
            emitCleanupPopTemp(*it);
    }

void CodeGen::Impl::emitScopeCleanupFor(Scope& scope) {
        for (auto it = scope.deferred.rbegin(); it != scope.deferred.rend(); ++it) {
            auto* argsPtr = builder->CreateConstInBoundsGEP2_64(
                it->argSlots->getAllocatedType(), it->argSlots, 0, 0,
                "defer.args.p");
            builder->CreateCall(it->thunk, {argsPtr});
        }
        for (auto& d : scope.deferred) {
            for (unsigned i = 0; i < d.argc; ++i) {
                if (d.drainKinds[i] == VarKind::Other) continue;
                auto* slotPtr = builder->CreateConstInBoundsGEP2_64(
                    d.argSlots->getAllocatedType(), d.argSlots, 0, i,
                    "defer.drain.p");
                auto* v64 = builder->CreateLoad(i64Type, slotPtr, "defer.drain.v");
                auto* p = builder->CreateIntToPtr(v64, i8PtrType, "defer.drain.ptr");
                emitDecrefByKind(p, d.drainKinds[i]);
            }
        }
        for (auto& [name, alloca] : scope.vars) {
            if (scope.borrowed.count(name)) continue;
            if (scope.stackAllocated.count(name)) continue;
            auto kindIt = scope.varKinds.find(name);
            if (kindIt == scope.varKinds.end()) continue;
            VarKind kind = kindIt->second;
            if (scope.cellBacked.count(name)) {
                auto* cellPtr = builder->CreateLoad(i8PtrType, alloca, name + ".cell.gc");
                builder->CreateCall(runtimeFuncs["dragon_decref"], {cellPtr});
                continue;
            }
            if (!isHeapKind(kind)) continue;
            if (kind == VarKind::Union || alloca->getAllocatedType() == boxType) {
                auto* box = builder->CreateLoad(boxType, alloca, name + ".box.gc");
                auto* tag = boxTag(box, name + ".tag.gc");
                auto* payload = boxPayloadI64(box, name + ".payload.gc");
                emitUnionDecref(payload, tag);
                continue;
            }
            auto* val = builder->CreateLoad(i8PtrType, alloca, name + ".gc");
            if (kind == VarKind::Str) {
                builder->CreateCall(runtimeFuncs["dragon_decref_str"], {val});
            } else if (kind == VarKind::Closure) {
                builder->CreateCall(runtimeFuncs["dragon_decref_callable"], {val});
            } else {
                builder->CreateCall(runtimeFuncs["dragon_decref"], {val});
            }
        }
        // Task-detach tail: a fire-and-forget Task is a bare DragonVThread* (no
        // header), released via vthread refcount not dragon_decref; detach is idempotent with join (CAS), so no double-free either way.
        for (const auto& name : scope.detachOnExit) {
            auto vit = scope.vars.find(name);
            if (vit == scope.vars.end()) continue;
            auto* tv = builder->CreateLoad(i8PtrType, vit->second, name + ".task.detach");
            builder->CreateCall(runtimeFuncs["dragon_vthread_detach"], {tv});
        }
        for (const auto& name : scope.lockDestroyOnExit) {
            auto vit = scope.vars.find(name);
            if (vit == scope.vars.end()) continue;
            if (scope.borrowed.count(name)) continue;
            auto* lv = builder->CreateLoad(i8PtrType, vit->second,
                                           name + ".lock.exit");
            auto* nonNull = builder->CreateICmpNE(
                lv,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(lv->getType())),
                name + ".lock.set");
            auto* func = currentFunction;
            auto* relBB =
                llvm::BasicBlock::Create(*context, name + ".lock.rel", func);
            auto* contBB =
                llvm::BasicBlock::Create(*context, name + ".lock.cont", func);
            builder->CreateCondBr(nonNull, relBB, contBB);
            builder->SetInsertPoint(relBB);
            builder->CreateCall(runtimeFuncs["dragon_lock_destroy"], {lv});
            builder->CreateBr(contBB);
            builder->SetInsertPoint(contBB);
        }
        if (scope.cleanupBaseAlloca) {
            auto* i32Ty = llvm::Type::getInt32Ty(*context);
            auto* func = currentFunction;
            auto* base = builder->CreateLoad(i32Ty, scope.cleanupBaseAlloca, "clbase.l");
            auto* pushed = builder->CreateICmpSGE(
                base, llvm::ConstantInt::get(i32Ty, 0), "clbase.pushed");
            auto* doBB   = llvm::BasicBlock::Create(*context, "clreset.do", func);
            auto* contBB = llvm::BasicBlock::Create(*context, "clreset.cont", func);
            builder->CreateCondBr(pushed, doBB, contBB);
            builder->SetInsertPoint(doBB);
            builder->CreateCall(runtimeFuncs["dragon_cleanup_reset"], {base});
            builder->CreateStore(llvm::ConstantInt::get(i32Ty, -1), scope.cleanupBaseAlloca);
            builder->CreateBr(contBB);
            builder->SetInsertPoint(contBB);
        }
    }


}
