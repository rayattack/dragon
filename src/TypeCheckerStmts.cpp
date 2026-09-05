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
#include <unordered_set>

namespace dragon {

static bool annotationElementIsAny(const std::shared_ptr<Type>& t) {
    if (!t) return false;
    if (auto* lt = dynamic_cast<ListType*>(t.get()))
        return lt->elementType && lt->elementType->kind() == Type::Kind::Boxed;
    if (auto* dt = dynamic_cast<DictType*>(t.get()))
        return dt->valueType && dt->valueType->kind() == Type::Kind::Boxed;
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
    bool listLike = base->name == "list" || base->name == "set";
    if (listLike && g->typeArgs.size() == 1)
        return isTypeArg(g->typeArgs[0].get());
    if (base->name == "dict" && g->typeArgs.size() == 2)
        return isTypeArg(g->typeArgs[1].get());
    return false;
}

bool TypeChecker::diagnoseHeterogeneousLiteral(
    Expr* value, const std::shared_ptr<Type>& annot) {
    if (!value || !annot) return false;
    if (annot->kind() != Type::Kind::List) return false;
    const auto& elemT = static_cast<const ListType&>(*annot).elementType;
    if (!elemT || elemT->kind() == Type::Kind::Boxed ||
        elemT->kind() == Type::Kind::Unknown)
        return false;

    const std::vector<std::unique_ptr<Expr>>* elems = nullptr;
    if (auto* lit = dynamic_cast<ListExpr*>(value)) elems = &lit->elements;
    else if (auto* st = dynamic_cast<SetExpr*>(value)) elems = &st->elements;
    if (!elems) return false;

    for (auto& e : *elems) {
        if (e->type && e->type->kind() != Type::Kind::Unknown &&
            !e->type->isSubtypeOf(*elemT)) {
            // A nested literal still fits if the element type is a union with
            // a matching container arm (how list[Data] holds {"k": 3}).
            if (elemT->kind() == Type::Kind::Union &&
                coerceLiteralToUnion(e.get(), elemT))
                continue;
            // A container LITERAL element is built in the expected layout from
            // birth (section 7), so `list[dict[str, Data]] = [{"k": 1}]` is
            // fine even though dict[str, int] is not a dict[str, Data].
            if ((elemT->kind() == Type::Kind::List ||
                 elemT->kind() == Type::Kind::Dict ||
                 elemT->kind() == Type::Kind::Tuple) &&
                tryExpectedTypeLiteral(e.get(), elemT))
                continue;
            error(value->location(),
                  "list element of type '" + e->type->toString() +
                  "' is not assignable to element type '" + elemT->toString() +
                  "'");
            return true;
        }
    }
    return false;
}

void TypeChecker::checkSubscriptSlotStore(AssignStmt& node,
                                          const std::shared_ptr<Type>& slot) {
    if (!node.value || !slot) return;
    if (slot->kind() == Type::Kind::Boxed) {
        boxNestedContainerLiteralForAny(node.value.get());
        return;
    }
    if (slot->kind() != Type::Kind::Union) return;
    propagateAnnotationToEmptyLiteral(node.value.get(), slot);
    auto valueType = inferType(node.value.get());
    if (tryExpectedTypeLiteral(node.value.get(), slot)) return;
    if (!valueType || valueType->kind() == Type::Kind::Unknown) return;
    if (valueType->isAssignableTo(*slot)) return;
    const std::string hint = listReprMismatchHint(*valueType, *slot);
    if (hint.empty()) return;
    error(node.location(),
          "cannot store '" + valueType->toString() + "' into a slot of type '" +
          slot->toString() + "'" + hint);
}

void TypeChecker::markNarrowTarget(Expr& value,
                                   const std::shared_ptr<Type>& want) {
    if (!want || !value.type || value.type->kind() != Type::Kind::Boxed) return;
    switch (want->kind()) {
        case Type::Kind::Boxed:
        case Type::Kind::Unknown:
        case Type::Kind::Union:
        case Type::Kind::Optional:
        case Type::Kind::TypeVar:
        case Type::Kind::Never:
            return;
        default:
            value.narrowTo = want;
    }
}

bool TypeChecker::tryExpectedTypeLiteral(Expr* value, const std::shared_ptr<Type>& expected) {
    if (!value || !expected) return false;
    // A container literal written straight into a union-typed slot
    // (`t: Tree = [1, [2, 3]]`) matches against the union's container arm.
    if (expected->kind() == Type::Kind::Union)
        return coerceLiteralToUnion(value, expected);
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
            if (el->type->isSubtypeOf(*base)) continue;
            if (base->kind() == Type::Kind::Union &&
                coerceLiteralToUnion(el.get(), base))
                continue;
            // A nested container literal is built at the element type.
            if ((base->kind() == Type::Kind::List ||
                 base->kind() == Type::Kind::Dict ||
                 base->kind() == Type::Kind::Tuple) &&
                tryExpectedTypeLiteral(el.get(), base))
                continue;
            return false;
        }
        for (auto& el : lit->elements) markNarrowTarget(*el, base);
        if (base->kind() == Type::Kind::Boxed) {
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
                if (!v->type) return false;
                if (v->type->isSubtypeOf(*dt.valueType)) continue;
                if (dt.valueType && dt.valueType->kind() == Type::Kind::Union &&
                    coerceLiteralToUnion(v.get(), dt.valueType))
                    continue;
                return false;
            }
        }
        for (auto& [k, v] : lit->entries)
            if (v) markNarrowTarget(*v, dt.valueType);
        if (dt.valueType && dt.valueType->kind() == Type::Kind::Boxed) {
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
            markNarrowTarget(*lit->elements[i], want);
            if (want->kind() == Type::Kind::Boxed) {
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
        value->type = std::make_shared<ListType>(impl_->boxedType);
    } else if (auto* d = dynamic_cast<DictExpr*>(value)) {
        for (auto& [k, v] : d->entries)
            if (v) boxNestedContainerLiteralForAny(v.get());
        std::shared_ptr<Type> keyT;
        if (d->type && d->type->kind() == Type::Kind::Dict)
            keyT = static_cast<DictType&>(*d->type).keyType;
        if (!keyT) keyT = impl_->strType;
        value->type = std::make_shared<DictType>(keyT, impl_->boxedType);
    }
}

bool TypeChecker::coerceLiteralToUnion(Expr* value,
                                       const std::shared_ptr<Type>& target) {
    if (!value || !target || target->kind() != Type::Kind::Union) return false;
    auto& arms = static_cast<UnionType&>(*target).types;
    // A domain may name several arms of one kind (list[str] AND list[int]).
    // Prefer the arm the literal actually fits, so what is stored is what a
    // later downcast can name; fall back to the first of that kind.
    auto armOfKindFor = [&](Type::Kind k,
                            const std::shared_ptr<Type>& want) -> std::shared_ptr<Type> {
        if (want) {
            for (auto& a : arms)
                if (a && a->kind() == k && a->equals(*want)) return a;
            for (auto& a : arms) {
                if (!a || a->kind() != k) continue;
                if (k == Type::Kind::List && want->kind() == Type::Kind::List) {
                    auto& ae = static_cast<ListType&>(*a).elementType;
                    auto& we = static_cast<ListType&>(*want).elementType;
                    if (ae && we && ae->equals(*we)) return a;
                }
                if (k == Type::Kind::Dict && want->kind() == Type::Kind::Dict) {
                    auto& av = static_cast<DictType&>(*a).valueType;
                    auto& wv = static_cast<DictType&>(*want).valueType;
                    if (av && wv && av->equals(*wv)) return a;
                }
            }
        }
        for (auto& a : arms)
            if (a && a->kind() == k) return a;
        return nullptr;
    };
    auto armOfKind = [&](Type::Kind k) -> std::shared_ptr<Type> {
        return armOfKindFor(k, value->type);
    };

    if (auto* lit = dynamic_cast<ListExpr*>(value)) {
        auto arm = armOfKind(Type::Kind::List);
        if (!arm) return false;
        const auto& elemT = static_cast<ListType&>(*arm).elementType;
        if (!elemT) return false;
        for (auto& el : lit->elements) {
            if (!el) continue;
            if (el->type && el->type->isSubtypeOf(*elemT)) continue;
            if (!coerceLiteralToUnion(el.get(), elemT)) return false;
        }
        lit->type = arm;
        return true;
    }
    if (auto* lit = dynamic_cast<DictExpr*>(value)) {
        auto arm = armOfKind(Type::Kind::Dict);
        if (!arm) return false;
        auto& dt = static_cast<DictType&>(*arm);
        if (!dt.keyType || !dt.valueType) return false;
        for (auto& [k, v] : lit->entries) {
            if (k && (!k->type || !k->type->isSubtypeOf(*dt.keyType))) return false;
            if (!v) continue;
            if (v->type && v->type->isSubtypeOf(*dt.valueType)) continue;
            if (!coerceLiteralToUnion(v.get(), dt.valueType)) return false;
        }
        lit->type = arm;
        return true;
    }
    // A scalar already assignable to some arm needs no rewriting.
    return value->type && value->type->isSubtypeOf(*target);
}

std::string TypeChecker::listReprMismatchHint(const Type& from, const Type& to) {
    if (auto* ut = dynamic_cast<const UnionType*>(&to)) {
        for (auto& arm : ut->types) {
            if (!arm || arm->kind() != Type::Kind::List) continue;
            std::string hint = listReprMismatchHint(from, *arm);
            if (!hint.empty()) return hint;
        }
        return "";
    }
    auto* fl = dynamic_cast<const ListType*>(&from);
    auto* tl = dynamic_cast<const ListType*>(&to);
    if (!fl || !tl || !fl->elementType || !tl->elementType) return "";
    bool fromBox = fl->elementType->kind() == Type::Kind::Boxed ||
                   fl->elementType->kind() == Type::Kind::Union;
    bool toBox = tl->elementType->kind() == Type::Kind::Boxed ||
                 tl->elementType->kind() == Type::Kind::Union;
    if (fromBox == toBox) return "";
    return " (the two have different element layouts: monomorphized vs boxed;"
           " build the value with this element type at its declaration, or copy"
           " it element-wise)";
}

void TypeChecker::propagateAnnotationToEmptyLiteral(Expr* value, const std::shared_ptr<Type>& annotType) {
    if (!value || !annotType) return;

    if (auto* tern = dynamic_cast<IfExpr*>(value)) {
        propagateAnnotationToEmptyLiteral(tern->thenExpr.get(), annotType);
        propagateAnnotationToEmptyLiteral(tern->elseExpr.get(), annotType);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(value)) {
        auto op = bin->op.type();
        if (op != TokenType::AND && op != TokenType::OR) return;
        propagateAnnotationToEmptyLiteral(bin->left.get(), annotType);
        propagateAnnotationToEmptyLiteral(bin->right.get(), annotType);
        return;
    }
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
    refuseGeneratorBinding(node.value.get(), node.location());
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
                // Assigning the DECLARED (wider) type back to a narrowed
                // binding ends the narrow: the value is no longer of the
                // narrowed arm, and the fact must not outlive the store.
                // Writing a value that is not of the narrowed arm ends the
                // narrow: the binding holds its DECLARED type again. Without
                // this the narrowed type stands in for the declaration and the
                // diagnostic blames a type the author never wrote.
                if (existing && valueType &&
                    valueType->kind() != Type::Kind::Unknown &&
                    !valueType->equals(*existing) &&
                    !valueType->isSubtypeOf(*existing)) {
                    if (auto declared = impl_->dropNarrowedBinding(name->name))
                        existing = declared;
                }
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
                std::shared_ptr<Type> slotType;
                if (auto* dt = dynamic_cast<DictType*>(contType.get()))
                    slotType = dt->valueType;
                else if (auto* lt = dynamic_cast<ListType*>(contType.get()))
                    slotType = lt->elementType;
                checkSubscriptSlotStore(node, slotType);
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
        return k == Type::Kind::Unknown || k == Type::Kind::Boxed ||
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
    refuseGeneratorBinding(node.value.get(), node.location(), annotType);

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
        static_cast<TaskType&>(*annotType).resultType->kind() == Type::Kind::Boxed &&
        node.value->type && node.value->type->kind() == Type::Kind::Task) {
        declType = node.value->type;
    }

    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        impl_->define(name->name, declType);
    }
}

static std::shared_ptr<Type> subtractUnionMember(
        const std::shared_ptr<Type>& cur, const std::shared_ptr<Type>& sub) {
    if (!cur || cur->kind() != Type::Kind::Union) return nullptr;
    auto& ut = static_cast<UnionType&>(*cur);
    std::vector<std::shared_ptr<Type>> remaining;
    for (auto& m : ut.types) {
        if (!sub || !m->equals(*sub)) remaining.push_back(m);
    }
    if (remaining.empty()) return cur;
    if (remaining.size() == 1) return remaining[0];
    return std::make_shared<UnionType>(std::move(remaining));
}

static const NarrowBinding* findNarrowBinding(
        const std::vector<NarrowBinding>& bindings, const std::string& name) {
    for (auto& nb : bindings) {
        if (nb.name == name) return &nb;
    }
    return nullptr;
}

static std::vector<NarrowBinding> mergeNarrowFacts(
        const std::vector<NarrowBinding>& base,
        const std::vector<NarrowBinding>& refinement) {
    std::vector<NarrowBinding> merged = base;
    for (auto& nb : refinement) {
        auto* existing = const_cast<NarrowBinding*>(findNarrowBinding(merged, nb.name));
        if (existing) existing->type = nb.type;
        else merged.push_back(nb);
    }
    return merged;
}

static bool exprRebindsName(const Expr* target, const std::string& name) {
    if (!target) return false;
    if (auto* n = dynamic_cast<const NameExpr*>(target)) return n->name == name;
    if (auto* t = dynamic_cast<const TupleExpr*>(target)) {
        for (auto& e : t->elements)
            if (exprRebindsName(e.get(), name)) return true;
        return false;
    }
    if (auto* l = dynamic_cast<const ListExpr*>(target)) {
        for (auto& e : l->elements)
            if (exprRebindsName(e.get(), name)) return true;
        return false;
    }
    if (auto* s = dynamic_cast<const StarredExpr*>(target))
        return exprRebindsName(s->value.get(), name);
    return false;
}

static bool namesInclude(const std::vector<std::string>& names,
                         const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

static bool matchPatternBindsName(const MatchPattern& p, const std::string& name) {
    if (p.name == name) return true;
    for (auto& sub : p.subPatterns)
        if (matchPatternBindsName(sub, name)) return true;
    return false;
}

static bool stmtRebindsName(const Stmt* s, const std::string& name);

static bool stmtsRebindName(const std::vector<std::unique_ptr<Stmt>>& stmts,
                            const std::string& name) {
    for (auto& s : stmts)
        if (stmtRebindsName(s.get(), name)) return true;
    return false;
}

static bool stmtRebindsName(const Stmt* s, const std::string& name) {
    if (auto* a = dynamic_cast<const AssignStmt*>(s)) {
        for (auto& t : a->targets)
            if (exprRebindsName(t.get(), name)) return true;
        return false;
    }
    if (auto* a = dynamic_cast<const AugAssignStmt*>(s))
        return exprRebindsName(a->target.get(), name);
    if (auto* a = dynamic_cast<const AnnAssignStmt*>(s))
        return exprRebindsName(a->target.get(), name);
    if (auto* f = dynamic_cast<const ForStmt*>(s))
        return exprRebindsName(f->target.get(), name) ||
               stmtsRebindName(f->body, name) ||
               stmtsRebindName(f->elseBody, name);
    if (auto* w = dynamic_cast<const WhileStmt*>(s))
        return stmtsRebindName(w->body, name) ||
               stmtsRebindName(w->elseBody, name);
    if (auto* i = dynamic_cast<const IfStmt*>(s)) {
        if (stmtsRebindName(i->thenBody, name)) return true;
        for (auto& clause : i->elifClauses)
            if (stmtsRebindName(clause.second, name)) return true;
        return stmtsRebindName(i->elseBody, name);
    }
    if (auto* t = dynamic_cast<const TryStmt*>(s)) {
        if (stmtsRebindName(t->tryBody, name) ||
            stmtsRebindName(t->elseBody, name) ||
            stmtsRebindName(t->finallyBody, name)) return true;
        for (auto& h : t->handlers)
            if (h.name == name || stmtsRebindName(h.body, name)) return true;
        return false;
    }
    if (auto* w = dynamic_cast<const WithStmt*>(s)) {
        for (auto& item : w->items)
            if (exprRebindsName(item.optionalVars.get(), name)) return true;
        return stmtsRebindName(w->body, name);
    }
    if (auto* m = dynamic_cast<const MatchStmt*>(s)) {
        for (auto& c : m->cases)
            if (matchPatternBindsName(c.pattern, name) ||
                stmtsRebindName(c.body, name)) return true;
        return false;
    }
    if (auto* th = dynamic_cast<const ThreadStmt*>(s))
        return namesInclude(th->mutatedCapturedVars, name) ||
               stmtsRebindName(th->body, name);
    if (auto* d = dynamic_cast<const DeleteStmt*>(s)) {
        for (auto& t : d->targets)
            if (exprRebindsName(t.get(), name)) return true;
        return false;
    }
    if (auto* fn = dynamic_cast<const FunctionDecl*>(s))
        return fn->name == name || namesInclude(fn->mutatedCapturedVars, name);
    if (auto* cd = dynamic_cast<const ClassDecl*>(s))
        return cd->name == name;
    if (auto* im = dynamic_cast<const ImportStmt*>(s)) {
        for (auto& alias : im->names)
            if (alias.asName == name || (alias.asName.empty() && alias.name == name))
                return true;
        return false;
    }
    return false;
}

namespace {
struct NarrowRegion {
    const std::vector<NarrowBinding>* entry;
    const std::vector<std::unique_ptr<Stmt>>* body;
};
}

static std::vector<NarrowBinding> survivingNarrowFacts(const IfStmt& node) {
    std::vector<NarrowRegion> live;
    auto addRegion = [&live](const std::vector<NarrowBinding>& entry,
                             const std::vector<std::unique_ptr<Stmt>>& body) {
        if (!stmtsAlwaysTerminate(body)) live.push_back({&entry, &body});
    };
    addRegion(node.thenNarrow, node.thenBody);
    for (size_t i = 0; i < node.elifClauses.size(); ++i)
        addRegion(node.elifNarrows[i], node.elifClauses[i].second);
    addRegion(node.elseNarrow, node.elseBody);
    if (live.empty()) return {};

    std::vector<NarrowBinding> survivors;
    for (auto& nb : *live[0].entry) {
        bool holdsOnEveryLivePath = true;
        for (auto& region : live) {
            const NarrowBinding* fact = findNarrowBinding(*region.entry, nb.name);
            if (!fact || !fact->type || !nb.type || !fact->type->equals(*nb.type) ||
                stmtsRebindName(*region.body, nb.name)) {
                holdsOnEveryLivePath = false;
                break;
            }
        }
        if (holdsOnEveryLivePath) survivors.push_back(nb);
    }
    return survivors;
}

std::shared_ptr<Type> TypeChecker::narrowTargetTypeFromExpr(Expr* e) {
    auto* tn = dynamic_cast<NameExpr*>(e);
    if (!tn) return nullptr;
    if (tn->name == "int")   return impl_->intType;
    if (tn->name == "float") return impl_->floatType;
    if (tn->name == "bool")  return impl_->boolType;
    if (tn->name == "str")   return impl_->strType;
    if (tn->name == "bytes") return impl_->bytesType;
    auto tit = impl_->typeNames.find(tn->name);
    if (tit != impl_->typeNames.end()) return tit->second;
    return impl_->lookup(tn->name);
}

void TypeChecker::defineNarrowBindings(const std::vector<NarrowBinding>& bindings) {
    for (auto& nb : bindings) {
        if (!nb.type || nb.type->kind() == Type::Kind::Unknown) continue;
        // Remember what the name was declared as, so writing to it can end the
        // narrow instead of leaving the narrowed type standing in for the
        // declaration for the rest of the scope.
        auto prev = impl_->lookup(nb.name);
        if (prev && !prev->equals(*nb.type) &&
            !impl_->narrowedFrom.count(nb.name))
            impl_->narrowedFrom[nb.name] = prev;
        impl_->define(nb.name, nb.type);
    }
}

TypeChecker::NarrowFacts TypeChecker::guardFactsForLeaf(Expr* cond) {
    NarrowFacts facts;
    if (auto* bin = dynamic_cast<BinaryExpr*>(cond)) {
        auto op = bin->op.type();
        bool isEq = (op == TokenType::IS || op == TokenType::EQUAL_EQUAL);
        bool isNe = (op == TokenType::IS_NOT || op == TokenType::NOT_EQUAL);
        if (!isEq && !isNe) return facts;
        auto* nm = dynamic_cast<NameExpr*>(bin->left.get());
        bool noneOther = dynamic_cast<NoneLiteral*>(bin->right.get()) != nullptr;
        if (!nm || !noneOther) {
            nm = dynamic_cast<NameExpr*>(bin->right.get());
            noneOther = dynamic_cast<NoneLiteral*>(bin->left.get()) != nullptr;
        }
        if (!nm || !noneOther) return facts;
        auto curType = impl_->lookup(nm->name);
        if (!curType || curType->kind() != Type::Kind::Union) return facts;
        auto nonNone = subtractUnionMember(curType, impl_->noneType);
        auto& whenNone    = isEq ? facts.whenTrue : facts.whenFalse;
        auto& whenPresent = isEq ? facts.whenFalse : facts.whenTrue;
        whenNone.push_back({nm->name, impl_->noneType});
        if (nonNone) whenPresent.push_back({nm->name, nonNone});
        return facts;
    }
    auto* call = dynamic_cast<CallExpr*>(cond);
    if (!call) return facts;
    auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
    if (!callee || callee->name != "isinstance" || call->args.size() != 2)
        return facts;
    auto* argName = dynamic_cast<NameExpr*>(call->args[0].get());
    if (!argName) return facts;
    auto curType = impl_->lookup(argName->name);
    auto matchType = narrowTargetTypeFromExpr(call->args[1].get());
    if (!curType || !matchType) return facts;
    if (curType->kind() == Type::Kind::Union) {
        // `isinstance(v, dict)` on a union must narrow to that union's OWN dict
        // arm (dict[str, Data]), not a bare dict: the arm carries the element
        // types every later read depends on.
        auto& arms = static_cast<UnionType&>(*curType).types;
        for (auto& arm : arms) {
            if (arm && arm->kind() == matchType->kind() &&
                !arm->equals(*matchType)) {
                matchType = arm;
                break;
            }
        }
        facts.whenTrue.push_back({argName->name, matchType});
        auto rest = subtractUnionMember(curType, matchType);
        if (rest) facts.whenFalse.push_back({argName->name, rest});
        return facts;
    }
    if (curType->kind() == Type::Kind::Boxed)
        facts.whenTrue.push_back({argName->name, matchType});
    return facts;
}

TypeChecker::NarrowFacts TypeChecker::checkGuardCondition(Expr* cond) {
    auto* un = dynamic_cast<UnaryExpr*>(cond);
    if (un && un->op.type() == TokenType::NOT) {
        NarrowFacts inner = checkGuardCondition(un->operand.get());
        un->type = impl_->boolType;
        return {std::move(inner.whenFalse), std::move(inner.whenTrue)};
    }
    auto* bin = dynamic_cast<BinaryExpr*>(cond);
    bool isAnd = bin && bin->op.type() == TokenType::AND;
    bool isOr  = bin && bin->op.type() == TokenType::OR;
    if (isAnd || isOr) {
        NarrowFacts left = checkGuardCondition(bin->left.get());
        const auto& carried = isAnd ? left.whenTrue : left.whenFalse;
        impl_->pushScope();
        defineNarrowBindings(carried);
        NarrowFacts right = checkGuardCondition(bin->right.get());
        impl_->popScope();
        auto effectiveLeft =
            isOr ? typeWithoutNone(bin->left->type) : bin->left->type;
        bin->type = joinBranchTypes(effectiveLeft, bin->right->type);
        NarrowFacts facts;
        if (isAnd) facts.whenTrue = mergeNarrowFacts(left.whenTrue, right.whenTrue);
        else facts.whenFalse = mergeNarrowFacts(left.whenFalse, right.whenFalse);
        return facts;
    }
    inferType(cond);
    return guardFactsForLeaf(cond);
}

void TypeChecker::visit(IfStmt& node) {
    NarrowFacts condFacts;
    if (node.condition) condFacts = checkGuardCondition(node.condition.get());

    node.thenNarrow = condFacts.whenTrue;
    impl_->pushScope();
    defineNarrowBindings(node.thenNarrow);
    for (auto& s : node.thenBody) s->accept(*this);
    impl_->popScope();

    node.elifNarrows.clear();
    std::vector<NarrowBinding> reachingFalse = condFacts.whenFalse;
    for (auto& [cond, body] : node.elifClauses) {
        impl_->pushScope();
        defineNarrowBindings(reachingFalse);
        NarrowFacts clauseFacts;
        if (cond) clauseFacts = checkGuardCondition(cond.get());
        impl_->popScope();

        node.elifNarrows.push_back(
            mergeNarrowFacts(reachingFalse, clauseFacts.whenTrue));
        impl_->pushScope();
        defineNarrowBindings(node.elifNarrows.back());
        for (auto& s : body) s->accept(*this);
        impl_->popScope();
        reachingFalse = mergeNarrowFacts(reachingFalse, clauseFacts.whenFalse);
    }

    node.elseNarrow = reachingFalse;
    impl_->pushScope();
    defineNarrowBindings(node.elseNarrow);
    for (auto& s : node.elseBody) s->accept(*this);
    impl_->popScope();

    node.afterNarrow = survivingNarrowFacts(node);
    defineNarrowBindings(node.afterNarrow);
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

    refuseDequeIteration(node.iterable.get());

    impl_->pushScope();
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        if (iterType->kind() == Type::Kind::List) {
            impl_->define(name->name, static_cast<ListType&>(*iterType).elementType);
        } else if (iterType->kind() == Type::Kind::Set) {
            impl_->define(name->name, static_cast<SetType&>(*iterType).elementType);
        } else if (iterType->kind() == Type::Kind::Dict) {
            auto keyT = static_cast<DictType&>(*iterType).keyType;
            impl_->define(name->name, keyT ? keyT : impl_->unknownType);
        } else if (iterType->kind() == Type::Kind::Bytes) {
            impl_->define(name->name, impl_->intType);
        } else if (iterType->kind() == Type::Kind::Str) {
            impl_->define(name->name, impl_->strType);
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
    auto handlerBinding = [&](NamedTypeExpr* named) -> std::shared_ptr<Type> {
        if (named->name.find('.') != std::string::npos) {
            auto qualified = resolveType(named);
            if (qualified && qualified->kind() == Type::Kind::Instance)
                return qualified;
            return nullptr;
        }
        auto it = impl_->typeNames.find(named->name);
        if (it != impl_->typeNames.end() &&
            it->second->kind() == Type::Kind::Instance)
            return it->second;
        auto looked = impl_->lookup(named->name);
        if (looked && looked->kind() == Type::Kind::Class)
            return std::make_shared<InstanceType>(
                std::static_pointer_cast<ClassType>(looked));
        return nullptr;
    };
    auto opaqueExcBinding = [](const std::string& name) {
        static std::unordered_map<std::string,
                                  std::shared_ptr<ClassType>> cache;
        auto& cls = cache[name];
        if (!cls) cls = std::make_shared<ClassType>(name);
        return std::make_shared<InstanceType>(cls);
    };

    impl_->pushScope();
    for (auto& s : node.tryBody) s->accept(*this);
    impl_->popScope();
    for (auto& handler : node.handlers) {
        impl_->pushScope();
        auto* named = handler.name.empty() || !handler.type
                          ? nullptr
                          : dynamic_cast<NamedTypeExpr*>(handler.type.get());
        if (named) {
            auto bind = handlerBinding(named);
            if (!bind) bind = opaqueExcBinding(named->name);
            impl_->define(handler.name, bind);
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
    if (!node.call) return;
    inferType(node.call.get());

    auto* call = dynamic_cast<CallExpr*>(node.call.get());
    if (!call) return;

    if (!call->kwArgs.empty()) {
        error(node.location(),
              "defer does not take keyword arguments; pass them positionally");
        return;
    }

    if (auto* calleeName = dynamic_cast<NameExpr*>(call->callee.get())) {
        if (!impl_->plainFunctionSymbols.count(calleeName->name)) {
            error(node.location(),
                  "defer cannot bind '" + calleeName->name +
                  "': a deferred call must name a top-level function or a "
                  "method on a value. A builtin, a class, a nested def and a "
                  "variable holding a callable are all out of reach; wrap the "
                  "work in a top-level function and defer that");
            return;
        }
    }

    for (const auto& arg : call->args) {
        if (arg && arg->type && arg->type->kind() == Type::Kind::Union) {
            error(arg->location(),
                  "defer cannot store an argument of union type; narrow it to "
                  "a concrete type before deferring the call");
            return;
        }
    }
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
                        std::shared_ptr<Type> fieldT = impl_->boxedType;
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
    auto subjBindingType = subjName ? impl_->lookup(subjName->name) : nullptr;
    auto patternNarrowType = [&](const std::string& tn) -> std::shared_ptr<Type> {
        if (tn == "int")   return impl_->intType;
        if (tn == "float") return impl_->floatType;
        if (tn == "bool")  return impl_->boolType;
        if (tn == "str")   return impl_->strType;
        // A container pattern narrows a union subject to its arm of that
        // kind (e.g. `case dict()` on Data narrows to dict[str, Data]).
        if (subjBindingType && subjBindingType->kind() == Type::Kind::Union) {
            Type::Kind want = Type::Kind::Unknown;
            if (tn == "list")       want = Type::Kind::List;
            else if (tn == "dict")  want = Type::Kind::Dict;
            else if (tn == "set")   want = Type::Kind::Set;
            else if (tn == "tuple") want = Type::Kind::Tuple;
            else if (tn == "bytes") want = Type::Kind::Bytes;
            if (want != Type::Kind::Unknown) {
                auto& ut = static_cast<UnionType&>(*subjBindingType);
                for (auto& m : ut.types)
                    if (m && m->kind() == want) return m;
            }
        }
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
        if (!impl_->returnTypeStack.empty())
            propagateAnnotationToEmptyLiteral(node.value.get(),
                                              impl_->returnTypeStack.back());
        auto retType = inferType(node.value.get());
        if (!impl_->returnTypeStack.empty()) {
            auto& expected = impl_->returnTypeStack.back();
            if (annotationElementIsAny(expected))
                tryExpectedTypeLiteral(node.value.get(), expected);
            if (expected->kind() == Type::Kind::None_ &&
                !impl_->returnAnnotatedStack.empty() &&
                !impl_->returnAnnotatedStack.back()) {
                error(node.location(),
                      "returning a value from a function that declares no "
                      "return type; a def without '->' returns None. Annotate "
                      "it ('-> " + retType->toString() + "') to return a value");
                return;
            }
            if (expected->kind() != Type::Kind::Unknown &&
                retType->kind() != Type::Kind::Unknown &&
                // An unbound `T` in a generic TEMPLATE is re-checked with the
                // concrete type at each stamp; judging it here would reject
                // bodies that are fine for every real instantiation.
                expected->kind() != Type::Kind::TypeVar &&
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
    for (auto& alias : node.names) {
        const std::string bound = alias.asName.empty() ? alias.name : alias.asName;
        impl_->bindTemplateName(impl_->currentModuleName, bound,
                                Impl::qualifyTemplate(node.module, alias.name));
    }

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
            if (symIt->second && symIt->second->kind() == Type::Kind::Function)
                impl_->plainFunctionSymbols.insert(defName);
            if (symIt->second && symIt->second->kind() == Type::Kind::Class) {
                auto cls = std::static_pointer_cast<ClassType>(symIt->second);
                impl_->typeNames[defName] = std::make_shared<InstanceType>(cls);
            }
            continue;
        }
        auto taIt = srcModule.typeExports.find(alias.name);
        if (taIt != srcModule.typeExports.end()) {
            impl_->typeNames[defName] = taIt->second;
            // A renamed recursive alias is canonicalized back to its source
            // name in annotations, so that spelling must resolve here too.
            if (defName != alias.name)
                impl_->typeNames[alias.name] = taIt->second;
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
    {
        Impl::ExternSignatureScope externScope(*impl_, node.isExtern);
        for (size_t i = 0; i < node.params.size(); ++i) {
            if (node.isMethod && !node.hasImplicitSelf &&
                node.params[i].name == "self")
                continue;
            paramTypes.push_back(resolveType(node.params[i].type.get()));
        }
    }
    std::shared_ptr<Type> retType;
    {
        Impl::ExternSignatureScope externScope(*impl_, node.isExtern);
        retType = bodyContainsYield(node.body)
                      ? resolveType(node.returnType.get())
                      : resolveReturnType(node.returnType.get());
    }
    if (node.isAsync && node.isClassMethod) {
        error(node.location(),
              "an async @classmethod is not supported; make it an async "
              "instance method or a module-level async function");
    }
    if (node.isClassMethod && bodyContainsYield(node.body)) {
        error(node.location(),
              "a @classmethod cannot be a generator: '" + node.name +
              "' contains 'yield'. Make it an instance method or a "
              "module-level generator function");
    }
    if (node.isMethod && node.name == "__exit__") {
        size_t userParams = node.params.size();
        if (!node.hasImplicitSelf && !node.params.empty() &&
            node.params[0].name == "self")
            userParams -= 1;
        if (userParams != 0)
            error(node.location(),
                  "__exit__ takes no parameters: it is a guaranteed cleanup "
                  "hook and cannot see or suppress the in-flight exception. "
                  "Drop the parameter list and handle errors with try/except "
                  "around the 'with'");
    }
    if (node.isAsync && retType &&
        (retType->kind() == Type::Kind::Boxed ||
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
    funcType->isGenerator = bodyContainsYield(node.body);
    fillFuncMeta(*funcType, node.params, node.isMethod, node.hasImplicitSelf,
                 node.isClassMethod);

    impl_->define(node.name, funcType);

    impl_->pushScope();
    impl_->returnTypeStack.push_back(retType);
    impl_->returnAnnotatedStack.push_back(node.returnType ? 1 : 0);

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
    impl_->returnAnnotatedStack.pop_back();
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
    if (!node.typeParams.empty()) impl_->genericClassTypeByDecl[&node] = classType;

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
    classType->constructorOverloads.clear();
    std::unordered_map<std::string, int> _ovlCount;
    for (auto& s : node.body) {
        auto* f = dynamic_cast<FunctionDecl*>(s.get());
        if (!f || !f->typeParams.empty() || f->isProperty) continue;
        if (f->name == "__init__" || f->isConstructor) continue;
        _ovlCount[f->name]++;
    }
    std::unordered_map<std::string, int> _ovlNext;
    std::unordered_set<size_t> ctorAritiesSeen;
    std::vector<std::pair<size_t, size_t>> ctorArityRanges;
    for (auto& s : node.body) {
        auto* func = dynamic_cast<FunctionDecl*>(s.get());
        if (!func) continue;
        if (!func->typeParams.empty()) continue;
        if (func->name == "__init__" || func->isConstructor) {
            classType->constructorCount++;
            size_t ctorParamStart = func->hasImplicitSelf ? 0 : 1;
            size_t ctorArity = func->params.size() >= ctorParamStart
                                   ? func->params.size() - ctorParamStart
                                   : 0;
            if (!ctorAritiesSeen.insert(ctorArity).second)
                error(func->location(),
                      "class '" + node.name + "' declares two constructors taking " +
                          std::to_string(ctorArity) +
                          " argument(s); constructors dispatch by argument count, so "
                          "each must take a different number (for same-count variants, "
                          "use a `static def` factory)");
            size_t ctorTrailingDefaults = 0;
            for (size_t i = func->params.size();
                 i > ctorParamStart && func->params[i - 1].defaultValue;
                 --i)
                ++ctorTrailingDefaults;
            size_t ctorMinArity = ctorArity - ctorTrailingDefaults;
            for (auto& [prevMin, prevMax] : ctorArityRanges) {
                size_t lo = std::max(prevMin, ctorMinArity);
                size_t hi = std::min(prevMax, ctorArity);
                if (lo < hi)
                    error(func->location(),
                          "class '" + node.name + "' declares constructors whose "
                          "defaulted parameters overlap: a " + std::to_string(lo) +
                          "-argument call would match both the " +
                          std::to_string(prevMax) + "-parameter and the " +
                          std::to_string(ctorArity) + "-parameter constructor; "
                          "shrink the defaults or use a `static def` factory");
            }
            ctorArityRanges.push_back({ctorMinArity, ctorArity});
        }
        std::vector<std::shared_ptr<Type>> paramTypes;
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (func->isMethod && !func->hasImplicitSelf &&
                (func->params[i].name == "self" ||
                 (func->isClassMethod && func->params[i].name == "cls")))
                continue;
            paramTypes.push_back(resolveType(func->params[i].type.get()));
        }
        auto retType = bodyContainsYield(func->body)
                           ? resolveType(func->returnType.get())
                           : resolveReturnType(func->returnType.get());
        auto externalRet = func->isAsync ? std::static_pointer_cast<Type>(
                                              std::make_shared<TaskType>(retType))
                                         : retType;
        auto fType = std::make_shared<FunctionType>(paramTypes, externalRet);
        fType->spawnsFreshTask = func->isAsync;
        fType->isGenerator = bodyContainsYield(func->body);
        fillFuncMeta(*fType, func->params, func->isMethod, func->hasImplicitSelf,
                     func->isClassMethod);
        if (func->isProperty) {
            if (!classType->fields.count(func->name))
                classType->fields[func->name] = retType;
        } else {
            bool genericBesideConcrete = !func->typeParams.empty() &&
                                         classType->methods.count(func->name) > 0;
            if (!genericBesideConcrete) classType->methods[func->name] = fType;
            if (func->name == "__init__" || func->isConstructor) {
                classType->constructorOverloads.push_back(fType);
            } else {
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
                    classType->fields[func->name] =
                        resolveReturnType(func->returnType.get());
                } else if (func->typeParams.empty() ||
                           classType->methods.count(func->name) == 0) {
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
                            return std::make_shared<ListType>(impl_->boxedType);
                        auto first = rhsLiteralType(le->elements[0].get());
                        if (!first) return std::make_shared<ListType>(impl_->boxedType);
                        for (size_t i = 1; i < le->elements.size(); ++i) {
                            auto t = rhsLiteralType(le->elements[i].get());
                            if (!t || t->kind() != first->kind())
                                return std::make_shared<ListType>(impl_->boxedType);
                        }
                        return std::make_shared<ListType>(first);
                    }
                    if (auto* de = dynamic_cast<DictExpr*>(rhs)) {
                        if (de->entries.empty())
                            return std::make_shared<DictType>(impl_->boxedType, impl_->boxedType);
                        auto firstK = rhsLiteralType(de->entries[0].first.get());
                        auto firstV = rhsLiteralType(de->entries[0].second.get());
                        if (!firstK || !firstV)
                            return std::make_shared<DictType>(impl_->boxedType, impl_->boxedType);
                        for (size_t i = 1; i < de->entries.size(); ++i) {
                            auto kt = rhsLiteralType(de->entries[i].first.get());
                            auto vt = rhsLiteralType(de->entries[i].second.get());
                            if (!kt || kt->kind() != firstK->kind())
                                firstK = impl_->boxedType;
                            if (!vt || vt->kind() != firstV->kind())
                                firstV = impl_->boxedType;
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

    static const std::string kExcMessageField = "message";
    auto messageField = classType->fields.find(kExcMessageField);
    const bool messageIsText =
        messageField == classType->fields.end() || !messageField->second ||
        messageField->second->kind() == Type::Kind::Str ||
        messageField->second->kind() == Type::Kind::Unknown;
    if (!messageIsText && derivesFromBuiltinException(classType.get()))
        error(node.location(),
              "field '" + kExcMessageField + "' on exception class '" +
              node.name + "' must be 'str': it holds the message every "
              "exception subclass is constructed and rendered with. Give this "
              "field another name");

    impl_->currentClass = prevClass;
    impl_->popScope();
}

void TypeChecker::visit(ContractDecl&) {}

void TypeChecker::visit(TypeAliasStmt& node) {
    if (!node.value) return;
    if (typeExprMentionsName(node.value.get(), node.name)) {
        // Recursive alias (e.g. `type Data = str | ... | list[Data]`):
        // pre-register a named knot so the definition can reference itself,
        // then fill in the resolved arms. The knot is nominal: it prints and
        // compares by its alias name, and every use shares this one object.
        auto* u = dynamic_cast<UnionTypeExpr*>(node.value.get());
        if (!u) {
            error(node.location(), "recursive type alias '" + node.name +
                  "' must be a union at the top level (e.g. `type " +
                  node.name + " = int | list[" + node.name + "]`)");
            return;
        }
        for (auto& m : u->types) {
            if (auto* nm = dynamic_cast<NamedTypeExpr*>(m.get())) {
                if (nm->name == node.name) {
                    error(node.location(), "type alias '" + node.name +
                          "' includes itself as a bare arm; a recursive "
                          "reference must sit inside a container (list[" +
                          node.name + "], dict[str, " + node.name + "], ...)");
                    return;
                }
            }
        }
        auto knot = std::make_shared<UnionType>(
            std::vector<std::shared_ptr<Type>>{});
        knot->aliasName = node.name;
        impl_->typeNames[node.name] = knot;
        auto resolved = resolveType(node.value.get());
        if (resolved && resolved->kind() == Type::Kind::Union)
            knot->types = static_cast<UnionType&>(*resolved).types;
        else if (resolved && resolved.get() != knot.get())
            knot->types = {resolved};
        if (impl_->scopes.size() == 1)
            impl_->cachedTypeExports[node.name] = knot;
        return;
    }
    auto resolved = resolveType(node.value.get());
    if (resolved) {
        impl_->typeNames[node.name] = resolved;
        // Module-top-level aliases export to importers (privacy tiers apply at
        // the import site, as for value exports).
        if (impl_->scopes.size() == 1)
            impl_->cachedTypeExports[node.name] = resolved;
    }
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
            std::vector<std::shared_ptr<Type>> ps(nparams, impl_->boxedType);
            ct->methods[func->name] = std::make_shared<FunctionType>(ps, impl_->boxedType);
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
