#include "dragon/TypeChecker.h"
#include "dragon/Privacy.h"
#include "TypeCheckerImpl.h"
#include "dragon/AstClone.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <functional>
#include <set>
#include <system_error>

namespace dragon {

static bool annotationElementIsAny(const std::shared_ptr<Type>& t) {
    if (!t) return false;
    if (auto* lt = dynamic_cast<ListType*>(t.get()))
        return lt->elementType && lt->elementType->kind() == Type::Kind::Any;
    if (auto* dt = dynamic_cast<DictType*>(t.get()))
        return dt->valueType && dt->valueType->kind() == Type::Kind::Any;
    return false;
}

static bool containerElementAnnotationIsType(TypeExpr* ann) {
    auto* g = dynamic_cast<GenericTypeExpr*>(ann);
    if (!g) return false;
    auto* base = dynamic_cast<NamedTypeExpr*>(g->base.get());
    if (!base) return false;
    auto isTypeArg = [](TypeExpr* a) {
        auto* n = dynamic_cast<NamedTypeExpr*>(a);
        return n && n->name == "type";
    };
    bool listLike = base->name == "list" || base->name == "List" ||
                    base->name == "set" || base->name == "Set";
    if (listLike && g->typeArgs.size() == 1)
        return isTypeArg(g->typeArgs[0].get());
    if ((base->name == "dict" || base->name == "Dict") && g->typeArgs.size() == 2)
        return isTypeArg(g->typeArgs[1].get());
    return false;
}

bool TypeChecker::diagnoseHeterogeneousLiteral(
    Expr* value, const std::shared_ptr<Type>& annot) {
    if (!value || !annot) return false;
    if (annot->kind() != Type::Kind::List) return false;
    const auto& elemT = static_cast<const ListType&>(*annot).elementType;
    if (!elemT || elemT->kind() == Type::Kind::Any ||
        elemT->kind() == Type::Kind::Unknown)
        return false;

    const std::vector<std::unique_ptr<Expr>>* elems = nullptr;
    if (auto* lit = dynamic_cast<ListExpr*>(value)) elems = &lit->elements;
    else if (auto* st = dynamic_cast<SetExpr*>(value)) elems = &st->elements;
    if (!elems) return false;

    for (auto& e : *elems) {
        if (e->type && e->type->kind() != Type::Kind::Unknown &&
            !e->type->isSubtypeOf(*elemT)) {
            error(value->location(),
                  "list element of type '" + e->type->toString() +
                  "' is not assignable to element type '" + elemT->toString() +
                  "'");
            return true;
        }
    }
    return false;
}

bool TypeChecker::tryExpectedTypeLiteral(Expr* value, const std::shared_ptr<Type>& expected) {
    if (!value || !expected) return false;
    if (expected->kind() == Type::Kind::List) {
        if (auto* setLit = dynamic_cast<SetExpr*>(value)) {
            const auto& base =
                static_cast<const ListType&>(*expected).elementType;
            if (!base || setLit->elements.empty()) return false;
            for (auto& el : setLit->elements) {
                if (!el->type) return false;
                if (!el->type->isSubtypeOf(*base)) return false;
            }
            return true;
        }
        auto* lit = dynamic_cast<ListExpr*>(value);
        if (!lit || lit->elements.empty()) return false;
        const auto& base = static_cast<const ListType&>(*expected).elementType;
        if (!base) return false;
        for (auto& el : lit->elements) {
            if (!el->type) return false;
            if (!el->type->isSubtypeOf(*base)) return false;
        }
        if (base->kind() == Type::Kind::Any) {
            for (auto& el : lit->elements)
                if (el->type && el->type->kind() == Type::Kind::Class)
                    return true;
            for (auto& el : lit->elements)
                boxNestedContainerLiteralForAny(el.get());
        } else if (base->kind() == Type::Kind::List ||
                   base->kind() == Type::Kind::Dict) {
            for (auto& el : lit->elements)
                tryExpectedTypeLiteral(el.get(), base);
        }
        lit->type = expected;
        return true;
    }
    if (expected->kind() == Type::Kind::Dict) {
        auto* lit = dynamic_cast<DictExpr*>(value);
        if (!lit || lit->entries.empty()) return false;
        const auto& dt = static_cast<const DictType&>(*expected);
        for (auto& [k, v] : lit->entries) {
            if (k) {
                if (!k->type || !k->type->isSubtypeOf(*dt.keyType)) return false;
            }
            if (v) {
                if (!v->type || !v->type->isSubtypeOf(*dt.valueType)) return false;
            }
        }
        if (dt.valueType && dt.valueType->kind() == Type::Kind::Any) {
            for (auto& [k, v] : lit->entries)
                if (v) boxNestedContainerLiteralForAny(v.get());
        } else if (dt.valueType && (dt.valueType->kind() == Type::Kind::List ||
                                    dt.valueType->kind() == Type::Kind::Dict)) {
            for (auto& [k, v] : lit->entries)
                if (v) tryExpectedTypeLiteral(v.get(), dt.valueType);
        }
        lit->type = expected;
        return true;
    }
    if (expected->kind() == Type::Kind::Tuple) {
        auto* lit = dynamic_cast<TupleExpr*>(value);
        if (!lit) return false;
        const auto& tt = static_cast<const TupleType&>(*expected);
        if (lit->elements.size() != tt.elementTypes.size()) return false;
        for (size_t i = 0; i < lit->elements.size(); ++i) {
            const auto& want = tt.elementTypes[i];
            const auto& el = lit->elements[i];
            if (!want || !el || !el->type) return false;
            if (!el->type->isSubtypeOf(*want)) return false;
        }
        for (size_t i = 0; i < lit->elements.size(); ++i) {
            const auto& want = tt.elementTypes[i];
            if (want->kind() == Type::Kind::Any) {
                boxNestedContainerLiteralForAny(lit->elements[i].get());
            } else if (want->kind() == Type::Kind::List ||
                       want->kind() == Type::Kind::Dict ||
                       want->kind() == Type::Kind::Tuple) {
                tryExpectedTypeLiteral(lit->elements[i].get(), want);
            }
        }
        lit->type = expected;
        return true;
    }
    return false;
}

static bool literalElementsAreClassDescriptors(ListExpr* lit) {
    for (auto& el : lit->elements)
        if (el->type && el->type->kind() == Type::Kind::Class) return true;
    return false;
}

void TypeChecker::boxNestedContainerLiteralForAny(Expr* value) {
    if (!value) return;
    if (auto* l = dynamic_cast<ListExpr*>(value)) {
        if (literalElementsAreClassDescriptors(l)) return;
        for (auto& el : l->elements)
            boxNestedContainerLiteralForAny(el.get());
        value->type = std::make_shared<ListType>(impl_->anyType);
    } else if (auto* d = dynamic_cast<DictExpr*>(value)) {
        for (auto& [k, v] : d->entries)
            if (v) boxNestedContainerLiteralForAny(v.get());
        std::shared_ptr<Type> keyT;
        if (d->type && d->type->kind() == Type::Kind::Dict)
            keyT = static_cast<DictType&>(*d->type).keyType;
        if (!keyT) keyT = impl_->strType;
        value->type = std::make_shared<DictType>(keyT, impl_->anyType);
    }
}

std::string TypeChecker::listReprMismatchHint(const Type& from, const Type& to) {
    auto* fl = dynamic_cast<const ListType*>(&from);
    auto* tl = dynamic_cast<const ListType*>(&to);
    if (!fl || !tl || !fl->elementType || !tl->elementType) return "";
    bool fromBox = fl->elementType->kind() == Type::Kind::Any ||
                   fl->elementType->kind() == Type::Kind::Union;
    bool toBox = tl->elementType->kind() == Type::Kind::Any ||
                 tl->elementType->kind() == Type::Kind::Union;
    if (fromBox == toBox) return "";
    return " (the two have different element layouts: monomorphized vs boxed;"
           " build the value with this element type at its declaration, or copy"
           " it element-wise)";
}

void TypeChecker::propagateAnnotationToEmptyLiteral(Expr* value, const std::shared_ptr<Type>& annotType) {
    if (!value || !annotType) return;

    if (auto* list = dynamic_cast<ListExpr*>(value)) {
        if (list->elements.empty() && annotType->kind() == Type::Kind::List) {
            list->type = annotType;
        }
        return;
    }
    if (auto* dict = dynamic_cast<DictExpr*>(value)) {
        if (dict->entries.empty() && annotType->kind() == Type::Kind::Dict) {
            dict->type = annotType;
        }
        return;
    }
    if (auto* set = dynamic_cast<SetExpr*>(value)) {
        if (set->elements.empty() && annotType->kind() == Type::Kind::List) {
            set->type = annotType;
        }
        return;
    }
}

void TypeChecker::visit(ExprStmt& node) {
    if (!node.expr) return;
    auto exprType = inferType(node.expr.get());
    if (exprType && exprType->kind() == Type::Kind::Task &&
        !dynamic_cast<FireExpr*>(node.expr.get())) {
        error(node.location(), "this expression produces a '" +
              exprType->toString() +
              "' that is silently discarded; bind it, then await, join, or "
              "del it (`fire fn()` is the explicit fire-and-forget spelling)");
    }
}

void TypeChecker::visit(AssignStmt& node) {
    if (node.typeAnnotation) {
        auto annotType = resolveType(node.typeAnnotation.get());
        propagateAnnotationToEmptyLiteral(node.value.get(), annotType);
        impl_->currentExpectedType = annotType;
        auto valueType = inferType(node.value.get());
        impl_->currentExpectedType = nullptr;
        bool elemIsType = containerElementAnnotationIsType(node.typeAnnotation.get());
        if (annotationElementIsAny(annotType) && !elemIsType)
            tryExpectedTypeLiteral(node.value.get(), annotType);
        if (!diagnoseHeterogeneousLiteral(node.value.get(), annotType) &&
            annotType->kind() != Type::Kind::Unknown &&
            valueType->kind() != Type::Kind::Unknown &&
            !(elemIsType && valueType->kind() == Type::Kind::List) &&
            !valueType->isAssignableTo(*annotType) &&
            !tryExpectedTypeLiteral(node.value.get(), annotType)) {
            error(node.location(), "cannot assign '" + valueType->toString() +
                  "' to variable of type '" + annotType->toString() + "'" +
                  listReprMismatchHint(*valueType, *annotType));
        }
        for (auto& target : node.targets) {
            if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                impl_->define(name->name, annotType);
            } else if (auto* tup = dynamic_cast<TupleExpr*>(target.get())) {
                if (annotType->kind() == Type::Kind::Tuple) {
                    auto& tupleAnn = static_cast<TupleType&>(*annotType);
                    for (size_t i = 0; i < tup->elements.size() &&
                                       i < tupleAnn.elementTypes.size(); ++i) {
                        if (auto* n = dynamic_cast<NameExpr*>(tup->elements[i].get()))
                            impl_->define(n->name, tupleAnn.elementTypes[i]);
                    }
                }
            }
            inferType(target.get());
        }
    } else {
        auto valueType = inferType(node.value.get());
        for (auto& target : node.targets) {
            if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                auto existing = impl_->lookup(name->name);
                if (existing) {
                    propagateAnnotationToEmptyLiteral(node.value.get(), existing);
                    valueType = inferType(node.value.get());
                    if (!diagnoseHeterogeneousLiteral(node.value.get(), existing) &&
                        existing->kind() != Type::Kind::Unknown &&
                        valueType->kind() != Type::Kind::Unknown &&
                        !valueType->isAssignableTo(*existing) &&
                        !tryExpectedTypeLiteral(node.value.get(), existing)) {
                        error(node.location(), "cannot assign '" + valueType->toString() +
                              "' to '" + name->name + "' of type '" + existing->toString() +
                              "' (a variable's type is fixed at its declaration)");
                    }
                } else {
                    impl_->define(name->name, valueType);
                }
            } else if (auto* attr = dynamic_cast<AttributeExpr*>(target.get())) {
                auto objType = inferType(attr->object.get());
                const ClassType* recvCls = nullptr;
                if (objType && objType->kind() == Type::Kind::Instance)
                    recvCls = static_cast<const InstanceType&>(*objType).classType.get();
                else if (objType && objType->kind() == Type::Kind::Class)
                    recvCls = static_cast<const ClassType*>(objType.get());
                if (const ClassType* owner = findMethodOwner(recvCls, attr->attribute)) {
                    error(node.location(), "cannot assign to method '" +
                          attr->attribute + "' of class '" + owner->name +
                          "'; methods are not assignable - did you mean to "
                          "call it: `." + attr->attribute + "(...)`?");
                    continue;
                }
                if (objType && objType->kind() == Type::Kind::Instance) {
                    const auto& inst = static_cast<const InstanceType&>(*objType);
                    std::shared_ptr<Type> fieldType;
                    for (const ClassType* cls = inst.classType.get(); cls; ) {
                        auto it = cls->fields.find(attr->attribute);
                        if (it != cls->fields.end()) { fieldType = it->second; break; }
                        cls = (cls->parentClass && cls->parentClass->kind() == Type::Kind::Class)
                                  ? static_cast<const ClassType*>(cls->parentClass.get())
                                  : nullptr;
                    }
                    if (fieldType && fieldType->kind() != Type::Kind::Unknown) {
                        propagateAnnotationToEmptyLiteral(node.value.get(), fieldType);
                        valueType = inferType(node.value.get());
                    }
                }
            } else if (auto* tup = dynamic_cast<TupleExpr*>(target.get())) {
                if (valueType->kind() == Type::Kind::Tuple) {
                    auto& tt = static_cast<TupleType&>(*valueType);
                    for (size_t i = 0; i < tup->elements.size() &&
                                       i < tt.elementTypes.size(); ++i) {
                        auto* n = dynamic_cast<NameExpr*>(tup->elements[i].get());
                        if (n && !impl_->lookup(n->name) && tt.elementTypes[i])
                            impl_->define(n->name, tt.elementTypes[i]);
                    }
                }
            } else if (auto* sub = dynamic_cast<SubscriptExpr*>(target.get())) {
                auto contType = inferType(sub->object.get());
                bool slotIsAny = false;
                if (auto* dt = dynamic_cast<DictType*>(contType.get()))
                    slotIsAny = dt->valueType &&
                                dt->valueType->kind() == Type::Kind::Any;
                else if (auto* lt = dynamic_cast<ListType*>(contType.get()))
                    slotIsAny = lt->elementType &&
                                lt->elementType->kind() == Type::Kind::Any;
                if (slotIsAny)
                    boxNestedContainerLiteralForAny(node.value.get());
            }
            inferType(target.get());
        }
    }
}

void TypeChecker::visit(AugAssignStmt& node) {
    auto targetType = inferType(node.target.get());
    auto valueType = inferType(node.value.get());
    if (!targetType || !valueType) return;
    auto tk = targetType->kind();
    auto vk = valueType->kind();
    auto opaque = [](Type::Kind k) {
        return k == Type::Kind::Unknown || k == Type::Kind::Any ||
               k == Type::Kind::Instance || k == Type::Kind::TypeVar;
    };
    if (opaque(tk) || opaque(vk)) return;

    bool tNum = targetType->isSubtypeOf(*impl_->intType) ||
                tk == Type::Kind::Float;
    bool vNum = valueType->isSubtypeOf(*impl_->intType) ||
                vk == Type::Kind::Float;
    TokenType op = node.op.type();
    bool ok = false;
    if (tNum && vNum) {
        ok = true;
    } else if (op == TokenType::PLUS_EQUAL) {
        ok = (tk == Type::Kind::Str && vk == Type::Kind::Str) ||
             (tk == Type::Kind::Bytes && vk == Type::Kind::Bytes) ||
             (tk == Type::Kind::List && vk == Type::Kind::List);
    } else if (op == TokenType::STAR_EQUAL) {
        ok = (tk == Type::Kind::Str || tk == Type::Kind::Bytes ||
              tk == Type::Kind::List) && vNum;
    }
    if (!ok) {
        error(node.location(), "unsupported operand types for " +
              node.op.lexeme() + ": '" + targetType->toString() + "' and '" +
              valueType->toString() + "'");
    }
}

void TypeChecker::visit(AnnAssignStmt& node) {
    auto annotType = resolveType(node.annotation.get());

    if (auto* attr = dynamic_cast<AttributeExpr*>(node.target.get())) {
        auto objType = inferType(attr->object.get());
        const ClassType* recvCls = nullptr;
        if (objType && objType->kind() == Type::Kind::Instance)
            recvCls = static_cast<const InstanceType&>(*objType).classType.get();
        else if (objType && objType->kind() == Type::Kind::Class)
            recvCls = static_cast<const ClassType*>(objType.get());
        if (const ClassType* owner = findMethodOwner(recvCls, attr->attribute)) {
            error(node.location(), "cannot assign to method '" +
                  attr->attribute + "' of class '" + owner->name +
                  "'; methods are not assignable - did you mean to call it: `." +
                  attr->attribute + "(...)`?");
            return;
        }
    }

    if (node.value) {
        propagateAnnotationToEmptyLiteral(node.value.get(), annotType);
        impl_->currentExpectedType = annotType;
        auto valueType = inferType(node.value.get());
        impl_->currentExpectedType = nullptr;
        if (auto* spawnCall = dynamic_cast<CallExpr*>(node.value.get())) {
            auto calleeType = spawnCall->callee ? spawnCall->callee->type : nullptr;
            if (calleeType && calleeType->kind() == Type::Kind::Function &&
                static_cast<const FunctionType&>(*calleeType).spawnsFreshTask)
                node.valueIsFreshTask = true;
        }
        bool elemIsType = containerElementAnnotationIsType(node.annotation.get());
        if (annotationElementIsAny(annotType) && !elemIsType)
            tryExpectedTypeLiteral(node.value.get(), annotType);
        if (!diagnoseHeterogeneousLiteral(node.value.get(), annotType) &&
            annotType->kind() != Type::Kind::Unknown &&
            valueType->kind() != Type::Kind::Unknown &&
            !(elemIsType && valueType->kind() == Type::Kind::List) &&
            !valueType->isAssignableTo(*annotType) &&
            !tryExpectedTypeLiteral(node.value.get(), annotType)) {
            error(node.location(), "cannot assign '" + valueType->toString() +
                  "' to variable of type '" + annotType->toString() + "'" +
                  listReprMismatchHint(*valueType, *annotType));
        }
    }

    std::shared_ptr<Type> declType = annotType;
    if (node.value && annotType->kind() == Type::Kind::Task &&
        static_cast<TaskType&>(*annotType).resultType->kind() == Type::Kind::Any &&
        node.value->type && node.value->type->kind() == Type::Kind::Task) {
        declType = node.value->type;
    }

    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        impl_->define(name->name, declType);
    }
}

void TypeChecker::visit(IfStmt& node) {
    auto narrowedTypeFromExpr = [&](Expr* e) -> std::shared_ptr<Type> {
        auto* tn = dynamic_cast<NameExpr*>(e);
        if (!tn) return nullptr;
        if (tn->name == "int")   return impl_->intType;
        if (tn->name == "float") return impl_->floatType;
        if (tn->name == "bool")  return impl_->boolType;
        if (tn->name == "str")   return impl_->strType;
        if (tn->name == "bytes") return impl_->bytesType;
        auto tit = impl_->typeNames.find(tn->name);
        if (tit != impl_->typeNames.end()) return tit->second;
        auto found = impl_->lookup(tn->name);
        return found;
    };
    auto subtractFromUnion = [](const std::shared_ptr<Type>& cur,
                                const std::shared_ptr<Type>& sub) -> std::shared_ptr<Type> {
        if (!cur || cur->kind() != Type::Kind::Union) return nullptr;
        auto& ut = static_cast<UnionType&>(*cur);
        std::vector<std::shared_ptr<Type>> remaining;
        for (auto& m : ut.types) {
            if (!sub || !m->equals(*sub)) remaining.push_back(m);
        }
        if (remaining.empty()) return cur;
        if (remaining.size() == 1) return remaining[0];
        return std::make_shared<UnionType>(std::move(remaining));
    };
    auto analyzeIsinstance = [&](Expr* cond, std::string& outName,
                                 std::shared_ptr<Type>& outThenT,
                                 std::shared_ptr<Type>& outElseT) -> bool {
        if (auto* bin = dynamic_cast<BinaryExpr*>(cond)) {
            auto op = bin->op.type();
            bool isEq = (op == TokenType::IS || op == TokenType::EQUAL_EQUAL);
            bool isNe = (op == TokenType::IS_NOT || op == TokenType::NOT_EQUAL);
            if (isEq || isNe) {
                auto* nm = dynamic_cast<NameExpr*>(bin->left.get());
                bool noneOther = dynamic_cast<NoneLiteral*>(bin->right.get()) != nullptr;
                if (!nm || !noneOther) {
                    nm = dynamic_cast<NameExpr*>(bin->right.get());
                    noneOther = dynamic_cast<NoneLiteral*>(bin->left.get()) != nullptr;
                }
                if (nm && noneOther) {
                    auto curType = impl_->lookup(nm->name);
                    if (curType && curType->kind() == Type::Kind::Union) {
                        auto nonNone = subtractFromUnion(curType, impl_->noneType);
                        outName = nm->name;
                        if (isEq) { outThenT = impl_->noneType; outElseT = nonNone; }
                        else      { outThenT = nonNone;         outElseT = impl_->noneType; }
                        return true;
                    }
                }
            }
        }
        auto* call = dynamic_cast<CallExpr*>(cond);
        if (!call) return false;
        auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
        if (!callee || callee->name != "isinstance" || call->args.size() != 2)
            return false;
        auto* argName = dynamic_cast<NameExpr*>(call->args[0].get());
        if (!argName) return false;
        auto curType = impl_->lookup(argName->name);
        auto narrowT = narrowedTypeFromExpr(call->args[1].get());
        if (!curType || !narrowT) return false;
        if (curType->kind() == Type::Kind::Union) {
            outName = argName->name;
            outThenT = narrowT;
            outElseT = subtractFromUnion(curType, narrowT);
            return true;
        }
        if (curType->kind() == Type::Kind::Any) {
            outName = argName->name;
            outThenT = narrowT;
            outElseT = curType;
            return true;
        }
        return false;
    };

    if (node.condition) inferType(node.condition.get());

    std::string nName;
    std::shared_ptr<Type> nThen;
    std::shared_ptr<Type> nElse;
    bool narrowedHere = node.condition &&
                        analyzeIsinstance(node.condition.get(), nName, nThen, nElse);
    impl_->pushScope();
    if (narrowedHere) impl_->define(nName, nThen);
    for (auto& s : node.thenBody) s->accept(*this);
    impl_->popScope();

    for (auto& [cond, body] : node.elifClauses) {
        if (cond) inferType(cond.get());
        std::string en;
        std::shared_ptr<Type> et;
        std::shared_ptr<Type> ee;
        bool en_ok = cond && analyzeIsinstance(cond.get(), en, et, ee);
        impl_->pushScope();
        if (en_ok) impl_->define(en, et);
        for (auto& s : body) s->accept(*this);
        impl_->popScope();
    }

    impl_->pushScope();
    if (narrowedHere) impl_->define(nName, nElse ? nElse : impl_->unknownType);
    for (auto& s : node.elseBody) s->accept(*this);
    impl_->popScope();

    if (narrowedHere && node.elifClauses.empty() && node.elseBody.empty() &&
        stmtsAlwaysTerminate(node.thenBody)) {
        impl_->define(nName, nElse ? nElse : impl_->unknownType);
    }
}

void TypeChecker::visit(WhileStmt& node) {
    if (node.condition) inferType(node.condition.get());
    impl_->pushScope();
    for (auto& s : node.body) s->accept(*this);
    impl_->popScope();
    impl_->pushScope();
    for (auto& s : node.elseBody) s->accept(*this);
    impl_->popScope();
}

void TypeChecker::visit(ForStmt& node) {
    impl_->rangeValueOkExprs.insert(node.iterable.get());
    auto iterType = inferType(node.iterable.get());

    if (iterType && iterType->kind() == Type::Kind::Function) {
        if (auto* fn = dynamic_cast<NameExpr*>(node.iterable.get())) {
            error(node.location(), "cannot iterate the function '" + fn->name +
                  "'; call it first: 'for ... in " + fn->name + "()'");
        } else {
            error(node.location(), "cannot iterate a function value; call it first");
        }
    }

    impl_->pushScope();
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        if (iterType->kind() == Type::Kind::List) {
            impl_->define(name->name, static_cast<ListType&>(*iterType).elementType);
        } else if (iterType->kind() == Type::Kind::Set) {
            impl_->define(name->name, static_cast<SetType&>(*iterType).elementType);
        } else if (iterType->kind() == Type::Kind::Dict) {
            auto keyT = static_cast<DictType&>(*iterType).keyType;
            impl_->define(name->name, keyT ? keyT : impl_->unknownType);
        } else if (iterType->kind() == Type::Kind::Instance) {
            auto methodReturn = [&](const ClassType* cls, const std::string& m)
                -> std::shared_ptr<Type> {
                if (const ClassType* owner = findMethodOwner(cls, m)) {
                    auto it = owner->methods.find(m);
                    if (it != owner->methods.end() && it->second &&
                        it->second->kind() == Type::Kind::Function)
                        return static_cast<FunctionType&>(*it->second).returnType;
                }
                return nullptr;
            };
            std::shared_ptr<Type> bind = impl_->unknownType;
            const ClassType* iterCls =
                static_cast<InstanceType&>(*iterType).classType.get();
            if (auto iterRet = methodReturn(iterCls, "__iter__")) {
                const ClassType* nextCls = iterCls;
                if (iterRet->kind() == Type::Kind::Instance)
                    nextCls = static_cast<InstanceType&>(*iterRet).classType.get();
                else if (iterRet->kind() == Type::Kind::Class)
                    nextCls = static_cast<ClassType*>(iterRet.get());
                if (auto nextRet = methodReturn(nextCls, "__next__"))
                    bind = nextRet;
                else if (nextCls)
                    error(node.location(),
                          "cannot iterate: __iter__ returns '" + nextCls->name +
                          "', which has no __next__ method");
            }
            impl_->define(name->name, bind);
        } else {
            impl_->define(name->name, impl_->unknownType);
        }
    } else if (auto* tup = dynamic_cast<TupleExpr*>(node.target.get())) {
        std::shared_ptr<Type> elemT = impl_->unknownType;
        if (iterType->kind() == Type::Kind::List)
            elemT = static_cast<ListType&>(*iterType).elementType;
        const bool elemIsTuple = elemT && elemT->kind() == Type::Kind::Tuple;
        for (size_t i = 0; i < tup->elements.size(); i++) {
            auto* en = dynamic_cast<NameExpr*>(tup->elements[i].get());
            if (!en) continue;
            std::shared_ptr<Type> bind = impl_->unknownType;
            if (elemIsTuple) {
                auto& tt = static_cast<TupleType&>(*elemT);
                if (i < tt.elementTypes.size() && tt.elementTypes[i])
                    bind = tt.elementTypes[i];
            }
            impl_->define(en->name, bind);
        }
    }

    for (auto& s : node.body) s->accept(*this);
    impl_->popScope();
    impl_->pushScope();
    for (auto& s : node.elseBody) s->accept(*this);
    impl_->popScope();
}

void TypeChecker::visit(TryStmt& node) {
    impl_->pushScope();
    for (auto& s : node.tryBody) s->accept(*this);
    impl_->popScope();
    for (auto& handler : node.handlers) {
        impl_->pushScope();
        if (!handler.name.empty() && handler.type) {
            if (auto* named = dynamic_cast<NamedTypeExpr*>(handler.type.get())) {
                std::shared_ptr<Type> bind;
                auto it = impl_->typeNames.find(named->name);
                if (it != impl_->typeNames.end() &&
                    it->second->kind() == Type::Kind::Instance) {
                    bind = it->second;
                } else {
                    auto looked = impl_->lookup(named->name);
                    if (looked && looked->kind() == Type::Kind::Class) {
                        bind = std::make_shared<InstanceType>(
                            std::static_pointer_cast<ClassType>(looked));
                    }
                }
                if (!bind) {
                    static std::unordered_map<std::string,
                                              std::shared_ptr<ClassType>> cache;
                    auto& cls = cache[named->name];
                    if (!cls) cls = std::make_shared<ClassType>(named->name);
                    bind = std::make_shared<InstanceType>(cls);
                }
                impl_->define(handler.name, bind);
            }
        }
        for (auto& s : handler.body) s->accept(*this);
        impl_->popScope();
    }
    impl_->pushScope();
    for (auto& s : node.elseBody) s->accept(*this);
    impl_->popScope();
    impl_->pushScope();
    for (auto& s : node.finallyBody) s->accept(*this);
    impl_->popScope();
}

void TypeChecker::visit(WithStmt& node) {
    impl_->pushScope();
    for (auto& item : node.items) {
        std::shared_ptr<Type> ctxType = impl_->unknownType;
        if (item.contextExpr) ctxType = inferType(item.contextExpr.get());
        if (item.optionalVars) {
            if (auto* nm = dynamic_cast<NameExpr*>(item.optionalVars.get()))
                impl_->define(nm->name, ctxType);
        }
    }
    for (auto& s : node.body) s->accept(*this);
    impl_->popScope();
}

void TypeChecker::visit(ThreadStmt& node) {
    impl_->pushScope();
    for (auto& s : node.body) s->accept(*this);
    impl_->popScope();
}

void TypeChecker::visit(DeferStmt& node) {
    if (node.call) inferType(node.call.get());
}

void TypeChecker::visit(MatchStmt& node) {
    if (node.subject) inferType(node.subject.get());
    auto classFullFieldOrder =
        [](const std::shared_ptr<ClassType>& ct) -> std::vector<std::string> {
        std::vector<ClassType*> chain;
        ClassType* cur = ct.get();
        while (cur) {
            chain.push_back(cur);
            ClassType* par = nullptr;
            if (cur->parentClass) {
                if (auto pi = std::dynamic_pointer_cast<InstanceType>(cur->parentClass))
                    par = pi->classType.get();
                else if (auto pc = std::dynamic_pointer_cast<ClassType>(cur->parentClass))
                    par = pc.get();
            }
            cur = par;
        }
        std::vector<std::string> order;
        std::set<std::string> seen;
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit)
            for (auto& f : (*rit)->fieldOrder)
                if (seen.insert(f).second) order.push_back(f);
        return order;
    };

    std::function<void(MatchPattern&)> inferPatternTypes = [&](MatchPattern& pat) {
        if (pat.literal) inferType(pat.literal.get());
        if (pat.guard) inferType(pat.guard.get());
        if (pat.kind == MatchPattern::Kind::Class) {
            static const std::set<std::string> kPrimPatternTypes = {
                "int", "str", "float", "bool", "list", "dict",
                "tuple", "set", "bytes"};
            bool isPrim = kPrimPatternTypes.count(pat.name) > 0;
            bool isClass = impl_->typeNames.count(pat.name) > 0;
            if (!isPrim && !isClass)
                error(node.location(),
                      "unknown type '" + pat.name + "' in class pattern");

            if (!pat.subPatterns.empty()) {
                if (isPrim) {
                    error(node.location(), "`" + pat.name + "` has no positional "
                          "fields to destructure - write `case " + pat.name +
                          "()` as a type test");
                } else {
                    std::shared_ptr<ClassType> ct;
                    auto t = impl_->lookup(pat.name);
                    if (auto inst = std::dynamic_pointer_cast<InstanceType>(t))
                        ct = inst->classType;
                    else if (auto c = std::dynamic_pointer_cast<ClassType>(t))
                        ct = c;
                    std::vector<std::string> order =
                        ct ? classFullFieldOrder(ct) : std::vector<std::string>{};
                    if (pat.subPatterns.size() > order.size())
                        error(node.location(), "class pattern `" + pat.name +
                              "(...)` has " + std::to_string(pat.subPatterns.size()) +
                              " sub-patterns but `" + pat.name + "` has " +
                              std::to_string(order.size()) + " field(s)");
                    for (size_t i = 0; i < pat.subPatterns.size(); ++i) {
                        auto& sub = pat.subPatterns[i];
                        std::shared_ptr<Type> fieldT = impl_->anyType;
                        if (ct && i < order.size()) {
                            auto fit = ct->fields.find(order[i]);
                            if (fit != ct->fields.end() && fit->second)
                                fieldT = fit->second;
                        }
                        inferPatternTypes(sub);
                        if (sub.kind == MatchPattern::Kind::Capture && !sub.name.empty())
                            impl_->define(sub.name, fieldT);
                    }
                    return;
                }
            }
        }
        for (auto& sub : pat.subPatterns) inferPatternTypes(sub);
    };

    auto* subjName = dynamic_cast<NameExpr*>(node.subject.get());
    auto patternNarrowType = [&](const std::string& tn) -> std::shared_ptr<Type> {
        if (tn == "int")   return impl_->intType;
        if (tn == "float") return impl_->floatType;
        if (tn == "bool")  return impl_->boolType;
        if (tn == "str")   return impl_->strType;
        return nullptr;
    };

    for (auto& c : node.cases) {
        impl_->pushScope();
        inferPatternTypes(c.pattern);
        if (subjName && c.pattern.kind == MatchPattern::Kind::Class &&
            c.pattern.subPatterns.empty()) {
            if (auto narrowed = patternNarrowType(c.pattern.name))
                impl_->define(subjName->name, narrowed);
        }
        if (c.guard) inferType(c.guard.get());
        for (auto& s : c.body) s->accept(*this);
        impl_->popScope();
    }

    auto memberMatchName = [](Type* t) -> std::string {
        if (!t) return "";
        switch (t->kind()) {
            case Type::Kind::Int:   return "int";
            case Type::Kind::Float: return "float";
            case Type::Kind::Bool:  return "bool";
            case Type::Kind::Str:   return "str";
            case Type::Kind::Bytes: return "bytes";
            case Type::Kind::List:  return "list";
            case Type::Kind::Dict:  return "dict";
            case Type::Kind::Tuple: return "tuple";
            case Type::Kind::Set:   return "set";
            case Type::Kind::Instance:
                return static_cast<InstanceType*>(t)->classType
                     ? static_cast<InstanceType*>(t)->classType->name : "";
            case Type::Kind::Class: return static_cast<ClassType*>(t)->name;
            default: return "";
        }
    };

    std::set<std::string> coveredTypes;
    std::set<int64_t>     coveredInts;
    std::set<std::string> coveredStrs;
    bool coveredNone = false, coveredTrue = false, coveredFalse = false;
    int catchAllIdx = -1;

    std::function<bool(const MatchPattern&)> isIrrefutable =
        [&](const MatchPattern& p) -> bool {
        if (p.kind == MatchPattern::Kind::Wildcard ||
            p.kind == MatchPattern::Kind::Capture)
            return true;
        if (p.kind == MatchPattern::Kind::Class) {
            for (auto& s : p.subPatterns)
                if (!isIrrefutable(s)) return false;
            return true;
        }
        return false;
    };
    auto collectTypeTest = [&](const MatchPattern& p) {
        if (p.kind == MatchPattern::Kind::Class && isIrrefutable(p))
            coveredTypes.insert(p.name);
    };

    for (size_t i = 0; i < node.cases.size(); ++i) {
        auto& c = node.cases[i];
        auto& pat = c.pattern;
        bool guarded = (c.guard != nullptr) || (pat.guard != nullptr);

        if (catchAllIdx >= 0)
            error(node.location(),
                  "unreachable case: a previous catch-all (`case _` or a bare "
                  "capture) already matches every value");

        if (!guarded) {
            collectTypeTest(pat);
            if (pat.kind == MatchPattern::Kind::Or)
                for (auto& sub : pat.subPatterns) collectTypeTest(sub);
        }

        if (pat.kind == MatchPattern::Kind::Literal && pat.literal) {
            Expr* lit = pat.literal.get();
            if (!guarded && dynamic_cast<NoneLiteral*>(lit)) coveredNone = true;
            else if (auto* b = dynamic_cast<BooleanLiteral*>(lit)) {
                if (!guarded) { if (b->value) coveredTrue = true; else coveredFalse = true; }
            } else if (auto* in = dynamic_cast<IntegerLiteral*>(lit)) {
                if (!coveredInts.insert(in->value).second)
                    error(node.location(), "duplicate case literal '" +
                          std::to_string(in->value) +
                          "' - the second is unreachable");
            } else if (auto* s = dynamic_cast<StringLiteral*>(lit)) {
                if (!coveredStrs.insert(s->value).second)
                    error(node.location(),
                          "duplicate case string literal - the second is unreachable");
            }
        }

        if (!guarded && (pat.kind == MatchPattern::Kind::Wildcard ||
                         pat.kind == MatchPattern::Kind::Capture))
            catchAllIdx = (int)i;
    }

    auto subjType = node.subject ? node.subject->type : nullptr;
    if (catchAllIdx < 0 && subjType) {
        if (subjType->kind() == Type::Kind::Union) {
            auto& members = static_cast<UnionType&>(*subjType).types;
            std::vector<std::string> missing;
            for (auto& m : members) {
                if (m->kind() == Type::Kind::None_) {
                    if (!coveredNone) missing.push_back("`None` (add `case None`)");
                    continue;
                }
                std::string mn = memberMatchName(m.get());
                if (mn.empty() || !coveredTypes.count(mn))
                    missing.push_back("`" + m->toString() + "` (add `case " +
                        (mn.empty() ? "_" : mn + "()") + "`)");
            }
            if (!missing.empty()) {
                std::string msg = "non-exhaustive match on `" +
                    subjType->toString() + "`: no case for ";
                for (size_t j = 0; j < missing.size(); ++j) {
                    if (j) msg += ", ";
                    msg += missing[j];
                }
                msg += "; cover it or add `case _`";
                error(node.location(), msg);
            }
        } else if (subjType->kind() == Type::Kind::Bool) {
            if (!((coveredTrue && coveredFalse) || coveredTypes.count("bool")))
                error(node.location(),
                      "non-exhaustive match on `bool`: cover both `True` and "
                      "`False`, or add `case _`");
        }
    }
}

void TypeChecker::visit(ReturnStmt& node) {
    if (node.value) {
        auto retType = inferType(node.value.get());
        if (!impl_->returnTypeStack.empty()) {
            auto& expected = impl_->returnTypeStack.back();
            if (annotationElementIsAny(expected))
                tryExpectedTypeLiteral(node.value.get(), expected);
            if (expected->kind() != Type::Kind::Unknown &&
                retType->kind() != Type::Kind::Unknown &&
                !retType->isAssignableTo(*expected) &&
                !tryExpectedTypeLiteral(node.value.get(), expected)) {
                error(node.location(), "return type '" + retType->toString() +
                      "' does not match declared return type '" +
                      expected->toString() + "'" +
                      listReprMismatchHint(*retType, *expected));
            }
        }
    } else {
        if (!impl_->returnTypeStack.empty()) {
            auto& expected = impl_->returnTypeStack.back();
            if (expected->kind() != Type::Kind::Unknown &&
                expected->kind() != Type::Kind::None_) {
                error(node.location(), "return without value in function returning '" +
                      expected->toString() + "'");
            }
        }
    }
}

void TypeChecker::visit(RaiseStmt& node) {
    if (node.exception) inferType(node.exception.get());
    if (node.cause) {
        error(node.cause->location(),
              "exception chaining with 'from' is not supported: the cause is not "
              "retained and cannot be read back. Raise the new exception without 'from'.");
    }
}

void TypeChecker::visit(BreakStmt&) {}
void TypeChecker::visit(ContinueStmt&) {}
void TypeChecker::visit(PassStmt&) {}

void TypeChecker::visit(AssertStmt& node) {
    if (node.test) inferType(node.test.get());
    if (node.msg) inferType(node.msg.get());
}

void TypeChecker::visit(GlobalStmt&) {}
void TypeChecker::visit(NonlocalStmt&) {}

void TypeChecker::visit(DeleteStmt& node) {
    for (auto& t : node.targets) inferType(t.get());
}

void TypeChecker::visit(ImportStmt& node) {
    for (auto& alias : node.names) {
        if (!alias.asName.empty()) {
            auto mt = impl_->getOrCreateModuleType(alias.name);
            impl_->define(alias.asName, mt);
        } else {
            auto dot = alias.name.find('.');
            std::string topName = (dot == std::string::npos)
                ? alias.name
                : alias.name.substr(0, dot);
            impl_->getOrCreateModuleType(alias.name);
            auto topMt = impl_->getOrCreateModuleType(topName);
            impl_->define(topName, topMt);
        }
    }
}

void TypeChecker::visit(FromImportStmt& node) {
    auto modIt = impl_->moduleTypes.find(node.module);
    if (modIt == impl_->moduleTypes.end()) return;

    auto& srcModule = *modIt->second;
    for (auto& alias : node.names) {
        std::string defName = alias.asName.empty() ? alias.name : alias.asName;
        checkModuleNamePrivacy(srcModule, alias.name, node.location());
        auto subIt = srcModule.submodules.find(alias.name);
        if (subIt != srcModule.submodules.end()) {
            impl_->define(defName, subIt->second);
            continue;
        }
        auto symIt = srcModule.exports.find(alias.name);
        if (symIt != srcModule.exports.end()) {
            if (symIt->second && symIt->second->kind() == Type::Kind::Lock) {
                if (!alias.asName.empty() && alias.asName != alias.name) {
                    error(node.location(),
                          "cannot alias intrinsic '" + alias.name +
                          "' on import; use 'from " + node.module +
                          " import " + alias.name + "'");
                    continue;
                }
                impl_->define(defName, std::make_shared<FunctionType>(
                    std::vector<std::shared_ptr<Type>>{}, symIt->second));
                impl_->typeNames[defName] = symIt->second;
                continue;
            }
            impl_->define(defName, symIt->second);
            if (symIt->second && symIt->second->kind() == Type::Kind::Class) {
                auto cls = std::static_pointer_cast<ClassType>(symIt->second);
                impl_->typeNames[defName] = std::make_shared<InstanceType>(cls);
            }
            continue;
        }
        if (node.module == "collections" && alias.name == "deque") {
            auto tnIt = impl_->typeNames.find("deque");
            if (tnIt != impl_->typeNames.end())
                impl_->typeNames[defName] = tnIt->second;
            continue;
        }
        error(node.location(),
              "cannot import name '" + alias.name + "' from module '" + node.module + "'");
    }
}

void fillFuncMeta(FunctionType& ft, const std::vector<Parameter>& params,
                  bool isMethod, bool hasImplicitSelf,
                  bool isClassMethod) {
    ft.paramNames.clear();
    ft.paramOwns.clear();
    ft.requiredParams = 0;
    ft.hasVarArg = false;
    ft.hasKwArg = false;
    ft.hasArgMeta = true;
    ft.isMethod = isMethod;
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        if (isMethod && !hasImplicitSelf && i == 0 &&
            (p.name == "self" || (isClassMethod && p.name == "cls")))
            continue;
        if (p.isVarArg || p.isKwArg) {
            ft.hasVarArg = true;
            if (p.isKwArg) ft.hasKwArg = true;
            continue;
        }
        ft.paramNames.push_back(p.name);
        ft.paramOwns.push_back(p.isOwn);
        if (!p.defaultValue) ft.requiredParams++;
    }
}

void TypeChecker::visit(FunctionDecl& node) {
    if (!node.typeParams.empty() && impl_->genericChecked.count(&node)) return;
    bool pushedTP = !node.typeParams.empty();
    if (pushedTP) {
        std::unordered_map<std::string, std::shared_ptr<Type>> frame;
        for (auto& tp : node.typeParams) {
            std::shared_ptr<Type> bnd =
                tp.bound ? resolveType(tp.bound.get()) : nullptr;
            frame[tp.name] = std::make_shared<TypeVarType>(tp.name, bnd);
        }
        impl_->typeParamScopes.push_back(std::move(frame));
        impl_->genericTemplateDepth++;
    }

    std::vector<std::shared_ptr<Type>> paramTypes;
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (node.isMethod && !node.hasImplicitSelf && node.params[i].name == "self")
            continue;
        paramTypes.push_back(resolveType(node.params[i].type.get()));
    }
    auto retType = resolveType(node.returnType.get());
    if (node.isAsync && node.isClassMethod) {
        error(node.location(),
              "an async @classmethod is not supported; make it an async "
              "instance method or a module-level async function");
    }
    if (node.isAsync && retType &&
        (retType->kind() == Type::Kind::Any ||
         retType->kind() == Type::Kind::Union)) {
        error(node.location(), "an async function cannot return '" +
              retType->toString() +
              "': a task result crosses the spawn boundary monomorphized; "
              "annotate the concrete return type");
    }
    auto externalRet = node.isAsync ? std::static_pointer_cast<Type>(
                                          std::make_shared<TaskType>(retType))
                                    : retType;
    auto funcType = std::make_shared<FunctionType>(paramTypes, externalRet);
    funcType->spawnsFreshTask = node.isAsync;
    fillFuncMeta(*funcType, node.params, node.isMethod, node.hasImplicitSelf,
                 node.isClassMethod);

    impl_->define(node.name, funcType);

    impl_->pushScope();
    impl_->returnTypeStack.push_back(retType);

    if (node.isMethod && node.hasImplicitSelf) {
        auto selfType = impl_->lookup("self");
        if (selfType) impl_->define("self", selfType);
    }

    size_t typeIdx = 0;
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (node.isMethod && !node.hasImplicitSelf && node.params[i].name == "self")
            continue;
        auto pt = paramTypes[typeIdx++];
        if (node.params[i].isVarArg)
            pt = std::make_shared<ListType>(pt);
        else if (node.params[i].isKwArg)
            pt = std::make_shared<DictType>(impl_->strType, pt);
        impl_->define(node.params[i].name, pt);
    }

    for (auto& s : node.body) {
        s->accept(*this);
    }

    impl_->returnTypeStack.pop_back();
    impl_->popScope();
    if (pushedTP) { impl_->typeParamScopes.pop_back(); impl_->genericTemplateDepth--; }
}

void TypeChecker::visit(ClassDecl& node) {
    if (!node.typeParams.empty() && impl_->genericChecked.count(&node)) return;
    for (auto& base : node.bases) {
        if (dynamic_cast<SubscriptExpr*>(base.get())) {
            error(node.location(), "subclassing a generic instantiation (e.g. "
                  "`class " + node.name + "(Base[...])`) is not yet supported; "
                  "v1 generics cover generic free functions and generic classes");
            break;
        }
    }
    bool pushedTP = !node.typeParams.empty();
    if (pushedTP) {
        std::unordered_map<std::string, std::shared_ptr<Type>> frame;
        for (auto& tp : node.typeParams) {
            std::shared_ptr<Type> bnd =
                tp.bound ? resolveType(tp.bound.get()) : nullptr;
            frame[tp.name] = std::make_shared<TypeVarType>(tp.name, bnd);
        }
        impl_->typeParamScopes.push_back(std::move(frame));
        impl_->genericTemplateDepth++;
    }
    visitClassDeclBody(node);
    if (pushedTP) { impl_->typeParamScopes.pop_back(); impl_->genericTemplateDepth--; }
}

void TypeChecker::visitClassDeclBody(ClassDecl& node) {
    std::shared_ptr<ClassType> classType;
    if (auto tnIt = impl_->typeNames.find(node.name); tnIt != impl_->typeNames.end()) {
        if (auto inst = std::dynamic_pointer_cast<InstanceType>(tnIt->second))
            classType = inst->classType;
    }
    if (!classType) classType = std::make_shared<ClassType>(node.name);

    classType->definingModule = impl_->currentModuleName;
    classType->definingFile = impl_->currentFile;
    classType->decl = &node;

    for (auto& s : node.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(s.get()))
            checkDunderDeclaration(func->name, false, node.name,
                                   func->location());
        else if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get()))
            if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                checkDunderDeclaration(tgt->name, false, node.name,
                                       ann->location());
    }

    std::shared_ptr<Type> enumValueType = impl_->intType;
    for (auto& base : node.bases) {
        if (auto* baseName = dynamic_cast<NameExpr*>(base.get())) {
            if (baseName->name == "TypedDict") {
                classType->isTypedDict = true;
                continue;
            }
            if (baseName->name == "Enum" || baseName->name == "IntEnum" ||
                baseName->name == "StrEnum") {
                classType->isEnum = true;
                if (baseName->name == "StrEnum") enumValueType = impl_->strType;
                continue;
            }
        }
        if (classType->parentClass) continue;
        std::shared_ptr<Type> baseType;
        if (auto* baseName = dynamic_cast<NameExpr*>(base.get())) {
            baseType = impl_->lookup(baseName->name);
            if (!baseType) {
                auto tit = impl_->typeNames.find(baseName->name);
                if (tit != impl_->typeNames.end()) baseType = tit->second;
            }
        } else if (dynamic_cast<AttributeExpr*>(base.get())) {
            baseType = inferType(base.get());
        }
        if (!baseType) continue;
        if (baseType->kind() == Type::Kind::Class) {
            classType->parentClass = baseType;
        } else if (baseType->kind() == Type::Kind::Instance) {
            classType->parentClass =
                std::static_pointer_cast<InstanceType>(baseType)->classType;
        }
    }

    impl_->typeNames[node.name] = std::make_shared<InstanceType>(classType);
    impl_->define(node.name, classType);

    impl_->pushScope();

    impl_->define("self", std::make_shared<InstanceType>(classType));

    const ClassType* prevClass = impl_->currentClass;
    impl_->currentClass = classType.get();

    if (!classType->isTypedDict) {
        for (auto& s : node.body) {
            auto* ann = dynamic_cast<AnnAssignStmt*>(s.get());
            if (!ann || !ann->annotation) continue;
            auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
            if (!tgt || classType->fields.count(tgt->name)) continue;
            classType->fields[tgt->name] = resolveType(ann->annotation.get());
        }
    }

    if (classType->isEnum) {
        auto selfInstance = std::make_shared<InstanceType>(classType);
        for (auto& s : node.body) {
            auto* ann = dynamic_cast<AnnAssignStmt*>(s.get());
            if (!ann || ann->isStatic) continue;
            auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
            if (!tgt) continue;
            classType->fields[tgt->name] = selfInstance;
        }
        classType->fields["name"] = impl_->strType;
        classType->fields["value"] = enumValueType;
    }

    classType->constructorCount = 0;
    classType->methodOverloads.clear();
    std::unordered_map<std::string, int> _ovlCount;
    for (auto& s : node.body) {
        auto* f = dynamic_cast<FunctionDecl*>(s.get());
        if (!f || !f->typeParams.empty() || f->isProperty) continue;
        if (f->name == "__init__" || f->isConstructor) continue;
        _ovlCount[f->name]++;
    }
    std::unordered_map<std::string, int> _ovlNext;
    for (auto& s : node.body) {
        auto* func = dynamic_cast<FunctionDecl*>(s.get());
        if (!func) continue;
        if (!func->typeParams.empty()) continue;
        if (func->name == "__init__" || func->isConstructor)
            classType->constructorCount++;
        std::vector<std::shared_ptr<Type>> paramTypes;
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (func->isMethod && !func->hasImplicitSelf &&
                (func->params[i].name == "self" ||
                 (func->isClassMethod && func->params[i].name == "cls")))
                continue;
            paramTypes.push_back(resolveType(func->params[i].type.get()));
        }
        auto retType = resolveType(func->returnType.get());
        auto externalRet = func->isAsync ? std::static_pointer_cast<Type>(
                                              std::make_shared<TaskType>(retType))
                                         : retType;
        auto fType = std::make_shared<FunctionType>(paramTypes, externalRet);
        fType->spawnsFreshTask = func->isAsync;
        fillFuncMeta(*fType, func->params, func->isMethod, func->hasImplicitSelf,
                     func->isClassMethod);
        if (func->isProperty) {
            if (!classType->fields.count(func->name))
                classType->fields[func->name] = retType;
        } else {
            classType->methods[func->name] = fType;
            if (func->name != "__init__" && !func->isConstructor) {
                int cnt = _ovlCount[func->name];
                func->methodOverloadCount = cnt;
                func->methodOverloadIndex = (cnt > 1) ? _ovlNext[func->name]++ : -1;
                if (cnt > 1)
                    classType->methodOverloads[func->name].push_back(fType);
            }
        }
    }

    for (auto& s : node.body) {
        s->accept(*this);

        if (auto* func = dynamic_cast<FunctionDecl*>(s.get())) {
            auto fType = impl_->lookup(func->name);
            if (fType) {
                if (func->isProperty) {
                    classType->fields[func->name] = resolveType(func->returnType.get());
                } else {
                    classType->methods[func->name] = fType;
                }
            }
            if (func->name == "__init__" || func->isConstructor) {
                std::unordered_map<std::string, std::shared_ptr<Type>> paramTypes;
                for (auto& p : func->params) {
                    if (!p.name.empty() && p.type) {
                        paramTypes[p.name] = resolveType(p.type.get());
                    }
                }
                std::function<std::shared_ptr<Type>(Expr*)> rhsLiteralType =
                    [&](Expr* rhs) -> std::shared_ptr<Type> {
                    if (!rhs) return nullptr;
                    if (rhs->type && rhs->type->kind() != Type::Kind::Unknown) {
                        return rhs->type;
                    }
                    if (dynamic_cast<StringLiteral*>(rhs)) return impl_->strType;
                    if (dynamic_cast<IntegerLiteral*>(rhs)) return impl_->intType;
                    if (dynamic_cast<FloatLiteral*>(rhs)) return impl_->floatType;
                    if (dynamic_cast<BooleanLiteral*>(rhs)) return impl_->boolType;
                    if (dynamic_cast<NoneLiteral*>(rhs)) return impl_->noneType;
                    if (auto* le = dynamic_cast<ListExpr*>(rhs)) {
                        if (le->elements.empty())
                            return std::make_shared<ListType>(impl_->anyType);
                        auto first = rhsLiteralType(le->elements[0].get());
                        if (!first) return std::make_shared<ListType>(impl_->anyType);
                        for (size_t i = 1; i < le->elements.size(); ++i) {
                            auto t = rhsLiteralType(le->elements[i].get());
                            if (!t || t->kind() != first->kind())
                                return std::make_shared<ListType>(impl_->anyType);
                        }
                        return std::make_shared<ListType>(first);
                    }
                    if (auto* de = dynamic_cast<DictExpr*>(rhs)) {
                        if (de->entries.empty())
                            return std::make_shared<DictType>(impl_->anyType, impl_->anyType);
                        auto firstK = rhsLiteralType(de->entries[0].first.get());
                        auto firstV = rhsLiteralType(de->entries[0].second.get());
                        if (!firstK || !firstV)
                            return std::make_shared<DictType>(impl_->anyType, impl_->anyType);
                        for (size_t i = 1; i < de->entries.size(); ++i) {
                            auto kt = rhsLiteralType(de->entries[i].first.get());
                            auto vt = rhsLiteralType(de->entries[i].second.get());
                            if (!kt || kt->kind() != firstK->kind())
                                firstK = impl_->anyType;
                            if (!vt || vt->kind() != firstV->kind())
                                firstV = impl_->anyType;
                        }
                        return std::make_shared<DictType>(firstK, firstV);
                    }
                    if (auto* n = dynamic_cast<NameExpr*>(rhs)) {
                        auto it = paramTypes.find(n->name);
                        if (it != paramTypes.end()) return it->second;
                    }
                    if (auto* ce = dynamic_cast<CallExpr*>(rhs)) {
                        if (auto* cn = dynamic_cast<NameExpr*>(ce->callee.get())) {
                            if (cn->name == "bytes") return impl_->bytesType;
                        }
                    }
                    return nullptr;
                };
                std::function<void(Stmt*)> walk = [&](Stmt* st) {
                    if (!st) return;
                    if (auto* as = dynamic_cast<AssignStmt*>(st)) {
                        for (auto& t : as->targets) {
                            auto* attr = dynamic_cast<AttributeExpr*>(t.get());
                            if (!attr) continue;
                            auto* obj = dynamic_cast<NameExpr*>(attr->object.get());
                            if (!obj || obj->name != "self") continue;
                            if (classType->fields.count(attr->attribute)) continue;
                            if (classType->methods.count(attr->attribute) ||
                                classType->methodOverloads.count(attr->attribute))
                                continue;
                            auto rhsType = rhsLiteralType(as->value.get());
                            if (rhsType) {
                                classType->fields[attr->attribute] = rhsType;
                            }
                        }
                    } else if (auto* ann = dynamic_cast<AnnAssignStmt*>(st)) {
                        auto* attr = dynamic_cast<AttributeExpr*>(ann->target.get());
                        if (attr) {
                            auto* obj = dynamic_cast<NameExpr*>(attr->object.get());
                            if (obj && obj->name == "self" &&
                                !classType->fields.count(attr->attribute) &&
                                !classType->methods.count(attr->attribute) &&
                                !classType->methodOverloads.count(attr->attribute)) {
                                classType->fields[attr->attribute] =
                                    resolveType(ann->annotation.get());
                            }
                        }
                    } else if (auto* ifs = dynamic_cast<IfStmt*>(st)) {
                        for (auto& s2 : ifs->thenBody) walk(s2.get());
                        for (auto& [_, body] : ifs->elifClauses)
                            for (auto& s2 : body) walk(s2.get());
                        for (auto& s2 : ifs->elseBody) walk(s2.get());
                    } else if (auto* w = dynamic_cast<WhileStmt*>(st)) {
                        for (auto& s2 : w->body) walk(s2.get());
                    } else if (auto* f = dynamic_cast<ForStmt*>(st)) {
                        for (auto& s2 : f->body) walk(s2.get());
                    } else if (auto* tr = dynamic_cast<TryStmt*>(st)) {
                        for (auto& s2 : tr->tryBody) walk(s2.get());
                        for (auto& h : tr->handlers)
                            for (auto& s2 : h.body) walk(s2.get());
                    }
                };
                for (auto& s2 : func->body) walk(s2.get());
            }
        }

        if (classType->isTypedDict) {
            if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get())) {
                if (auto* fieldName = dynamic_cast<NameExpr*>(ann->target.get())) {
                    auto fieldType = resolveType(ann->annotation.get());
                    classType->fields[fieldName->name] = fieldType;
                }
            }
        }
    }

    for (auto& pname : node.promises) {
        auto ct = resolveContractRef(pname, node.location(), true);
        if (!ct) continue;
        auto problems = contractConformanceProblems(*classType, *ct);
        if (!problems.empty()) {
            std::string msg = "class '" + node.name + "' promises contract '" +
                              pname + "' but does not satisfy it";
            for (auto& p : problems) msg += "; " + p;
            error(node.location(), msg);
            continue;
        }
        for (auto* a : ct->atoms) {
            classType->promisedContracts.insert(a);
            if (std::find(node.conformedContracts.begin(),
                          node.conformedContracts.end(),
                          a) == node.conformedContracts.end())
                node.conformedContracts.push_back(a);
        }
    }

    impl_->currentClass = prevClass;
    impl_->popScope();
}

void TypeChecker::visit(ContractDecl&) {}

void TypeChecker::visit(TypeAliasStmt& node) {
    if (!node.value) return;
    auto resolved = resolveType(node.value.get());
    if (resolved) impl_->typeNames[node.name] = resolved;
}

void TypeChecker::visit(Module& node) {
    registerContracts(node);
    for (auto& stmt : node.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            std::shared_ptr<ClassType> classType;
            if (impl_->typeNames.find(cd->name) == impl_->typeNames.end()) {
                classType = std::make_shared<ClassType>(cd->name);
                impl_->typeNames[cd->name] = std::make_shared<InstanceType>(classType);
                impl_->define(cd->name, classType);
            } else if (auto inst = std::dynamic_pointer_cast<InstanceType>(
                           impl_->typeNames[cd->name])) {
                classType = inst->classType;
            }
            if (classType && classType->definingFile.empty()) {
                classType->definingModule = impl_->currentModuleName;
                classType->definingFile = impl_->currentFile;
            }
            if (classType && !classType->decl) classType->decl = cd;
        }
    }

    for (auto& stmt : node.body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        if (!cd) continue;
        std::shared_ptr<ClassType> ct;
        if (auto it = impl_->typeNames.find(cd->name); it != impl_->typeNames.end()) {
            if (auto inst = std::dynamic_pointer_cast<InstanceType>(it->second))
                ct = inst->classType;
        }
        if (!ct) continue;

        ct->fieldOrder = instanceFieldOrder(*cd);

        for (auto& s : cd->body) {
            auto* func = dynamic_cast<FunctionDecl*>(s.get());
            if (!func || func->isProperty) continue;
            size_t nparams = 0;
            for (auto& p : func->params)
                if (!(func->isMethod && !func->hasImplicitSelf &&
                      (p.name == "self" || (func->isClassMethod && p.name == "cls"))))
                    ++nparams;
            std::vector<std::shared_ptr<Type>> ps(nparams, impl_->anyType);
            ct->methods[func->name] = std::make_shared<FunctionType>(ps, impl_->anyType);
        }

        for (auto& s : cd->body) {
            auto* ann = dynamic_cast<AnnAssignStmt*>(s.get());
            if (!ann || !ann->annotation) continue;
            if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                ct->declaredFieldNames.insert(tgt->name);
        }
        std::function<void(Stmt*)> collectNames = [&](Stmt* st) {
            if (!st) return;
            auto selfTarget = [&](Expr* e) {
                if (auto* attr = dynamic_cast<AttributeExpr*>(e))
                    if (auto* obj = dynamic_cast<NameExpr*>(attr->object.get()))
                        if (obj->name == "self")
                            ct->declaredFieldNames.insert(attr->attribute);
            };
            if (auto* as = dynamic_cast<AssignStmt*>(st)) {
                for (auto& t : as->targets) selfTarget(t.get());
            } else if (auto* ann = dynamic_cast<AnnAssignStmt*>(st)) {
                selfTarget(ann->target.get());
            } else if (auto* aug = dynamic_cast<AugAssignStmt*>(st)) {
                selfTarget(aug->target.get());
            } else if (auto* ifs = dynamic_cast<IfStmt*>(st)) {
                for (auto& s2 : ifs->thenBody) collectNames(s2.get());
                for (auto& [_, body] : ifs->elifClauses)
                    for (auto& s2 : body) collectNames(s2.get());
                for (auto& s2 : ifs->elseBody) collectNames(s2.get());
            } else if (auto* w = dynamic_cast<WhileStmt*>(st)) {
                for (auto& s2 : w->body) collectNames(s2.get());
            } else if (auto* f = dynamic_cast<ForStmt*>(st)) {
                for (auto& s2 : f->body) collectNames(s2.get());
            } else if (auto* tr = dynamic_cast<TryStmt*>(st)) {
                for (auto& s2 : tr->tryBody) collectNames(s2.get());
                for (auto& h : tr->handlers)
                    for (auto& s2 : h.body) collectNames(s2.get());
            } else if (auto* ws = dynamic_cast<WithStmt*>(st)) {
                for (auto& s2 : ws->body) collectNames(s2.get());
            }
        };
        for (auto& s : cd->body) {
            if (auto* func = dynamic_cast<FunctionDecl*>(s.get()))
                for (auto& s2 : func->body) collectNames(s2.get());
        }
    }

    for (auto& stmt : node.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get()))
            checkDunderDeclaration(func->name, true, "", func->location());
        else if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get()))
            checkDunderDeclaration(cd->name, true, "", cd->location());
        else if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                checkDunderDeclaration(tgt->name, true, "", ann->location());
        } else if (auto* as = dynamic_cast<AssignStmt*>(stmt.get())) {
            for (auto& t : as->targets)
                if (auto* tgt = dynamic_cast<NameExpr*>(t.get()))
                    checkDunderDeclaration(tgt->name, true, "", as->location());
        }
    }

    auto isImport = [](Stmt* s) {
        return dynamic_cast<ImportStmt*>(s) || dynamic_cast<FromImportStmt*>(s);
    };
    for (auto& stmt : node.body)
        if (isImport(stmt.get())) stmt->accept(*this);

    collectGenericTemplates(node);

    for (auto& stmt : node.body) {
        if (isImport(stmt.get())) continue;
        stmt->accept(*this);
    }
}

}
