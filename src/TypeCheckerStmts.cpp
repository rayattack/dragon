/// Dragon TypeChecker - statement & declaration visitors (assign, control flow, imports, functions, classes)
/// Split from TypeChecker.cpp (file-size policy): same class, same behavior -
/// pure code motion, no logic changes.
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

// `type` and `object` both resolve to anyType, but `list[type]` / `dict[K, type]`
// use a class-descriptor representation (i64 handles), NOT the {tag,payload}
// box. Distinguishable only at the annotation AST level (post-resolution both
// are Any), so the box-forcing in AnnAssignStmt must skip the `type` element.
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
    // Only list/set literals against a list-shaped annotation (sets are
    // represented as ListType today). Dict literals have their own keyed check.
    if (annot->kind() != Type::Kind::List) return false;
    const auto& elemT = static_cast<const ListType&>(*annot).elementType;
    if (!elemT || elemT->kind() == Type::Kind::Any ||
        elemT->kind() == Type::Kind::Unknown)
        return false;  // list[Any] / unbound: every element is admissible

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
    // list literal -> list[Base]: accept when every element <: Base.
    if (expected->kind() == Type::Kind::List) {
        // A SET literal also types as ListType (sets ride the list model).
        // Accept it WITHOUT retyping (`s: set = {1, 2}`, where bare `set`
        // resolves to list[Any]): set codegen routes on the literal's own
        // type, and sets have their own runtime representation (dragon_set_*)
        // - there is no monomorphized/boxed list split to guard here.
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
            // Class-descriptor literals ([Foo, Bar] into list[type], which
            // resolves to list[Any]): accept WITHOUT retyping - descriptor
            // lists are native i64 handles, never the box representation.
            for (auto& el : lit->elements)
                if (el->type && el->type->kind() == Type::Kind::Class)
                    return true;
            // An Any element type must reach INTO nested container literals:
            // the outer literal becomes a box list, so its elements are only
            // ever seen through Any - a nested literal left at its narrow type
            // would be built monomorphized and then walked at the box stride.
            for (auto& el : lit->elements)
                boxNestedContainerLiteralForAny(el.get());
        } else if (base->kind() == Type::Kind::List ||
                   base->kind() == Type::Kind::Dict) {
            // base is a CONCRETE container (e.g. `list[dict[str, Any]]`): a
            // nested container-literal element must be BUILT in base's exact
            // representation, so an Any-typed slot nested INSIDE it (the
            // dict[str, Any] value here) is born boxed rather than
            // monomorphized. Recursing at the precise base type - not the Any
            // hammer - keeps concrete nested types (list[str], ...) native and
            // only boxes the slots base actually spells as Any. Idempotent on a
            // literal already matching base; a no-op on a named variable.
            for (auto& el : lit->elements)
                tryExpectedTypeLiteral(el.get(), base);
        }
        lit->type = expected;  // fresh literal -> sound covariant retype
        return true;
    }
    // dict literal -> dict[K, V]: keys must equal K, values <: V.
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
        // Same Any propagation for dict VALUES (dicts themselves have one
        // uniform tagged representation, but a nested list literal in an Any
        // value slot must still be born boxed). Braces are load-bearing: the
        // trailing `else if` must bind to THIS `if`, not the inner `if (v)`.
        if (dt.valueType && dt.valueType->kind() == Type::Kind::Any) {
            for (auto& [k, v] : lit->entries)
                if (v) boxNestedContainerLiteralForAny(v.get());
        } else if (dt.valueType && (dt.valueType->kind() == Type::Kind::List ||
                                    dt.valueType->kind() == Type::Kind::Dict)) {
            // Concrete-container value type (`dict[str, dict[str, Any]]`):
            // recurse so a nested container-literal value is built in the value
            // type's exact representation and its own Any slots born boxed.
            for (auto& [k, v] : lit->entries)
                if (v) tryExpectedTypeLiteral(v.get(), dt.valueType);
        }
        lit->type = expected;
        return true;
    }
    return false;
}

// True when a literal's elements are class descriptors ([Foo, Bar]) - those
// lists use native i64 handles (list[type]), never the box representation.
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
    // Set literals (SetExpr) keep their type: sets ride the ListType model but
    // have their own build path; an Any-held set is untested territory - do
    // not silently retype it here.
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

    // Empty list literal [] with list[T] annotation
    if (auto* list = dynamic_cast<ListExpr*>(value)) {
        if (list->elements.empty() && annotType->kind() == Type::Kind::List) {
            list->type = annotType;
        }
        return;
    }
    // Empty dict literal {} with dict[K,V] annotation
    if (auto* dict = dynamic_cast<DictExpr*>(value)) {
        if (dict->entries.empty() && annotType->kind() == Type::Kind::Dict) {
            dict->type = annotType;
        }
        return;
    }
    // Empty set literal (parsed as SetExpr) with set annotation
    if (auto* set = dynamic_cast<SetExpr*>(value)) {
        if (set->elements.empty() && annotType->kind() == Type::Kind::List) {
            set->type = annotType;
        }
        return;
    }
}

void TypeChecker::visit(ExprStmt& node) {
    if (node.expr) inferType(node.expr.get());
}

void TypeChecker::visit(AssignStmt& node) {
    // If there's a type annotation, propagate it into empty container literals
    // so that e.g. `x: list[str] = []` resolves to list[str] instead of list[<unknown>]
    if (node.typeAnnotation) {
        auto annotType = resolveType(node.typeAnnotation.get());
        propagateAnnotationToEmptyLiteral(node.value.get(), annotType);
        // D044 - expected-type hint for bare generic construction (see AnnAssign).
        impl_->currentExpectedType = annotType;
        auto valueType = inferType(node.value.get());
        impl_->currentExpectedType = nullptr;
        // Force box representation for a literal assigned to list[Any] /
        // dict[K, Any] (see AnnAssignStmt for the rationale); `list[type]`
        // resolves to Any but uses class descriptors, so it is excluded.
        bool elemIsType = containerElementAnnotationIsType(node.typeAnnotation.get());
        if (annotationElementIsAny(annotType) && !elemIsType)
            tryExpectedTypeLiteral(node.value.get(), annotType);
        // Per-element literal check first: first-element inference can match the
        // annotation while a later element doesn't (list[int] = [1, 2, "three"]).
        // `list[type]` (class-descriptor lists) accepts any list value: both
        // sides are native i64 handles, and list invariance would otherwise
        // reject it now that list[T] </: list[Any].
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
        // Define with the annotation type
        for (auto& target : node.targets) {
            if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                impl_->define(name->name, annotType);
            } else if (auto* tup = dynamic_cast<TupleExpr*>(target.get())) {
                // const multi-target unpack: a TupleExpr target with a
                // tuple[...] annotation binds each name at its annotated
                // element type (arity matches by construction in the parser).
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
                // A binding's type is fixed at its declaration. A bare `x = v`
                // on an already-declared name is a *reassignment*: the value
                // must be assignable to the declared type, and the declared
                // type is preserved (never silently re-typed - that produced
                // miscompiles, since codegen flows values at fixed LLVM types).
                auto existing = impl_->lookup(name->name);
                if (existing) {
                    // An empty container literal (`x = []`, `x = {}`) carries
                    // no element type of its own. Push the declared type in so
                    // the reassignment is sound - the same contextual typing the
                    // declaration site applies to `x: list[T] = []`. Without it,
                    // `[]` infers as list[<unknown>] and a valid reassignment is
                    // wrongly rejected.
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
                    // Brand-new binding (e.g. tuple-unpack element, or a form
                    // Sema permits as an implicit declaration): record its type.
                    impl_->define(name->name, valueType);
                }
            } else if (auto* attr = dynamic_cast<AttributeExpr*>(target.get())) {
                // `obj.attr = []` / `self.field = {}` - bare reassign of a
                // class field. Without this, the empty literal carries no
                // element type, and codegen falls back to TAG_INT - a later
                // `self.field.append("x")` then stores a str-pointer into an
                // int-tagged list, breaking value-equality (`in`, .index, etc.)
                // forever. Resolve the field's declared type via the instance
                // and push it into the empty literal - same contextual-typing
                // contract the annotated-decl and name-reassignment paths use.
                auto objType = inferType(attr->object.get());
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
                // Tuple-unpack with no annotation: bind each NEW target name to
                // the RHS tuple's corresponding element type, not Any (cmd #3:
                // honest types) - e.g. `q, r = divmod(...)` -> int, int;
                // `k, s, v = "..".partition("=")` -> str, str, str. The parser
                // guarantees arity; an already-declared name is a reassignment
                // and is left to its fixed declared type.
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
                // `d[k] = ["a", "b"]` into a dict[K, Any] / list[Any] slot: the
                // literal flows into an Any context, so it must be born in the
                // box representation (same rule the annotated-decl sites apply
                // via tryExpectedTypeLiteral).
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
    // `x += y` desugars to `x = x <op> y`, so it must obey the SAME operand
    // rules as the binary operator - and then the result must be assignable
    // back to the target. If this checks nothing, `x: int` followed by
    // `x += "s"` compiles while the equivalent `x = x + "s"` is correctly
    // rejected - a hole in commandment 3 (types must be honest). This mirrors
    // the accept/reject decisions in visit(BinaryExpr) for the compound
    // operators; the two encode the same operator table and should be
    // unified when that table is centralized.
    if (!targetType || !valueType) return;
    auto tk = targetType->kind();
    auto vk = valueType->kind();
    // Unknown / Any / class instances (dunder dispatch) / type parameters are
    // resolved elsewhere or at CodeGen - never rejected here, exactly as
    // visit(BinaryExpr) passes them through.
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
        ok = true;  // int/float arithmetic in any combination
    } else if (op == TokenType::PLUS_EQUAL) {
        // Concatenation forms: str += str, bytes += bytes, list += list.
        ok = (tk == Type::Kind::Str && vk == Type::Kind::Str) ||
             (tk == Type::Kind::Bytes && vk == Type::Kind::Bytes) ||
             (tk == Type::Kind::List && vk == Type::Kind::List);
    } else if (op == TokenType::STAR_EQUAL) {
        // Repetition: str/bytes/list *= int (target is the sequence).
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

    if (node.value) {
        // Propagate annotation type into empty container literals
        // so that e.g. `x: list[str] = []` resolves to list[str]
        propagateAnnotationToEmptyLiteral(node.value.get(), annotType);
        // D044 - hand the annotation to the value as an expected type, so a bare
        // generic construction `Box(5)` infers its type args from `Box[int]`.
        impl_->currentExpectedType = annotType;
        auto valueType = inferType(node.value.get());
        impl_->currentExpectedType = nullptr;
        // A heterogeneous literal stored into a list[Any] / dict[K, Any] target
        // must be built as a BOX container (each element keeps its own tag).
        // `list[T] <: list[Any]` would otherwise let the narrow literal type
        // (e.g. list[int] from `[1, 2]`) pass the assignability check below, so
        // codegen builds the wrong i64/ptr variant and a later `x[i]` reads two
        // adjacent native slots as a {tag, payload} box. Retyping the literal to
        // the Any annotation (sound - every element <: Any) forces the box list.
        // `list[type]` also resolves to Any but uses class descriptors, not
        // boxes, so it is excluded.
        bool elemIsType = containerElementAnnotationIsType(node.annotation.get());
        if (annotationElementIsAny(annotType) && !elemIsType)
            tryExpectedTypeLiteral(node.value.get(), annotType);
        // Per-element literal check first (see AssignStmt): catches a later
        // element that violates the declared element type even when first-
        // element inference happened to match. `list[type]` accepts any list
        // value (native i64 descriptor handles on both sides - see AssignStmt).
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

    // Bare `t: Task = fire f()` refines its result type from the concrete RHS:
    // a task yields exactly one typed value, so the binding becomes Task[T]
    // (sound, no boxing) rather than the lossy Task[Any] the bare annotation
    // resolved to. Explicit `Task[T]` is already concrete and kept as-is.
    std::shared_ptr<Type> declType = annotType;
    if (node.value && annotType->kind() == Type::Kind::Task &&
        static_cast<TaskType&>(*annotType).resultType->kind() == Type::Kind::Any &&
        node.value->type && node.value->type->kind() == Type::Kind::Task) {
        declType = node.value->type;
    }

    // Define with annotation type
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        impl_->define(name->name, declType);
    }
}

void TypeChecker::visit(IfStmt& node) {
    // D039 Phase 5a: extend isinstance narrowing from IfExpr to statement-level
    // if/elif/else. When the condition is `isinstance(name, T)`, the then-body
    // sees `name` as type T; the else-body sees it as (union - T) or, for bare
    // Any, falls back to Any. Mirrors IfExpr's narrowing logic at line 1491+.
    auto narrowedTypeFromExpr = [&](Expr* e) -> std::shared_ptr<Type> {
        auto* tn = dynamic_cast<NameExpr*>(e);
        if (!tn) return nullptr;
        if (tn->name == "int")   return impl_->intType;
        if (tn->name == "float") return impl_->floatType;
        if (tn->name == "bool")  return impl_->boolType;
        if (tn->name == "str")   return impl_->strType;
        if (tn->name == "bytes") return impl_->bytesType;
        // Class types live in the type registry (`typeNames`) - the same place
        // resolveType() looks them up - NOT in value scopes. Resolving via
        // lookup() here returned null for every class name, silently disabling
        // isinstance narrowing for `isinstance(x, SomeClass)` (builtin types
        // worked only because they are hard-coded above). Check typeNames first,
        // then fall back to a class bound in scope.
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
    // Inspect a condition for `isinstance(name, T)` and produce the narrowed
    // then-type and else-type. Returns true if narrowing applies.
    auto analyzeIsinstance = [&](Expr* cond, std::string& outName,
                                 std::shared_ptr<Type>& outThenT,
                                 std::shared_ptr<Type>& outElseT) -> bool {
        // `x is None` / `x == None` / `x is not None` / `x != None` against a
        // union narrows None vs its non-None complement across the branches.
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
        // Union source: narrow to T in then, (union - T) in else.
        if (curType->kind() == Type::Kind::Union) {
            outName = argName->name;
            outThenT = narrowT;
            outElseT = subtractFromUnion(curType, narrowT);
            return true;
        }
        // Any source: narrow to T in then; else stays Any (the universe).
        if (curType->kind() == Type::Kind::Any) {
            outName = argName->name;
            outThenT = narrowT;
            outElseT = curType;
            return true;
        }
        return false;
    };

    if (node.condition) inferType(node.condition.get());

    // Then-branch with optional narrowing.
    std::string nName;
    std::shared_ptr<Type> nThen;
    std::shared_ptr<Type> nElse;
    bool narrowedHere = node.condition &&
                        analyzeIsinstance(node.condition.get(), nName, nThen, nElse);
    impl_->pushScope();
    if (narrowedHere) impl_->define(nName, nThen);
    for (auto& s : node.thenBody) s->accept(*this);
    impl_->popScope();

    // Elif chain: each clause is its own scope; collect narrowings independently.
    // Note: this doesn't model the cumulative else-of-prior-elifs narrowing yet.
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

    // Else-branch with the anti-narrowed type (only when the head condition
    // produced a narrowing).
    impl_->pushScope();
    if (narrowedHere) impl_->define(nName, nElse ? nElse : impl_->unknownType);
    for (auto& s : node.elseBody) s->accept(*this);
    impl_->popScope();

    // Fall-through narrowing: when the then-branch always terminates and there is
    // no elif/else, statements AFTER this `if` (in the current scope) are reached
    // only when the condition was false - so `name` is narrowed to the else-type
    // for the rest of the scope (the Python early-return idiom). Uses the same
    // terminator predicate as CodeGen's merge narrowing so the two agree.
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
    auto iterType = inferType(node.iterable.get());

    // A function value is not iterable: `for a in argv` (sys.argv is a
    // FUNCTION; the list is `argv()`) belongs with the subscript/len()
    // rejections - without this it fell through to an untyped loop over
    // garbage.
    if (iterType && iterType->kind() == Type::Kind::Function) {
        if (auto* fn = dynamic_cast<NameExpr*>(node.iterable.get())) {
            error(node.location(), "cannot iterate the function '" + fn->name +
                  "'; call it first: 'for ... in " + fn->name + "()'");
        } else {
            error(node.location(), "cannot iterate a function value; call it first");
        }
    }

    // Loop variable and body share the loop's block scope.
    impl_->pushScope();
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        if (iterType->kind() == Type::Kind::List) {
            impl_->define(name->name, static_cast<ListType&>(*iterType).elementType);
        } else {
            impl_->define(name->name, impl_->unknownType);
        }
    } else if (auto* tup = dynamic_cast<TupleExpr*>(node.target.get())) {
        // Tuple-unpack target: `for a, b in <iterable>`. Type each element from
        // the iterable's element type so the unpack vars carry their REAL static
        // type (str/float/...), not unknown->int. Without this a str element
        // funnels through int at use sites that key on the static type - e.g. a
        // dict-value store `out[k] = v` tagged the str `v` as int and the runtime
        // dict rejected it ("value ... is int, not str"). The runtime VarKind was
        // already correct (ForLoop.cpp posKinds), so only static-type-keyed sites
        // misbehaved; print()/concat (VarKind-keyed) looked fine, masking it.
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
        // Bind `handler.name` to the handler's declared type so attribute
        // access inside the body (`ex.reason` for `except MyErr as ex`)
        // resolves the field's declared type. Without this, NameExpr("ex")
        // typed to Unknown, AttributeExpr("ex.reason").type stayed unset,
        // and `emitTagForExpr` (D030 §5) couldn't derive TAG_STR - boxing
        // into an `Any` parameter (`takes_any(ex.reason)`,
        // `self.assertEqual(ex.reason, "x")`) tagged the str pointer as
        // TAG_INT and the receiver saw a raw pointer integer.
        //
        // Resolve without going through resolveType: built-in exception
        // names (Exception, ValueError, OSError, ...) aren't in typeNames
        // and would trigger an "unknown type" diagnostic - they bind an
        // OPAQUE exception instance type instead (one semantic for both
        // kinds: `e` IS the exception; `str(e)` / `print(e)` / f"{e}" yield
        // the message). Using `e` directly AS a str is a compile error -
        // the old silently-legal `e.upper()` / `s: str = e` fork is gone.
        // Codegen is unchanged (the binding still carries the message ptr;
        // zero runtime cost). Mirror resolveType's NamedType logic for
        // user-defined classes (local + imported via lookup).
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
                    // Builtin exception (Sema already validated the name
                    // exists): synthesize one opaque, field-less ClassType
                    // per name - attribute access errors ("has no
                    // attribute"), stringification routes to the message.
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
        // Bind the `as` variable to the context value's type. A context
        // manager's `as` value is __enter__()'s return; for the common
        // self-returning manager (Dragon's Connection, file handles, locks)
        // that is the context type itself. Without this the as-var is UNTYPED,
        // which silently breaks anything type-directed in the body - notably
        // generic method calls (`db.all[T](...)`), whose monomorphization needs
        // the receiver's class via inferType (the non-generic methods resolve at
        // codegen and so appeared to work, hiding the bug).
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
    // The operand type-checks as an ordinary call, so signature and own-mode
    // rules (E13/E14) apply at the defer statement unchanged. The call's
    // return value is discarded by the grammar; nothing to flow.
    if (node.call) inferType(node.call.get());
}

void TypeChecker::visit(MatchStmt& node) {
    if (node.subject) inferType(node.subject.get());
    // Recursively infer types for all literals in pattern tree
    // Full positional field order of a class pattern (ancestors first, then own),
    // built from each ClassType's `fieldOrder`. The matching list is rebuilt
    // identically in CodeGen from the same `instanceFieldOrder` helper, so
    // position -> field-name agrees across stages.
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
                // Positional field destructuring `case T(p0, p1, ...)`. Only user
                // classes have positional fields; a builtin scalar/container is a
                // pure type test.
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
                    // Type each sub-pattern against its field; bind captures.
                    for (size_t i = 0; i < pat.subPatterns.size(); ++i) {
                        auto& sub = pat.subPatterns[i];
                        std::shared_ptr<Type> fieldT = impl_->anyType;
                        if (ct && i < order.size()) {
                            auto fit = ct->fields.find(order[i]);
                            if (fit != ct->fields.end() && fit->second)
                                fieldT = fit->second;
                        }
                        inferPatternTypes(sub);  // nested literals / sub-patterns
                        if (sub.kind == MatchPattern::Kind::Capture && !sub.name.empty())
                            impl_->define(sub.name, fieldT);
                    }
                    return;  // sub-patterns handled; skip the generic recurse
                }
            }
        }
        for (auto& sub : pat.subPatterns) inferPatternTypes(sub);
    };

    // #1 in-arm narrowing: a single `case T()` type-test on a bare-name subject
    // narrows that name to T inside the arm (so the body can use the value at
    // its matched type - e.g. `case int() { print(v + 1) }`). Mirrors the
    // isinstance narrowing in visit(IfStmt). CodeGen unboxes exactly this same
    // set (Exceptions.cpp), so the two predicates MUST stay in sync.
    auto* subjName = dynamic_cast<NameExpr*>(node.subject.get());
    auto patternNarrowType = [&](const std::string& tn) -> std::shared_ptr<Type> {
        if (tn == "int")   return impl_->intType;
        if (tn == "float") return impl_->floatType;
        if (tn == "bool")  return impl_->boolType;
        if (tn == "str")   return impl_->strType;
        return nullptr;  // class / container / bytes narrowing lands later (#4)
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

    // ---- #2 exhaustiveness + #3 reachability (compile-time, zero runtime) ----
    // A match over a CLOSED scrutinee (a finite, enumerable domain: a union, a
    // bool) must cover every case or carry a catch-all, else a value silently
    // falls through and the match does nothing. Open domains (bare int/str/
    // class/...) can't be made exhaustive without `_`, so they are left alone
    // (the fall-through-is-intentional case). Reachability flags arms a prior
    // catch-all or a duplicate literal already subsumes. All static - no runtime
    // cost, and it converts a silent fall-through into a caught bug (#2 of the
    // commandments: no silent wrong behavior).
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

    std::set<std::string> coveredTypes;   // `case T()` type-test names
    std::set<int64_t>     coveredInts;    // `case 5` int literals (dup detection)
    std::set<std::string> coveredStrs;    // `case "x"` str literals (dup detection)
    bool coveredNone = false, coveredTrue = false, coveredFalse = false;
    int catchAllIdx = -1;

    // A pattern is irrefutable iff it matches every value of its (sub)type: a
    // wildcard, a capture, or a class destructuring whose every sub-pattern is
    // itself irrefutable. `case Point(a, b)` covers the `Point` member of a
    // union; `case Point(0, y)` (a literal sub-pattern) does NOT.
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
    // A class arm covers its type for exhaustiveness iff the destructuring (if
    // any) is irrefutable - a bare `case T()` or `case T(a, b)`.
    auto collectTypeTest = [&](const MatchPattern& p) {
        if (p.kind == MatchPattern::Kind::Class && isIrrefutable(p))
            coveredTypes.insert(p.name);
    };

    for (size_t i = 0; i < node.cases.size(); ++i) {
        auto& c = node.cases[i];
        auto& pat = c.pattern;
        bool guarded = (c.guard != nullptr) || (pat.guard != nullptr);

        // #3: any arm after an UNGUARDED catch-all is dead.
        if (catchAllIdx >= 0)
            error(node.location(),
                  "unreachable case: a previous catch-all (`case _` or a bare "
                  "capture) already matches every value");

        // Coverage only counts for an UNGUARDED arm (a guard may fail at runtime,
        // so the type isn't guaranteed handled).
        if (!guarded) {
            collectTypeTest(pat);
            if (pat.kind == MatchPattern::Kind::Or)
                for (auto& sub : pat.subPatterns) collectTypeTest(sub);
        }

        if (pat.kind == MatchPattern::Kind::Literal && pat.literal) {
            Expr* lit = pat.literal.get();
            // None/bool coverage counts only for an unguarded arm; duplicate
            // detection (below) runs regardless of guard.
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

    // #2: exhaustiveness on a closed scrutinee, only when there is no catch-all.
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
        // Check against expected return type. A fresh container literal is
        // blessed against the declared return type (tryExpectedTypeLiteral) so
        // `return ["a"]` in a `-> list[Any]` function is BUILT as a box list -
        // the same covariance-at-construction rule the assign sites apply.
        if (!impl_->returnTypeStack.empty()) {
            auto& expected = impl_->returnTypeStack.back();
            // A fresh container literal returned into a dict[K, Any] / list[Any]
            // target must be BUILT in the box representation (nested list
            // literals born list[Any]), exactly as the assign sites force via
            // tryExpectedTypeLiteral. Unlike lists, `dict[K, V] <: dict[K, Any]`
            // (the Any-only relaxation) PASSES the assignability check below, so
            // the error-fallback `tryExpectedTypeLiteral` never fires and a
            // nested list value stays monomorphized (list[str]) while stored
            // under an Any tag - read back as list[Any] it walks the wrong
            // stride (view-check raise). Force the born-box retype here. Scoped
            // to literals: tryExpectedTypeLiteral is a no-op on a named variable
            // (only ListExpr/DictExpr are retyped), so a NAMED list[str] value
            // returned into an Any slot still follows the invariance rule.
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
        // return without value -- check that return type is None or unknown
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
    if (node.cause) inferType(node.cause.get());
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
    // Python rule (matched exactly):
    //  `import x` -> binds `x` to module x
    //  `import x.y` -> binds `x` (NOT `x.y`); `x.y` reachable via attribute
    //  `import x.y as z` -> binds `z` to the leaf module `x.y` (not `x`)
    for (auto& alias : node.names) {
        if (!alias.asName.empty()) {
            // Aliased: bind asName to the leaf module's ModuleType.
            auto mt = impl_->getOrCreateModuleType(alias.name);
            impl_->define(alias.asName, mt);
        } else {
            // No alias: bind only the topmost segment.
            auto dot = alias.name.find('.');
            std::string topName = (dot == std::string::npos)
                ? alias.name
                : alias.name.substr(0, dot);
            // Make sure the full chain exists in the registry so attribute
            // walks (`x.y.z`) succeed even if no FromImport touched the leaf.
            impl_->getOrCreateModuleType(alias.name);
            auto topMt = impl_->getOrCreateModuleType(topName);
            impl_->define(topName, topMt);
        }
    }
}

void TypeChecker::visit(FromImportStmt& node) {
    // Look up the source module. If it's not registered, silently skip -
    // matches the prior behaviour and avoids spurious errors during partial
    // analysis (e.g., stdlib modules type-checked in isolation).
    auto modIt = impl_->moduleTypes.find(node.module);
    if (modIt == impl_->moduleTypes.end()) return;

    auto& srcModule = *modIt->second;
    for (auto& alias : node.names) {
        std::string defName = alias.asName.empty() ? alias.name : alias.asName;
        // D045 - privacy is keyed on the EXPORTED name (`alias.name`), not the
        // local alias: `from hmac import new as _hmac_new` imports the public
        // `new`; the underscore on the alias is a local binding, untouched.
        // Error-and-continue so binding still happens (no cascade of secondary
        // "undefined name" errors); the diagnostic points at this import.
        checkModuleNamePrivacy(srcModule, alias.name, node.location());
        // Submodule import: `from controllers import health` where `health`
        // is a sibling .dr file. Bind the imported name to the submodule's
        // ModuleType so subsequent `health.health_check` walks correctly.
        auto subIt = srcModule.submodules.find(alias.name);
        if (subIt != srcModule.submodules.end()) {
            impl_->define(defName, subIt->second);
            continue;
        }
        // Value import: function, class, const, etc. exported by the module.
        auto symIt = srcModule.exports.find(alias.name);
        if (symIt != srcModule.exports.end()) {
            // Intrinsic export (e.g. threading.Lock -> LockType): bind the
            // value name as a constructor returning the intrinsic, and register
            // the annotation type. Aliasing (`import Lock as L`) is rejected:
            // codegen's Lock intercepts are keyed on the name "Lock", so an
            // alias would mis-compile - a clear error beats a silent miscompile.
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
            // Register imported class types in typeNames so resolveType() finds them
            if (symIt->second && symIt->second->kind() == Type::Kind::Class) {
                auto cls = std::static_pointer_cast<ClassType>(symIt->second);
                impl_->typeNames[defName] = std::make_shared<InstanceType>(cls);
            }
            continue;
        }
        // `from collections import deque` - deque is a global builtin container
        // (codegen-backed, like list/set/dict, no import required). Python
        // sources it from collections; honor that spelling as a no-op binding
        // (the name is already globally in scope) rather than erroring.
        if (node.module == "collections" && alias.name == "deque") {
            auto tnIt = impl_->typeNames.find("deque");
            if (tnIt != impl_->typeNames.end())
                impl_->typeNames[defName] = tnIt->second;
            continue;
        }
        // Not in submodules or exports -> real error.
        error(node.location(),
              "cannot import name '" + alias.name + "' from module '" + node.module + "'");
    }
}

//===----------------------------------------------------------------------===//
// Declaration Visitors
//===----------------------------------------------------------------------===//

// M1/M2: populate call-validation metadata (param names, required-arg count,
// vararg flag) on a FunctionType from a decl's parameter list. `self` is
// excluded for methods so the metadata aligns with paramTypes. Required params
// precede defaulted ones (enforced by sema), so the first `requiredParams`
// names are the mandatory ones. A *args/**kwargs param sets hasVarArg, which
// disables the call-site arity check (its bound is open).
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
        // Exclude the implicit receiver from call-site arity/binding metadata:
        // `self` for an instance method, `cls` for a @classmethod.
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
    // D044 - a generic template is fully checked once by the generic pre-pass;
    // the main module walk must not re-check it (and it is never lowered).
    if (!node.typeParams.empty() && impl_->genericChecked.count(&node)) return;
    // D044+ - a generic METHOD (declares its OWN type parameter, `def m[T]()` on
    // a class) is monomorphized exactly like a generic free function once `T` is
    // bound below: its body is abstractly checked here with `self`/currentClass
    // already bound by the enclosing visit(ClassDecl) walk, and each concrete use
    // site (`recv.m[int](...)`) stamps a specialization via the worklist. (A method
    // that only uses its generic CLASS's parameter has empty typeParams and never
    // reaches this branch.)
    // Bind type parameters to TypeVarTypes for this template's signature + body
    // check; the unbounded-`T` restriction is gated by genericTemplateDepth.
    bool pushedTP = !node.typeParams.empty();
    if (pushedTP) {
        std::unordered_map<std::string, std::shared_ptr<Type>> frame;
        for (auto& tp : node.typeParams) {
            // Bounds - resolve the bound (`T: Bound`) so member/operator access on
            // `T` in the body can be checked against it; nullptr = unbounded.
            std::shared_ptr<Type> bnd =
                tp.bound ? resolveType(tp.bound.get()) : nullptr;
            frame[tp.name] = std::make_shared<TypeVarType>(tp.name, bnd);
        }
        impl_->typeParamScopes.push_back(std::move(frame));
        impl_->genericTemplateDepth++;
    }

    // Build function type - exclude self for methods (both modes)
    std::vector<std::shared_ptr<Type>> paramTypes;
    for (size_t i = 0; i < node.params.size(); ++i) {
        // In .py mode, skip explicit self from FunctionType
        if (node.isMethod && !node.hasImplicitSelf && node.params[i].name == "self")
            continue;
        paramTypes.push_back(resolveType(node.params[i].type.get()));
    }
    auto retType = resolveType(node.returnType.get());
    // `async def f() -> T` is callable as `f() : Task[T]` - calling it spawns a
    // green thread and hands back the handle. The body itself still returns T,
    // so the externally-visible FunctionType wraps in Task while the body's
    // return-checks (returnTypeStack) use the underlying T.
    auto externalRet = node.isAsync ? std::static_pointer_cast<Type>(
                                          std::make_shared<TaskType>(retType))
                                    : retType;
    auto funcType = std::make_shared<FunctionType>(paramTypes, externalRet);
    fillFuncMeta(*funcType, node.params, node.isMethod, node.hasImplicitSelf,
                 node.isClassMethod);

    // Define function in current scope
    impl_->define(node.name, funcType);

    // Type check body in new scope
    impl_->pushScope();
    impl_->returnTypeStack.push_back(retType);

    // For implicit-self methods, define self in method scope
    if (node.isMethod && node.hasImplicitSelf) {
        auto selfType = impl_->lookup("self");
        if (selfType) impl_->define("self", selfType);
    }

    // Define parameters
    size_t typeIdx = 0;
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (node.isMethod && !node.hasImplicitSelf && node.params[i].name == "self")
            continue;
        auto pt = paramTypes[typeIdx++];
        // Inside the body, `*args: T` IS a `list[T]` and `**kw: T` a
        // `dict[str, T]` - that's how codegen packs them. Binding the element
        // type alone (the annotation) mis-typed the param vs its runtime shape,
        // so `len(args)` / iteration / forwarding (`inner(*args, **kw)`, the
        // C9-B `*list`/`**dict` spread) all saw a scalar. The external
        // FunctionType (paramTypes) is unchanged - only the body binding wraps.
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
    // D044 - skip re-checking a generic template on the main walk (done by the
    // generic pre-pass); it is never lowered.
    if (!node.typeParams.empty() && impl_->genericChecked.count(&node)) return;
    // D044 v1 does not support subclassing a generic instantiation
    // (`class Dog(Animal[str])`) - the base resolves the instantiation but the
    // parent link / inherited members aren't wired across the stamp boundary
    // yet. A base written as a subscript (`Animal[str]`) is exactly that case;
    // report it clearly instead of letting inheritance silently lose members.
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
            // Bounds - resolve the bound (`T: Bound`) so member/operator access on
            // `T` in the body can be checked against it; nullptr = unbounded.
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
    // Reuse the ClassType the layout pre-pass registered (with annotated field
    // types already populated) so forward references resolved during the body
    // walk and the canonical type stay the same object. The field-registration
    // guards below (`fields.count(...) continue`) leave those entries intact and
    // only add ctor-inferred / method types. Falls back to a fresh type for any
    // class the pre-pass skipped (e.g. TypedDict, handled separately below).
    std::shared_ptr<ClassType> classType;
    if (auto tnIt = impl_->typeNames.find(node.name); tnIt != impl_->typeNames.end()) {
        if (auto inst = std::dynamic_pointer_cast<InstanceType>(tnIt->second))
            classType = inst->classType;
    }
    if (!classType) classType = std::make_shared<ClassType>(node.name);

    // D045 - stamp the declaring module/file so member-access privacy can
    // compute this class's package (P_D). Survives the export boundary on the
    // shared_ptr. Always set from the module currently being checked.
    classType->definingModule = impl_->currentModuleName;
    classType->definingFile = impl_->currentFile;

    // D045 point 3 - reject any reserved-shape (`__x__`) member name that is
    // not a recognized special method. Validate the explicit declarations:
    // methods (`def __x__`, incl. the parser-normalized `def()` -> __init__) and
    // class-body annotated fields (`__x__: T`).
    for (auto& s : node.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(s.get()))
            checkDunderDeclaration(func->name, /*moduleLevel=*/false, node.name,
                                   func->location());
        else if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get()))
            if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                checkDunderDeclaration(tgt->name, /*moduleLevel=*/false, node.name,
                                       ann->location());
    }

    // Detect TypedDict base class and resolve the first concrete parent
    // class (single-inheritance for type-level tracking - multi-bases like
    // mixins are still parsed but only the first non-special base is wired
    // into ClassType::parentClass). This populates the inheritance chain so
    // template[X] protocol checks (D017 Phase 4) and any future ancestry
    // queries have a single source of truth instead of a parallel map.
    // value type for an enum class: int for Enum/IntEnum, str for StrEnum.
    std::shared_ptr<Type> enumValueType = impl_->intType;
    for (auto& base : node.bases) {
        if (auto* baseName = dynamic_cast<NameExpr*>(base.get())) {
            if (baseName->name == "TypedDict") {
                classType->isTypedDict = true;
                continue;
            }
            // Class-based enum markers (`from enum import Enum`). Treated like
            // TypedDict: a marker base, not a real parent. synthesizeEnumMethods
            // (codegen) rewrites members into singleton statics; here we only
            // record the shape so member access type-checks.
            if (baseName->name == "Enum" || baseName->name == "IntEnum" ||
                baseName->name == "StrEnum") {
                classType->isEnum = true;
                if (baseName->name == "StrEnum") enumValueType = impl_->strType;
                continue;
            }
        }
        if (classType->parentClass) continue;  // already set (first concrete base wins)
        std::shared_ptr<Type> baseType;
        if (auto* baseName = dynamic_cast<NameExpr*>(base.get())) {
            baseType = impl_->lookup(baseName->name);
            if (!baseType) {
                auto tit = impl_->typeNames.find(baseName->name);
                if (tit != impl_->typeNames.end()) baseType = tit->second;
            }
        } else if (dynamic_cast<AttributeExpr*>(base.get())) {
            // Dotted cross-module base (`unittest.TestCase`): resolve through
            // the module's exports via the AttributeExpr visitor, which walks
            // ModuleType.exports. Without this the parentClass chain was never
            // wired for imported parents, so subclass <: parent failed.
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

    // Register class type name for resolution
    impl_->typeNames[node.name] = std::make_shared<InstanceType>(classType);
    impl_->define(node.name, classType);

    // Type check body in new scope
    impl_->pushScope();

    // Define 'self' as instance of this class
    impl_->define("self", std::make_shared<InstanceType>(classType));

    // D045 - the lexically-enclosing class for member-access privacy. Set for
    // the whole body walk (methods, ctors, @staticmethod/@classmethod all count
    // as "inside class C"); save/restore handles nested classes.
    const ClassType* prevClass = impl_->currentClass;
    impl_->currentClass = classType.get();

    // Register class-body field declarations (PEP 526 `name: T [= v]`) as
    // fields of this class. The declared annotation is authoritative (D030), so
    // this runs BEFORE the constructor `self.X = ...` inference walk below; that
    // walk's `if (classType->fields.count(...)) continue;` guard then leaves the
    // precise declared type in place rather than a looser inferred one (e.g. an
    // empty `[]` would otherwise downgrade `list[int]` to `list[Any]`). Without
    // this, a field declared only in the class body (never assigned in the
    // constructor) had no entry in classType->fields at all, so attribute access
    // `obj.field` resolved to an untyped fallback and whole-collection print /
    // method dispatch on it misfired. TypedDict schemas are handled separately
    // below (and isTypedDict is already set), so skip them here.
    if (!classType->isTypedDict) {
        for (auto& s : node.body) {
            auto* ann = dynamic_cast<AnnAssignStmt*>(s.get());
            if (!ann || !ann->annotation) continue;
            auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
            if (!tgt || classType->fields.count(tgt->name)) continue;
            classType->fields[tgt->name] = resolveType(ann->annotation.get());
        }
    }

    // Class-based enum: each member is a singleton INSTANCE of this class, not
    // the int/str literal its annotation suggests. Override the member field
    // types to InstanceType(self) and expose the per-member `.name`/`.value`
    // attributes, so `Color.RED` types as a Color and `Color.RED.value`
    // resolves. CodeGen trusts these types, so without this `Color.RED` would
    // mis-lower to an int read. (synthesizeEnumMethods does the codegen rewrite.)
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

    // Method-signature pre-pass: register every method's FunctionType (and every
    // @property's return type) BEFORE type-checking any method body. Methods may
    // call each other in any order, so a method that calls a sibling declared
    // LATER in the class must still resolve that sibling's return type. Without
    // this, the body-walk loop below registered a method only AFTER visiting its
    // body, so a forward call like `self.later()` resolved to an unknown return
    // type - and an inline `for x in self.later()` then lost its element type,
    // miscompiling field access on the loop variable (silent i64-0 lowering).
    // Mirrors the FunctionType construction in visit(FunctionDecl) (self excluded
    // for methods, async wrapped in Task). Guarded so the body-walk's identical
    // registration below is a harmless idempotent overwrite.
    classType->constructorCount = 0;  // M2: recount on each visit (idempotent)
    // ADR 010 method overloading: pre-count same-name methods so each can record
    // its overload index/count below. Excludes constructors (arity-overloaded via
    // constructorCount), properties (no-call accessors), and generic templates.
    // Rebuilt each visit (idempotent) - the push_back below must not double-count.
    classType->methodOverloads.clear();
    std::unordered_map<std::string, int> _ovlCount;
    for (auto& s : node.body) {
        auto* f = dynamic_cast<FunctionDecl*>(s.get());
        if (!f || !f->typeParams.empty() || f->isProperty) continue;
        if (f->name == "__init__" || f->isConstructor) continue;
        _ovlCount[f->name]++;
    }
    std::unordered_map<std::string, int> _ovlNext;  // running index per name
    for (auto& s : node.body) {
        auto* func = dynamic_cast<FunctionDecl*>(s.get());
        if (!func) continue;
        // D044 - a generic method (declares its own type parameter) is rejected
        // with a clear error in visit(FunctionDecl); resolving its `T`-typed
        // signature here (with no type-param scope bound) would only add spurious
        // "unknown type 'T'" noise, so skip it.
        if (!func->typeParams.empty()) continue;
        if (func->name == "__init__" || func->isConstructor)
            classType->constructorCount++;
        std::vector<std::shared_ptr<Type>> paramTypes;
        for (size_t i = 0; i < func->params.size(); ++i) {
            // The implicit receiver is excluded from the call-site signature:
            // `self` for an instance method, `cls` for a @classmethod (called
            // as `Cls.m()` with cls bound implicitly). Without skipping cls,
            // a classmethod call wrongly reported "missing required argument
            // 'cls'". @staticmethod has no receiver, so nothing is skipped.
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
        fillFuncMeta(*fType, func->params, func->isMethod, func->hasImplicitSelf,
                     func->isClassMethod);
        if (func->isProperty) {
            if (!classType->fields.count(func->name))
                classType->fields[func->name] = retType;
        } else {
            classType->methods[func->name] = fType;
            // ADR 010: record overload index/count and collect the overload set
            // (only when the name is genuinely overloaded, so single-method
            // dispatch is untouched). Constructors use constructorCount instead.
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

        // Collect method types. @property methods are accessed without parens,
        // so register them as fields of the property's return type.
        if (auto* func = dynamic_cast<FunctionDecl*>(s.get())) {
            auto fType = impl_->lookup(func->name);
            if (fType) {
                if (func->isProperty) {
                    classType->fields[func->name] = resolveType(func->returnType.get());
                } else {
                    classType->methods[func->name] = fType;
                }
            }
            // Infer field types from `self.X = ...` assignments inside the
            // constructor. Without this, a class instance passed by
            // parameter has no field types and `c.field + c.field2`
            // mistypes as `int` (the default for unknown attribute access),
            // which then fails to assign back to a `str` slot.
            //
            // We do this OUTSIDE the function's own scope (we already
            // popScope'd back to the class scope after FunctionDecl::visit),
            // so we can't call inferType on RHS - the param symbols are
            // gone. Build a param-name -> declared-type map from the
            // constructor signature directly, plus handle a small set of
            // type-obvious literal RHSs. Covers the common patterns:
            //  self.x = paramName (paramName: T) -> field T
            //  self.x = "literal" -> str
            //  self.x = 0 / True / 1.0 -> int / bool / float
            //  self.x = [] -> list[Any] (elem-kind unknown)
            //  self.x = {} -> dict[Any, Any]
            //  self.x: T = ... -> annotation wins
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
                    // D030 single source of truth: if the TypeChecker already
                    // typed this expression during the FunctionDecl visit
                    // above (line ~1834: s->accept(*this) runs before this
                    // walk), trust that result. This catches every RHS shape
                    // the AST-pattern cases below would miss - user-function
                    // calls (`self.x = make_bytes()`), method calls
                    // (`self.x = obj.frob()`), arithmetic results, ternaries,
                    // etc. - without each gap needing its own pattern.
                    // Without this, an unannotated `self.x = userFn()` where
                    // `userFn` returns `bytes` leaves `classType->fields[x]`
                    // empty, AttributeExpr `c.x` later has no node.type, and
                    // bytes-vs-list dispatch (len, [], +) routes through the
                    // wrong runtime ops on a bytes pointer -> garbage reads.
                    if (rhs->type && rhs->type->kind() != Type::Kind::Unknown) {
                        return rhs->type;
                    }
                    if (dynamic_cast<StringLiteral*>(rhs)) return impl_->strType;
                    if (dynamic_cast<IntegerLiteral*>(rhs)) return impl_->intType;
                    if (dynamic_cast<FloatLiteral*>(rhs)) return impl_->floatType;
                    if (dynamic_cast<BooleanLiteral*>(rhs)) return impl_->boolType;
                    if (dynamic_cast<NoneLiteral*>(rhs)) return impl_->noneType;
                    // Element-type inference for collection literals: peek at
                    // the literal's contents and, if every entry agrees on a
                    // single concrete type, propagate it. Without this a
                    // class field initialized as `self.h = {"k": "v"}` is
                    // recorded as `dict[Any, Any]`, which loses every
                    // downstream type check (`@property def h() -> dict[str,
                    // str]: return self.h` rejects with a misleading "Any vs
                    // str" mismatch). Empty literals still fall back to Any
                    // because there's nothing to infer from.
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
                    // `self.x = bytes()` / `bytes(list)` - recognize the
                    // builtin bytes constructor so chained `.decode()` and
                    // other bytes-method dispatches see node.type = Bytes
                    // and route through the bytes runtime ops instead of
                    // falling through to instance-method resolution.
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
                                !classType->fields.count(attr->attribute)) {
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

        // TypedDict: collect annotated fields as per-key types
        if (classType->isTypedDict) {
            if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get())) {
                if (auto* fieldName = dynamic_cast<NameExpr*>(ann->target.get())) {
                    auto fieldType = resolveType(ann->annotation.get());
                    classType->fields[fieldName->name] = fieldType;
                }
            }
        }
    }

    impl_->currentClass = prevClass;  // D045 - restore enclosing-class context
    impl_->popScope();
}

void TypeChecker::visit(TypeAliasStmt& node) {
    // PEP 695 `type X = <expr>`. Resolve the aliased type and register the alias
    // name in typeNames so later annotations (`x: X = ...`) resolve through the
    // normal NamedTypeExpr path. Without this registration the alias parsed but
    // was unusable - `x: UserId = 5` reported "unknown type 'UserId'".
    if (!node.value) return;
    auto resolved = resolveType(node.value.get());
    if (resolved) impl_->typeNames[node.name] = resolved;
}

void TypeChecker::visit(Module& node) {
    // Pre-pass: register all top-level class names so type annotations referring
    // to classes defined later in the module can resolve. Without this, mutual
    // class references in type annotations (`Callable[[B], None]` inside class A
    // where B is declared below) fail with "unknown type B".
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
            // D045 - stamp the declaring file NOW (pre-pass), so a class whose
            // body hasn't been walked yet (forward reference within this module)
            // still carries its package for any earlier class that touches it.
            if (classType && classType->definingFile.empty()) {
                classType->definingModule = impl_->currentModuleName;
                classType->definingFile = impl_->currentFile;
            }
        }
    }

    // Method pre-pass: with every class NAME registered, register each class's
    // METHOD names BEFORE any body is checked, so a CROSS-class forward method
    // reference resolves - class A calling `b.method()` where B is defined LATER
    // in the module (a TestCase using helper classes declared below it, or
    // server.dr's SubdomainContext delegating to a Router method). Without it the
    // "no method" error in visit(AttributeExpr) fires on a method of a not-yet-
    // visited class.
    //
    // EXISTENCE only - deliberately no resolveType (a param/return that names a
    // not-yet-imported class would emit a premature "unknown type"; imports may
    // be unresolved this early). Methods get correct arity with Any param/return
    // types; the owning class's visit(ClassDecl) overwrites them with precise,
    // resolved signatures once imports are ready. Fields are intentionally NOT
    // touched here (see the per-class note below).
    for (auto& stmt : node.body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        if (!cd) continue;
        std::shared_ptr<ClassType> ct;
        if (auto it = impl_->typeNames.find(cd->name); it != impl_->typeNames.end()) {
            if (auto inst = std::dynamic_pointer_cast<InstanceType>(it->second))
                ct = inst->classType;
        }
        if (!ct) continue;

        // Canonical positional field order for match destructuring (own fields;
        // ancestors are prepended at the use site by walking parentClass).
        ct->fieldOrder = instanceFieldOrder(*cd);

        // Method existence: correct arity, Any param/return types. The owning
        // class's visit(ClassDecl) overwrites these with the precise, resolved
        // signatures. @property is left to visit(ClassDecl) (it registers the
        // precise return type unconditionally in the body walk).
        for (auto& s : cd->body) {
            auto* func = dynamic_cast<FunctionDecl*>(s.get());
            if (!func || func->isProperty) continue;
            size_t nparams = 0;
            for (auto& p : func->params)
                // Exclude the implicit receiver from call-site arity: `self`
                // for instance methods, `cls` for @classmethod. (Mirrors the
                // precise-signature loop in visit(ClassDecl).)
                if (!(func->isMethod && !func->hasImplicitSelf &&
                      (p.name == "self" || (func->isClassMethod && p.name == "cls"))))
                    ++nparams;
            std::vector<std::shared_ptr<Type>> ps(nparams, impl_->anyType);
            ct->methods[func->name] = std::make_shared<FunctionType>(ps, impl_->anyType);
        }

        // Field-NAME existence (no types - purely syntactic): class-body `x: T`
        // and every `self.X` / `self.X: T` assignment target across ALL methods.
        // This is the set visit(AttributeExpr) consults to tell a genuinely-
        // undefined member from one whose precise type just isn't inferred yet.
        // We collect from every method (not only __init__) so a field assigned
        // outside the constructor still resolves rather than producing a false
        // "no attribute" - never injecting anything into `fields` (codegen-safe).
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

    // D045 point 3 - reject any reserved-shape (`__x__`) module-level name that
    // is not recognized metadata. Covers top-level functions, classes, and
    // annotated/plain assignment targets.
    for (auto& stmt : node.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get()))
            checkDunderDeclaration(func->name, /*moduleLevel=*/true, "", func->location());
        else if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get()))
            checkDunderDeclaration(cd->name, /*moduleLevel=*/true, "", cd->location());
        else if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                checkDunderDeclaration(tgt->name, /*moduleLevel=*/true, "", ann->location());
        } else if (auto* as = dynamic_cast<AssignStmt*>(stmt.get())) {
            for (auto& t : as->targets)
                if (auto* tgt = dynamic_cast<NameExpr*>(t.get()))
                    checkDunderDeclaration(tgt->name, /*moduleLevel=*/true, "", as->location());
        }
    }

    // D044 - register + abstractly check all generic templates here, AFTER the
    // method-name / declaredFieldNames pre-passes above (so a template's
    // ctor-assigned fields like `self.value` resolve) but BEFORE the main walk
    // (so a use site `Box[int]` finds a complete ClassType). The main walk then
    // skips each template (genericChecked); only stamped instantiations are
    // lowered. The instantiation worklist itself is drained in check(), after
    // this walk, while the module scope is still live.
    collectGenericTemplates(node);

    for (auto& stmt : node.body) {
        stmt->accept(*this);
    }
}

} // namespace dragon
