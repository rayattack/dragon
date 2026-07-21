/// Dragon TypeChecker - expression visitors (calls, attributes, subscripts, literals-composite, comprehensions, lambdas)
/// Split from TypeChecker.cpp (file-size policy): same class, same behavior -
/// pure code motion, no logic changes.
// FIXME: isinstance() on generic type params still wrong for nested unions
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

void TypeChecker::visit(CallExpr& node) {
    // D044 - consume the binding's expected-type hint (set by AnnAssign/Assign)
    // immediately, so it applies to THIS call's generic construction inference
    // and never leaks into a nested call's arguments.
    auto expectedType = impl_->currentExpectedType;
    impl_->currentExpectedType = nullptr;

    // Visit arguments first - generic-function inference needs their types, and
    // it must run before the callee is resolved as a value (an explicit
    // `first[int]` callee is a SubscriptExpr that is NOT a real value subscript).
    for (auto& arg : node.args) {
        inferType(arg.get());
    }
    for (auto& [name, arg] : node.kwArgs) {
        inferType(arg.get());
    }

    // D044 - generic free-function call (`first(xs)` inferred / `first[int](xs)`
    // explicit) and generic class construction (`Box[int](5)` explicit /
    // `b: Box[int] = Box(5)` inferred). On success each records the
    // instantiation, retargets the callee to the stamped name, sets the result
    // type, and the normal callee/arity validation below is bypassed.
    {
        std::vector<std::shared_ptr<Type>> argTypes;
        for (auto& a : node.args) {
            if (auto* st = dynamic_cast<StarredExpr*>(a.get()))
                argTypes.push_back(st->value ? st->value->type : nullptr);
            else
                argTypes.push_back(a->type);
        }
        if (tryInstantiateGenericCall(node, argTypes, expectedType)) return;
        if (tryInstantiateGenericConstruction(node, expectedType)) return;
    }

    // D044 - `T(...)` constructs a value of a type parameter: in a generic
    // template body T is a TypeVarType, so the call's type IS T (the stamp's
    // concrete re-check validates the real constructor / conversion). Without
    // this the call typed as <unknown>; a declared `v: T = T(...)` or
    // `out.append(T(...))` masked it through leniency, but `[T(...) for r in xs]`
    // surfaced it as `list[<unknown>]` vs the declared `list[T]`. Fixing it here
    // makes type-parameter construction type-correct everywhere it appears.
    if (auto* tpCallee = dynamic_cast<NameExpr*>(node.callee.get())) {
        if (auto tv = lookupTypeParam(tpCallee->name)) {
            node.type = tv;
            return;
        }
    }

    auto calleeType = inferType(node.callee.get());

    // ADR 010 method-overload resolution. When the callee is `obj.method` (or
    // `Class.method`) and the receiver's class declares >1 method with that name,
    // pick the overload whose parameters match this call's argument types - arity
    // first, then assignability, preferring an exact-type match. The chosen index
    // is recorded on the node; CodeGen appends `__ovN` to the symbol so the
    // emitted call is a DIRECT call to one monomorphic function (no runtime
    // dispatch). Single-definition names never enter methodOverloads, so the
    // common path is untouched. Spreads/kwargs with an overloaded method are a
    // clean error rather than a silent wrong pick.
    if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        std::shared_ptr<Type> objType = attr->object ? attr->object->type : nullptr;
        const ClassType* cls = nullptr;
        if (objType && objType->kind() == Type::Kind::Instance)
            cls = static_cast<InstanceType&>(*objType).classType.get();
        else if (objType && objType->kind() == Type::Kind::Class)
            cls = static_cast<const ClassType*>(objType.get());
        const std::vector<std::shared_ptr<Type>>* cands = nullptr;
        for (const ClassType* oc = cls; oc; ) {
            auto it = oc->methodOverloads.find(attr->attribute);
            if (it != oc->methodOverloads.end()) { cands = &it->second; break; }
            // A class that declares the name as a SINGLE method shadows any
            // ancestor's same-name overload set (standard override semantics):
            // stop here so the single-method path handles it - do NOT reach past
            // it to inherit the ancestor's overloads. Inherited overload sets
            // still resolve when no descendant declares the name at all.
            if (oc->methods.count(attr->attribute)) break;
            oc = (oc->parentClass && oc->parentClass->kind() == Type::Kind::Class)
                     ? static_cast<const ClassType*>(oc->parentClass.get()) : nullptr;
        }
        if (cands && cands->size() > 1) {
            bool hasSpread = false;
            for (auto& a : node.args)
                if (dynamic_cast<StarredExpr*>(a.get())) hasSpread = true;
            if (hasSpread || !node.kwArgs.empty()) {
                error(node.location(),
                      "spread or keyword arguments are not supported when calling "
                      "the overloaded method '" + attr->attribute + "'");
                node.type = impl_->anyType;
                return;
            }
            // (matches, exact) for one arg against one parameter type. Any/Unknown
            // on either side is lenient (matches, not exact) so inference gaps
            // don't spuriously fail or bias resolution.
            auto argMatch = [&](const std::shared_ptr<Type>& at,
                                const std::shared_ptr<Type>& pt,
                                bool& exact) -> bool {
                exact = false;
                if (!at || !pt) return true;
                auto ak = at->kind(), pk = pt->kind();
                if (ak == Type::Kind::Unknown || ak == Type::Kind::Any ||
                    pk == Type::Kind::Unknown || pk == Type::Kind::Any)
                    return true;
                if (at->equals(*pt)) { exact = true; return true; }
                return at->isSubtypeOf(*pt);
            };
            std::vector<int> matched;     // candidate indices whose params all match
            std::vector<int> exactMatched;
            for (size_t ci = 0; ci < cands->size(); ++ci) {
                auto& ft = static_cast<FunctionType&>(*(*cands)[ci]);
                if (ft.paramTypes.size() != node.args.size()) continue;  // arity
                bool all = true, allExact = true;
                for (size_t ai = 0; ai < node.args.size(); ++ai) {
                    bool ex = false;
                    if (!argMatch(node.args[ai]->type, ft.paramTypes[ai], ex)) {
                        all = false; break;
                    }
                    if (!ex) allExact = false;
                }
                if (all) { matched.push_back((int)ci); if (allExact) exactMatched.push_back((int)ci); }
            }
            int chosen = -1;
            if (exactMatched.size() == 1) chosen = exactMatched[0];
            else if (exactMatched.empty() && matched.size() == 1) chosen = matched[0];
            if (chosen < 0) {
                if (matched.empty())
                    error(node.location(),
                          "no overload of method '" + attr->attribute +
                          "' matches the argument types");
                else
                    error(node.location(),
                          "ambiguous call to overloaded method '" + attr->attribute +
                          "' - multiple overloads match the argument types");
                node.type = impl_->anyType;
                return;
            }
            node.resolvedMethodOverload = chosen;
            auto& chosenFt = static_cast<FunctionType&>(*(*cands)[chosen]);
            // The chosen overload's parameter types are this call's expected
            // types: bless fresh container-literal arguments against them,
            // exactly like the single-callee path does below (empty literals
            // adopt the param's representation; non-empty ones force the box
            // representation for Any/union element types, recursively).
            // Without this, an inline dict/list literal passed to an
            // OVERLOADED method kept its monomorphized guess - a nested
            // `["a", "b"]` inside a dict[str, Any] argument compiled as
            // list[str] and the callee walked the wrong stride reading it.
            for (size_t ai = 0; ai < node.args.size(); ++ai) {
                const auto& pt = chosenFt.paramTypes[ai];
                const auto& at = node.args[ai]->type;
                if (!pt || !at) continue;
                auto ak = at->kind(), pk = pt->kind();
                bool container = ak == Type::Kind::List || ak == Type::Kind::Dict ||
                                 ak == Type::Kind::Tuple || ak == Type::Kind::Task;
                if (!container || ak != pk) continue;
                if (auto* le = dynamic_cast<ListExpr*>(node.args[ai].get())) {
                    if (le->elements.empty()) {
                        propagateAnnotationToEmptyLiteral(node.args[ai].get(), pt);
                        continue;
                    }
                } else if (auto* de = dynamic_cast<DictExpr*>(node.args[ai].get())) {
                    if (de->entries.empty()) {
                        propagateAnnotationToEmptyLiteral(node.args[ai].get(), pt);
                        continue;
                    }
                }
                tryExpectedTypeLiteral(node.args[ai].get(), pt);
            }
            node.type = chosenFt.returnType ? chosenFt.returnType : impl_->anyType;
            return;
        }
    }

    // C9-B: general call-site spread (`*tuple` / `*list` / `**dict`). C9-A
    // rejected every non-TypedDict spread here; codegen now lowers the general
    // forms (decisions/047). This block ALLOWS the implemented shapes and emits
    // compile-time arity / element-type diagnostics where it can prove a
    // mismatch, while still rejecting genuinely unsupported shapes (a positional
    // argument after `*`, which Python makes keyword-only). The block is
    // self-contained - it sets node.type and returns so the exact-arity
    // validators below (which assume a 1:1 arg<->param mapping) never see spread.
    {
        int firstStar = -1, posAfterStar = -1;
        for (size_t i = 0; i < node.args.size(); ++i) {
            if (dynamic_cast<StarredExpr*>(node.args[i].get())) {
                if (firstStar < 0) firstStar = (int)i;
            } else if (firstStar >= 0 && posAfterStar < 0) {
                posAfterStar = (int)i;
            }
        }
        bool hasStarArg = firstStar >= 0;
        bool hasKwSpread = false;
        for (auto& kw : node.kwArgs)
            if (kw.first.empty()) { hasKwSpread = true; break; }

        if (hasStarArg || hasKwSpread) {
            bool calleeIsTypedDict =
                calleeType && calleeType->kind() == Type::Kind::Class &&
                static_cast<ClassType&>(*calleeType).isTypedDict;
            // `**`-only into a TypedDict class is the D032 typed-row path.
            if (calleeIsTypedDict && !hasStarArg) {
                node.type = std::make_shared<InstanceType>(
                    std::static_pointer_cast<ClassType>(calleeType));
                return;
            }

            // Reject a positional argument after `*spread` (Python makes those
            // keyword-only; Dragon rejects at `check` per the C9-B plan).
            if (posAfterStar >= 0) {
                error(node.args[posAfterStar]->location(),
                      "positional argument after `*` spread is not allowed");
                node.type = impl_->anyType;
                return;
            }

            // Resolve the callee's parameter signature (free function / method
            // FunctionType, or a single ctor's __init__ - all exclude `self`).
            // Overloaded ctors and metadata-less signatures (builtins / Callable
            // annotations) skip the detailed compatibility checks below.
            std::vector<std::shared_ptr<Type>> paramTypes;
            size_t requiredParams = 0;
            bool targetVarArg = false, haveMeta = false;
            std::shared_ptr<Type> retType;
            if (calleeType->kind() == Type::Kind::Function) {
                auto& ft = static_cast<FunctionType&>(*calleeType);
                paramTypes = ft.paramTypes;
                requiredParams = ft.requiredParams;
                targetVarArg = ft.hasVarArg;
                haveMeta = ft.hasArgMeta;
                retType = ft.returnType;
            } else if (calleeType->kind() == Type::Kind::Class) {
                auto& ct = static_cast<ClassType&>(*calleeType);
                retType = std::make_shared<InstanceType>(
                    std::static_pointer_cast<ClassType>(calleeType));
                if (ct.constructorCount <= 1) {
                    auto it = ct.methods.find("__init__");
                    if (it != ct.methods.end() && it->second &&
                        it->second->kind() == Type::Kind::Function) {
                        auto& ift = static_cast<FunctionType&>(*it->second);
                        paramTypes = ift.paramTypes;   // excludes self
                        requiredParams = ift.requiredParams;
                        targetVarArg = ift.hasVarArg;
                        haveMeta = ift.hasArgMeta;
                    }
                }
            } else {
                // Spread into an Unknown / Any / Module callee - can't verify
                // here; allow and let codegen resolve or diagnose.
                node.type = impl_->unknownType;
                return;
            }

            // `**dict` spread source checks: it binds parameters by NAME, so
            // the source must be a str-keyed dict; and when the target
            // declares `**kwargs: T`, the source's value type must be
            // assignable to T (the callee reads entries at T's native type -
            // a mismatched value would be reinterpreted, not converted).
            if (hasKwSpread) {
                std::shared_ptr<Type> kwElemType;
                if (calleeType->kind() == Type::Kind::Function) {
                    auto& ft = static_cast<FunctionType&>(*calleeType);
                    if (ft.hasKwArg && ft.hasArgMeta && !ft.paramTypes.empty())
                        kwElemType = ft.paramTypes.back();  // **kwargs is last
                }
                for (auto& kw : node.kwArgs) {
                    if (!kw.first.empty()) continue;
                    auto dt = inferType(kw.second.get());
                    if (!dt) continue;
                    if (dt->kind() == Type::Kind::Dict) {
                        auto& dictT = static_cast<DictType&>(*dt);
                        auto kk = dictT.keyType ? dictT.keyType->kind()
                                                : Type::Kind::Unknown;
                        if (kk != Type::Kind::Unknown && kk != Type::Kind::Any &&
                            kk != Type::Kind::Str) {
                            error(kw.second->location(),
                                  "`**` spread requires a str-keyed dict (it "
                                  "binds parameters by name); got '" +
                                  dt->toString() + "'");
                        }
                        auto vt = dictT.valueType;
                        if (kwElemType && vt &&
                            kwElemType->kind() != Type::Kind::Unknown &&
                            kwElemType->kind() != Type::Kind::Any &&
                            vt->kind() != Type::Kind::Unknown &&
                            vt->kind() != Type::Kind::Any &&
                            !vt->isAssignableTo(*kwElemType)) {
                            error(kw.second->location(),
                                  "dict spread value type '" + vt->toString() +
                                  "' is not assignable to the `**kwargs` "
                                  "element type '" + kwElemType->toString() +
                                  "'");
                        }
                    } else if (dt->kind() != Type::Kind::Unknown &&
                               dt->kind() != Type::Kind::Any) {
                        error(kw.second->location(),
                              "`**` spread source must be a dict, got '" +
                              dt->toString() + "'");
                    }
                }
            }

            // Forwarding into a `*args`/`**kwargs` target, or a signature with
            // no metadata: skip compatibility checks (codegen packs/handles it).
            if (haveMeta && !targetVarArg) {
                // Conservative compatibility (mirrors the exact-arity arg check
                // below): only fire on a PROVABLE scalar mismatch. Skip
                // Any/Unknown/Union/None and same-kind containers (Dragon
                // containers are invariant, but a fresh covariant element is
                // idiomatic - distinguishing isn't worth a false positive).
                auto assignable = [&](const std::shared_ptr<Type>& at,
                                      const std::shared_ptr<Type>& pt) -> bool {
                    if (!at || !pt) return true;
                    auto ak = at->kind(), pk = pt->kind();
                    if (ak == Type::Kind::Unknown || ak == Type::Kind::Any ||
                        pk == Type::Kind::Unknown || pk == Type::Kind::Any ||
                        ak == Type::Kind::None_ || ak == Type::Kind::Union ||
                        pk == Type::Kind::Union)
                        return true;
                    auto isContainer = [](Type::Kind k) {
                        return k == Type::Kind::List || k == Type::Kind::Dict ||
                               k == Type::Kind::Tuple || k == Type::Kind::Set ||
                               k == Type::Kind::Task;
                    };
                    if (isContainer(ak) && ak == pk) return true;
                    return at->isSubtypeOf(*pt);
                };
                size_t explicitPos = hasStarArg ? (size_t)firstStar
                                                : node.args.size();
                if (hasStarArg) {
                    auto& starExpr =
                        static_cast<StarredExpr&>(*node.args[firstStar]);
                    auto st = starExpr.value ? starExpr.value->type : nullptr;
                    if (st && st->kind() == Type::Kind::Tuple) {
                        // *tuple: static arity + per-element type checks. With
                        // no `**` the positional count is exact, so an over- or
                        // under-fill is a compile-time error (the cost model's
                        // guarantee).
                        auto& tt = static_cast<TupleType&>(*st);
                        size_t L = tt.elementTypes.size();
                        size_t totalPos = explicitPos + L;
                        auto starLoc = node.args[firstStar]->location();
                        if (node.kwArgs.empty() && totalPos > paramTypes.size()) {
                            error(starLoc,
                                  "tuple spread expands to " +
                                  std::to_string(totalPos) + " positional "
                                  "arguments but the callable takes at most " +
                                  std::to_string(paramTypes.size()));
                            node.type = retType ? retType : impl_->anyType;
                            return;
                        }
                        if (node.kwArgs.empty() && totalPos < requiredParams) {
                            error(starLoc,
                                  "tuple spread fills " +
                                  std::to_string(totalPos) + " positional "
                                  "arguments but the callable requires " +
                                  std::to_string(requiredParams));
                            node.type = retType ? retType : impl_->anyType;
                            return;
                        }
                        for (size_t k = 0; k < L; ++k) {
                            size_t pidx = explicitPos + k;
                            if (pidx >= paramTypes.size()) break;
                            if (!assignable(tt.elementTypes[k], paramTypes[pidx]))
                                error(starLoc,
                                      "tuple spread element " +
                                      std::to_string(k + 1) + " of type '" +
                                      tt.elementTypes[k]->toString() + "' is not "
                                      "assignable to parameter type '" +
                                      paramTypes[pidx]->toString() + "'");
                        }
                    } else if (st && st->kind() == Type::Kind::List) {
                        // *list[T]: arity is a runtime check; statically verify
                        // T is accepted by every slot the spread can fill.
                        auto& lt = static_cast<ListType&>(*st);
                        for (size_t pidx = explicitPos;
                             pidx < paramTypes.size(); ++pidx) {
                            if (!assignable(lt.elementType, paramTypes[pidx])) {
                                error(node.args[firstStar]->location(),
                                      "list spread element type '" +
                                      (lt.elementType
                                           ? lt.elementType->toString() : "?") +
                                      "' is not assignable to parameter type '" +
                                      paramTypes[pidx]->toString() + "'");
                                break;
                            }
                        }
                    }
                    // Unknown spread-source kind: allow (runtime).
                }
            }
            node.type = retType ? retType : impl_->anyType;
            return;
        }
    }

    // M1/M2: arity + keyword-argument validation against the param metadata
    // fillFuncMeta records for user functions/methods/ctors. Returns true (and
    // emits a located diagnostic) on a wrong-arity / unknown-kwarg / duplicate-
    // arg call. Skips entirely when the FunctionType has no metadata (builtins,
    // Callable annotations). For a variadic callee the OPEN-ended checks (too
    // many positionals, unknown keyword) are skipped - `*args`/`**kwargs` absorb
    // the surplus - but the required params BEFORE `*args` are still checked, so
    // `def f(a, b, *xs)` called as `f(1)` is a clean error instead of an LLVM
    // "Operand is null" codegen crash. (Spread calls never reach here - the
    // C9-B block above returns early for any `*`/`**` arg.)
    auto validateCall = [&](FunctionType& ft, const std::string& dispName) -> bool {
        if (!ft.hasArgMeta) return false;
        size_t nParams = ft.paramNames.size();
        std::vector<bool> filled(nParams, false);
        bool err = false;
        if (!ft.hasVarArg && node.args.size() > nParams) {
            error(node.location(), dispName + " takes at most " +
                  std::to_string(nParams) + " positional argument" +
                  (nParams == 1 ? "" : "s") + " but " +
                  std::to_string(node.args.size()) + " were given");
            err = true;
        }
        // docs/002 2.8: a move has a declaration end and a call-site end, and
        // BOTH must say own. E13: a NAMED argument to an own parameter needs
        // the visible move (fresh temps are exempt - no binding to poison).
        // E14: own at a borrowing parameter has no meaning.
        for (size_t i = 0; i < node.args.size() && i < ft.paramOwns.size(); ++i) {
            auto* nm = dynamic_cast<NameExpr*>(node.args[i].get());
            bool argMoved = nm && nm->isMoveMarked;
            if (ft.paramOwns[i] && nm && !argMoved) {
                error(node.args[i]->location(),
                      dispName + " takes ownership of its argument; move it "
                      "with 'own " + nm->name + "', or pass a fresh value");
                err = true;
            } else if (!ft.paramOwns[i] && argMoved) {
                error(node.args[i]->location(),
                      dispName +
                          " borrows its argument; own has no meaning here");
                err = true;
            }
        }
        for (size_t i = 0; i < node.args.size() && i < nParams; ++i) filled[i] = true;
        for (auto& kw : node.kwArgs) {
            if (kw.first.empty()) continue;  // ** spread - C9 handles
            auto it = std::find(ft.paramNames.begin(), ft.paramNames.end(), kw.first);
            if (it == ft.paramNames.end()) {
                // Unknown keyword: an error unless the callee has `**kwargs` to
                // absorb it. A `*args`-only callee (hasVarArg && !hasKwArg) still
                // rejects names that match no declared parameter.
                if (!ft.hasKwArg) {
                    error(node.location(),
                          dispName + " got an unexpected keyword argument '" + kw.first + "'");
                    err = true;
                }
                continue;
            }
            size_t idx = (size_t)std::distance(ft.paramNames.begin(), it);
            if (filled[idx]) {
                error(node.location(),
                      dispName + " got multiple values for argument '" + kw.first + "'");
                err = true; continue;
            }
            filled[idx] = true;
        }
        // Required params (the first `requiredParams`, since defaults follow
        // required in a valid signature) must all be filled.
        for (size_t i = 0; i < ft.requiredParams && i < nParams; ++i) {
            if (!filled[i]) {
                error(node.location(),
                      dispName + " missing required argument '" + ft.paramNames[i] + "'");
                err = true; break;
            }
        }
        return err;
    };

    // Conservative positional argument type check + fresh-literal blessing,
    // shared by plain function/method calls AND single-ctor class calls (the
    // caller gates on no kwargs + exact arity). Any/Unknown on either side is
    // skipped (can't prove a violation). This catches the silent miscompile
    // `def sq(n: int)...; sq("x")` and the str-vs-template[SQL] mismatch, and
    // blesses container literals against the param's representation. Ctor
    // calls must run it too: an inline `[11, 22]` passed to a `list[Any]`
    // ctor param otherwise compiles monomorphized and the callee walks the
    // wrong stride (`xs[0]` reads a box that was never there).
    auto checkPositionalArgs = [&](FunctionType& ft) {
        auto isContainer = [](Type::Kind k) {
            return k == Type::Kind::List || k == Type::Kind::Dict ||
                   k == Type::Kind::Tuple || k == Type::Kind::Task;
        };
        for (size_t i = 0; i < node.args.size(); ++i) {
            const auto& at = node.args[i]->type;
            const auto& pt = ft.paramTypes[i];
            if (!at || !pt) continue;
            auto ak = at->kind(), pk = pt->kind();
            if (ak == Type::Kind::Unknown || ak == Type::Kind::Any ||
                pk == Type::Kind::Unknown)
                continue;
            if (pk == Type::Kind::Any) {
                // A container literal ENTERING A BOXED-ELEMENT CONTAINER
                // (append/remove on a list[Any] receiver - the Any param is
                // the receiver's element type) is born in the box
                // representation: it will only ever be read back through
                // Any. A literal passed to a PLAIN Any param keeps its
                // concrete monomorphized type (commandment #3) - the box
                // boundary carries native lists safely (dynamic ops
                // header-dispatch; builtins like bytes() read natively;
                // unboxing to list[Any] raises a teaching TypeError).
                if (auto* att = dynamic_cast<AttributeExpr*>(node.callee.get())) {
                    bool recvIsBoxList = false;
                    if (att->object && att->object->type) {
                        if (auto* rlt = dynamic_cast<ListType*>(
                                att->object->type.get()))
                            recvIsBoxList = rlt->elementType &&
                                rlt->elementType->kind() == Type::Kind::Any;
                    }
                    if (recvIsBoxList &&
                        (att->attribute == "append" ||
                         att->attribute == "insert" ||
                         att->attribute == "remove"))
                        boxNestedContainerLiteralForAny(node.args[i].get());
                }
                continue;
            }
            // None is the null pointer - admissible for any ptr-shaped
            // param (nullable pattern); a Union arg may still need
            // narrowing the checker can't see. Both are skipped.
            if (ak == Type::Kind::None_ || ak == Type::Kind::Union ||
                pk == Type::Kind::Union)
                continue;
            // Dragon containers are INVARIANT, but a fresh literal arg of a
            // covariant element type (`main([SubTest()])` -> list[TestCase])
            // is sound and idiomatic: bless it via tryExpectedTypeLiteral,
            // which also forces the box representation for an Any element
            // type. For NON-literal same-kind container args, defer as
            // before - EXCEPT when the element layouts differ (list[T] vs
            // list[Any]/list[union]): passing a monomorphized list as a
            // box-element list walks the wrong stride inside the callee,
            // so that specific aliasing is rejected here.
            if (isContainer(ak) && ak == pk) {
                // An EMPTY container literal adopts the param type - the
                // same contextual typing `x: list[T] = []` gets - so
                // `f([])` builds the param's representation.
                if (auto* le = dynamic_cast<ListExpr*>(node.args[i].get())) {
                    if (le->elements.empty()) {
                        propagateAnnotationToEmptyLiteral(
                            node.args[i].get(), pt);
                        continue;
                    }
                } else if (auto* de =
                               dynamic_cast<DictExpr*>(node.args[i].get())) {
                    if (de->entries.empty()) {
                        propagateAnnotationToEmptyLiteral(
                            node.args[i].get(), pt);
                        continue;
                    }
                }
                if (tryExpectedTypeLiteral(node.args[i].get(), pt)) continue;
                if (ak == Type::Kind::List) {
                    const auto& ae =
                        static_cast<const ListType&>(*at).elementType;
                    const auto& pe =
                        static_cast<const ListType&>(*pt).elementType;
                    // Only judge when both element types are known - an
                    // Unknown element carries no representation claim.
                    if (ae && pe && ae->kind() != Type::Kind::Unknown &&
                        pe->kind() != Type::Kind::Unknown) {
                        auto boxElem = [](const Type::Kind k) {
                            return k == Type::Kind::Any ||
                                   k == Type::Kind::Union;
                        };
                        if (boxElem(ae->kind()) != boxElem(pe->kind())) {
                            error(node.args[i]->location(),
                                  "argument " + std::to_string(i + 1) +
                                  " of type '" + at->toString() +
                                  "' is not assignable to parameter type '" +
                                  pt->toString() + "'" +
                                  TypeChecker::listReprMismatchHint(*at, *pt));
                        }
                    }
                }
                continue;
            }
            if (!at->isSubtypeOf(*pt)) {
                error(node.args[i]->location(),
                      "argument " + std::to_string(i + 1) + " of type '" +
                      at->toString() + "' is not assignable to parameter "
                      "type '" + pt->toString() + "'");
            }
        }
    };

    if (calleeType->kind() == Type::Kind::Function) {
        auto& ft = static_cast<FunctionType&>(*calleeType);
        // len() of a function value: the argv[1]-class mistake one step
        // earlier (`len(argv)` instead of `len(argv())`). The builtin's Any
        // parameter skips the positional subtype check below, so without this
        // it typed int and miscompiled into a garbage length. Gated on the
        // builtin signature ([Any] -> int) so a user-defined `len` with a
        // concrete parameter still resolves through the normal checks.
        if (auto* lcn = dynamic_cast<NameExpr*>(node.callee.get())) {
            if (lcn->name == "len" && node.args.size() == 1 &&
                ft.paramTypes.size() == 1 && ft.paramTypes[0] &&
                ft.paramTypes[0]->kind() == Type::Kind::Any &&
                node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::Function) {
                if (auto* an = dynamic_cast<NameExpr*>(node.args[0].get())) {
                    error(node.location(), "len() of the function '" + an->name +
                          "'; call it first: 'len(" + an->name + "())'");
                } else {
                    error(node.location(), "len() of a function value; call it first");
                }
                node.type = impl_->intType;
                return;
            }
        }
        // M1/M2: validate arity + kwargs for a function (NameExpr) or method
        // (AttributeExpr) call before the type check below (which assumes exact
        // arity). On error, stop here with the declared return type.
        {
            std::string dispName;
            bool isBareName = false;
            if (auto* cn = dynamic_cast<NameExpr*>(node.callee.get())) {
                dispName = "function '" + cn->name + "'";
                isBareName = true;
            } else if (auto* ae = dynamic_cast<AttributeExpr*>(node.callee.get())) {
                dispName = "method '" + ae->attribute + "'";
            }
            // Skip a bare-name call that resolved to a METHOD signature: it's a
            // name-resolution artifact (e.g. socket.dr's method `close()`
            // shadowing the module extern `close(fd)` in class scope), not a
            // real call to the method. Genuine `obj.method()` (AttributeExpr)
            // calls are still validated.
            if (!dispName.empty() && !(isBareName && ft.isMethod) &&
                validateCall(ft, dispName)) {
                node.type = ft.returnType;
                return;
            }
        }
        // Conservative positional argument type check. FunctionType carries no
        // default/vararg metadata, so we only check a plain function reference
        // (NameExpr callee) or a method call (AttributeExpr callee) invoked
        // with no kwargs and EXACT arity; a defaulted or variadic call has a
        // mismatched count and is skipped. Any/Unknown on either side is also
        // skipped (can't prove a violation). This catches the silent miscompile
        // `def sq(n: int)...; sq("x")` AND `db.query(some_str)` where query wants
        // a template[SQL] (unchecked, the str->SQL mismatch reaches codegen
        // and segfaults) without misfiring on valid subtype passes
        // (InstanceType::isSubtypeOf walks the class hierarchy; int<:float etc.).
        // Method FunctionTypes exclude `self`, so paramTypes.size() == arg count.
        if ((dynamic_cast<NameExpr*>(node.callee.get()) ||
             dynamic_cast<AttributeExpr*>(node.callee.get())) &&
            node.kwArgs.empty() && node.args.size() == ft.paramTypes.size()) {
            checkPositionalArgs(ft);
        }
        // sorted()/reversed() are declared returning `any`, but they preserve
        // the iterable's element type. Propagate it here (this block runs
        // before the name-based builtin table) so `list(sorted(xs))` stays
        // list[T] and assigns to a typed binding instead of collapsing to Any.
        if (auto* cn = dynamic_cast<NameExpr*>(node.callee.get())) {
            if ((cn->name == "sorted" || cn->name == "reversed") &&
                node.args.size() == 1 && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::List) {
                node.type = std::make_shared<ListType>(
                    static_cast<ListType&>(*node.args[0]->type).elementType);
                return;
            }
            // map(f, xs) -> list[f.returnType]. Codegen lowers it by desugaring
            // to a list comprehension [f(x) for x in xs]; the result element
            // type is the callable's return type (fall back to Any for a
            // Callable-typed arg whose return type isn't statically known).
            if (cn->name == "map" && node.args.size() == 2) {
                std::shared_ptr<Type> elem = impl_->anyType;
                if (node.args[0]->type &&
                    node.args[0]->type->kind() == Type::Kind::Function) {
                    auto rt = static_cast<FunctionType&>(*node.args[0]->type).returnType;
                    if (rt) elem = rt;
                }
                node.type = std::make_shared<ListType>(elem);
                return;
            }
            // filter(f, xs) -> list[T] where T is xs's element type (filter
            // never transforms the elements). Codegen desugars to a list comp
            // with a condition. Without this the declared `any` return collapsed
            // `list(filter(...))` to list[Any] and rejected a list[int] binding.
            if (cn->name == "filter" && node.args.size() == 2 &&
                node.args[1]->type &&
                node.args[1]->type->kind() == Type::Kind::List) {
                node.type = std::make_shared<ListType>(
                    static_cast<ListType&>(*node.args[1]->type).elementType);
                return;
            }
        }
        node.type = ft.returnType;
        return;
    }

    if (calleeType->kind() == Type::Kind::Class) {
        auto& ct = static_cast<ClassType&>(*calleeType);
        // M2: constructor arity check - only when the class has a single ctor
        // (an overloaded ctor set isn't representable in `methods`, which holds
        // one __init__ signature; checking it would false-positive on a call
        // matching a different overload). Skips kwargs spread (C9 handled above).
        if (ct.constructorCount <= 1) {
            auto initIt = ct.methods.find("__init__");
            if (initIt != ct.methods.end() && initIt->second &&
                initIt->second->kind() == Type::Kind::Function) {
                auto& ift = static_cast<FunctionType&>(*initIt->second);
                validateCall(ift, "class '" + ct.name + "' constructor");
                // Positional type check + literal blessing for ctor args,
                // same gates as the function/method path: no kwargs, exact
                // arity. Without this a ctor call skips both entirely -
                // `Cls([11, 22])` against `xs: list[Any]` compiles the
                // literal monomorphized and every element read in the ctor
                // walks the wrong stride.
                if (node.kwArgs.empty() &&
                    node.args.size() == ift.paramTypes.size())
                    checkPositionalArgs(ift);
            }
        }
        node.type = std::make_shared<InstanceType>(
            std::static_pointer_cast<ClassType>(calleeType));
        return;
    }

    // Calling an instance value: `obj()` dispatches to the class's `__call__`
    // dunder (Python parity; the reactive `Signal()` read uses this). The
    // result type is `__call__`'s return type - already concrete for a generic
    // instance, since instantiateGenericClass substitutes the class's type
    // args into every method signature. Walk the inheritance chain so an
    // inherited `__call__` resolves. Without this the call typed Unknown, and a
    // chained `obj()[k]` / `obj().field` lost the element/field type and
    // miscompiled (str-index fallback / silent 0).
    if (calleeType->kind() == Type::Kind::Instance) {
        auto& inst = static_cast<InstanceType&>(*calleeType);
        for (const ClassType* cls = inst.classType.get(); cls; ) {
            auto cit = cls->methods.find("__call__");
            if (cit != cls->methods.end() && cit->second &&
                cit->second->kind() == Type::Kind::Function) {
                node.type = static_cast<FunctionType&>(*cit->second).returnType;
                return;
            }
            cls = (cls->parentClass && cls->parentClass->kind() == Type::Kind::Class)
                      ? static_cast<const ClassType*>(cls->parentClass.get())
                      : nullptr;
        }
    }

    // Known builtin return types
    if (auto* name = dynamic_cast<NameExpr*>(node.callee.get())) {
        const auto& n = name->name;
        // Builtins returning str
        if (n == "chr" || n == "hex" || n == "oct" || n == "bin" || n == "str" ||
            n == "repr" || n == "ascii" || n == "format") {
            node.type = impl_->strType;
            return;
        }
        // Builtins returning int
        if (n == "ord" || n == "len" || n == "round" ||
            n == "hash" || n == "id" || n == "int" ||
            n == "__float_bits" || n == "__float32_bits") {
            node.type = impl_->intType;
            return;
        }
        // abs follows its argument's numeric type: abs(int)->int,
        // abs(float)->float (Python parity). A fixed int type would print a
        // float result through the int path.
        if (n == "abs") {
            if (!node.args.empty() && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::Float) {
                node.type = impl_->floatType;
            } else {
                node.type = impl_->intType;
            }
            return;
        }
        // Builtins returning float
        if (n == "float" || n == "__float_from_bits" || n == "__float32_from_bits") {
            node.type = impl_->floatType;
            return;
        }
        // Builtins returning bool
        if (n == "bool" || n == "isinstance" || n == "issubclass" ||
            n == "callable" || n == "hasattr" || n == "any" || n == "all" ||
            n == "__exc_matches") {
            node.type = impl_->boolType;
            return;
        }
        // Builtins returning list
        // list()/sorted()/reversed()/filter() preserve the iterable's element
        // type so the result assigns to a typed binding (`list[int] = list(...)`)
        // instead of being stuck at list[Any].
        if (n == "list" || n == "sorted" || n == "reversed") {
            std::shared_ptr<Type> elem = impl_->anyType;
            if (!node.args.empty() && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::List)
                elem = static_cast<ListType&>(*node.args[0]->type).elementType;
            node.type = std::make_shared<ListType>(elem ? elem : impl_->anyType);
            return;
        }
        // map/filter (need callback codegen - deferred) and enumerate/zip
        // (iterator of tuples) stay list[any] for now.
        if (n == "filter" || n == "enumerate" || n == "zip") {
            node.type = std::make_shared<ListType>(impl_->anyType);
            return;
        }
        // divmod(a, b) -> (quotient, remainder) tuple (Python parity). Must be
        // a TupleType, not list[any], or `print` uses list repr on the tuple
        // pointer and emits garbage.
        if (n == "divmod") {
            node.type = std::make_shared<TupleType>(
                std::vector<std::shared_ptr<Type>>{impl_->intType, impl_->intType});
            return;
        }
        // Builtins returning the argument type
        if (n == "min" || n == "max" || n == "sum") {
            if (!node.args.empty() && node.args[0]->type) {
                if (node.args[0]->type->kind() == Type::Kind::List) {
                    node.type = static_cast<ListType&>(*node.args[0]->type).elementType;
                } else {
                    node.type = node.args[0]->type;
                }
            } else {
                node.type = impl_->intType;
            }
            return;
        }
        // print returns None
        if (n == "print") {
            node.type = impl_->noneType;
            return;
        }
        // input returns str
        if (n == "input") {
            node.type = impl_->strType;
            return;
        }
        // (type(x) is typed via its defined FunctionType return -> str, in
        // TypeChecker.cpp; no override needed here.)
        // pow returns int
        if (n == "pow") {
            node.type = impl_->intType;
            return;
        }
        // range returns list[int] (simplified)
        if (n == "range") {
            node.type = std::make_shared<ListType>(impl_->intType);
            return;
        }
    }

    // Unknown/Any callable -- result is unknown
    node.type = impl_->unknownType;
}

void TypeChecker::visit(AttributeExpr& node) {
    auto objType = inferType(node.object.get());

    // D044 - unbounded-`T` restriction: a value of type parameter `T` may be
    // stored, passed, returned, and compared, but its members/methods cannot be
    // accessed (the checker can't prove they exist for every `T`). A bounded
    // `T: B` lifts this - access resolves against the bound `B` below.
    if (objType && objType->kind() == Type::Kind::TypeVar) {
        auto& tv = static_cast<TypeVarType&>(*objType);
        if (tv.bound) {
            // Bounds - a bounded `T` behaves like its bound. Resolve the member
            // against the bound type and fall through to the normal member-
            // access paths below (instance field/method walk, etc.). After
            // monomorphization the receiver is the concrete arg (a subtype of
            // the bound), so the member resolves there too.
            objType = tv.bound;
        } else {
            // D044 - an UNBOUNDED `T` exposes no members (the checker can't prove
            // they exist for every `T`); add a bound to call members on it.
            error(node.location(), "cannot access '" + node.attribute +
                  "' on a value of unbounded type parameter '" + tv.name +
                  "'; declare a bound (`" + tv.name +
                  ": SomeClass`) to access its members");
            node.type = impl_->unknownType;
            return;
        }
    }

    // Commandment #3: a member access on a statically-`Any` receiver cannot be
    // dispatched - Dragon has no duck typing, so the concrete type must be known
    // AT the access. Previously this fell through to `Unknown` and silently
    // miscompiled: a `Task[int]` stored in a bare `list` (= `list[Any]`) lost the
    // handle tag that drives `join()`, so `for t in tasks { t.join() }` returned
    // garbage instead of the worker's result. Reject it at compile time and point
    // at the annotation, rather than box-and-pray. (`Unknown` - a not-yet-inferred
    // forward/transient type - is intentionally NOT caught here; only real `Any`.)
    if (objType->kind() == Type::Kind::Any) {
        error(node.location(),
              "cannot access '" + node.attribute +
              "' on a value of type `Any`; annotate the concrete type (e.g. "
              "`list[Task[int]]` rather than a bare `list`) so the member "
              "resolves - Dragon does not dispatch members on `Any`");
        node.type = impl_->unknownType;
        return;
    }

    // `expr.__doc__` is `Optional[str]` (= `Union[str, None]`) regardless of
    // whether the base is a module, function, class, or instance - Python
    // parity. CodeGen lowers this to a niche-ptr load (D030/D031): non-null
    // points at a `.rodata` C string, null encodes None.
    if (node.attribute == "__doc__") {
        bool isDocTarget =
            objType->kind() == Type::Kind::Module    ||
            objType->kind() == Type::Kind::Function  ||
            objType->kind() == Type::Kind::Class     ||
            objType->kind() == Type::Kind::Instance;
        if (isDocTarget) {
            std::vector<std::shared_ptr<Type>> opt = {impl_->strType, impl_->noneType};
            node.type = std::make_shared<UnionType>(std::move(opt));
            return;
        }
    }

    // Module attribute access: `controllers.health.health_check` chains through
    // ModuleType nodes until it bottoms out at a function/class/const export.
    // Submodule lookup wins over export lookup so a package can re-export a
    // value with the same name as a sibling submodule without breaking the
    // submodule walk (matches Python's resolution order).
    if (objType->kind() == Type::Kind::Module) {
        auto& mt = static_cast<ModuleType&>(*objType);
        auto subIt = mt.submodules.find(node.attribute);
        if (subIt != mt.submodules.end()) {
            checkModuleNamePrivacy(mt, node.attribute, node.location());  // D045
            node.type = subIt->second;
            return;
        }
        auto expIt = mt.exports.find(node.attribute);
        if (expIt != mt.exports.end()) {
            checkModuleNamePrivacy(mt, node.attribute, node.location());  // D045
            node.type = expIt->second;
            return;
        }
        error(node.location(),
              "module '" + mt.name + "' has no attribute '" + node.attribute + "'");
        node.type = impl_->unknownType;
        return;
    }

    if (objType->kind() == Type::Kind::Instance) {
        auto& inst = static_cast<InstanceType&>(*objType);
        // Look up the attribute on the class AND its ancestors. An inherited-only
        // field - declared/set in a base ctor and never re-assigned in the
        // subclass - lives only in the base's `fields` map, so without the parent
        // walk node.type stayed unset. Downstream tag derivation (emitTagForExpr)
        // then fell back to TAG_INT, and boxing such a field into `Any` read a str
        // pointer as an int (and an unset field as 0 instead of None). D030 §5:
        // the static type is the source of truth, so it must see inherited fields.
        bool declared = false;
        const ClassType* declaringViaName = nullptr;  // D045: owner of a not-yet-typed field
        for (const ClassType* cls = inst.classType.get(); cls; ) {
            auto it = cls->fields.find(node.attribute);
            if (it != cls->fields.end()) {
                checkMemberPrivacy(cls, node.attribute, node.location());  // D045
                node.type = it->second;
                return;
            }
            auto mit = cls->methods.find(node.attribute);
            if (mit != cls->methods.end()) {
                checkMemberPrivacy(cls, node.attribute, node.location());  // D045
                node.type = mit->second;
                return;
            }
            if (cls->declaredFieldNames.count(node.attribute)) {
                declared = true;
                if (!declaringViaName) declaringViaName = cls;  // most-derived declarer
            }
            cls = (cls->parentClass && cls->parentClass->kind() == Type::Kind::Class)
                      ? static_cast<const ClassType*>(cls->parentClass.get())
                      : nullptr;
        }
        // D045 - a member that resolved only by NAME (declared, but its precise
        // type isn't inferred yet: an unannotated/forward field) still gets the
        // privacy check, so a private/protected field can't be reached just
        // because the type-inference order hasn't caught up.
        if (declared && declaringViaName)
            checkMemberPrivacy(declaringViaName, node.attribute, node.location());
        // Resolved nowhere with a precise type. If the name is not a declared
        // member of the class or any ancestor, it does not exist - a hard type
        // error for BOTH reads and calls, matching Python's AttributeError
        // (`'C' object has no attribute 'x'`) rather than JS-style silent
        // undefined. Member NAMES are collected syntactically up front (module
        // pre-pass), so a miss here is real regardless of declaration order.
        //
        // If the name IS a declared field whose precise type just isn't inferred
        // yet - a read inside the constructor before the post-body field
        // inference runs, or a forward/cross-class reference - fall through to
        // the existing default. We deliberately do NOT synthesize a placeholder
        // type into `fields` here: that map drives codegen's struct layout, and
        // an Any/unknown placeholder mis-lowers the slot (box vs native).
        if (!declared) {
            error(node.location(), "type '" + inst.classType->name +
                  "' has no attribute '" + node.attribute + "'");
            node.type = impl_->unknownType;
            return;
        }
    }

    // Static / classmethod (and class-level attribute) access via the class
    // NAME: `ClassName.member` - here the receiver is the class itself, not an
    // instance. Resolve the member from the ClassType's method/field maps so a
    // static factory carries its declared return type. Without this, a call
    // like `Inner.make(...)` / `TcpStream.open(...)` typed to Unknown, so a
    // field initialized by one (`self.sock = TcpStream.open(...)`) was never
    // typed, and chained access (`self.sock.fd`) couldn't resolve the struct
    // layout. The static type IS the truth (D030).
    if (objType->kind() == Type::Kind::Class) {
        auto& ct = static_cast<ClassType&>(*objType);
        auto mit = ct.methods.find(node.attribute);
        if (mit != ct.methods.end()) {
            checkMemberPrivacy(&ct, node.attribute, node.location());  // D045
            node.type = mit->second;
            return;
        }
        auto fit = ct.fields.find(node.attribute);
        if (fit != ct.fields.end()) {
            checkMemberPrivacy(&ct, node.attribute, node.location());  // D045
            node.type = fit->second;
            return;
        }
    }

    // Task[T] handle methods: join() unwraps to T, is_alive() -> bool.
    if (objType->kind() == Type::Kind::Task) {
        auto& task = static_cast<TaskType&>(*objType);
        if (node.attribute == "join") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, task.resultType);
            return;
        }
        if (node.attribute == "is_alive") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, impl_->boolType);
            return;
        }
        error(node.location(), "Task has no attribute '" + node.attribute + "'");
        node.type = impl_->unknownType;
        return;
    }

    // Lock handle methods (Python threading.Lock shape).
    if (objType->kind() == Type::Kind::Lock) {
        if (node.attribute == "acquire") {
            // acquire(blocking=True) -> bool: True once held; False when
            // blocking=False and the lock was already taken.
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, impl_->boolType);
            return;
        }
        if (node.attribute == "release" || node.attribute == "destroy") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, impl_->noneType);
            return;
        }
        error(node.location(), "Lock has no attribute '" + node.attribute + "'");
        node.type = impl_->unknownType;
        return;
    }

    // str methods
    if (objType->kind() == Type::Kind::Str) {
        // str.format() has no codegen lowering - it silently returned None
        // (a miscompile). Dragon's one obvious way to interpolate is the
        // f-string (`f"{x}"`), the documented flagship. Reject .format()
        // loudly rather than miscompile it. (Reversible: implement a
        // dragon_str_format runtime + the {}/{n}/{name}/spec mini-language
        // if full Python str.format() parity is later wanted.)
        if (node.attribute == "format") {
            error(node.location(),
                  "str.format() is not supported - use an f-string instead, "
                  "e.g. f\"{value}\" (Dragon's one obvious way to interpolate)");
            node.type = impl_->unknownType;
            return;
        }
        // Methods returning str
        if (node.attribute == "upper" || node.attribute == "lower" ||
            node.attribute == "strip" || node.attribute == "lstrip" ||
            node.attribute == "rstrip" || node.attribute == "replace" ||
            node.attribute == "join" ||
            node.attribute == "title" || node.attribute == "capitalize" ||
            node.attribute == "swapcase" || node.attribute == "center" ||
            node.attribute == "ljust" || node.attribute == "rjust" ||
            node.attribute == "zfill" || node.attribute == "removeprefix" ||
            node.attribute == "removesuffix" || node.attribute == "expandtabs" ||
            node.attribute == "casefold") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->anyType},
                impl_->strType);
            return;
        }
        // Methods returning int
        if (node.attribute == "find" || node.attribute == "index" ||
            node.attribute == "rfind" || node.attribute == "rindex" ||
            node.attribute == "count") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->intType);
            return;
        }
        // Methods returning bool
        if (node.attribute == "startswith" || node.attribute == "endswith" ||
            node.attribute == "isdigit" || node.attribute == "isalpha" ||
            node.attribute == "isalnum" || node.attribute == "isspace" ||
            node.attribute == "isupper" || node.attribute == "islower" ||
            node.attribute == "istitle" || node.attribute == "isnumeric" ||
            node.attribute == "isdecimal" || node.attribute == "isascii" ||
            node.attribute == "isidentifier" || node.attribute == "isprintable") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->boolType);
            return;
        }
        // L3: partition()/rpartition() return a 3-str TUPLE (Python parity), not
        // a list - must be a TupleType so print() uses tuple repr (parens), like
        // divmod above. The runtime builds a DragonTuple to match.
        if (node.attribute == "partition" || node.attribute == "rpartition") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                std::make_shared<TupleType>(std::vector<std::shared_ptr<Type>>{
                    impl_->strType, impl_->strType, impl_->strType}));
            return;
        }
        // Methods returning list[str]
        if (node.attribute == "split" || node.attribute == "rsplit" ||
            node.attribute == "splitlines") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                std::make_shared<ListType>(impl_->strType));
            return;
        }
        // encode() returns bytes
        if (node.attribute == "encode") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->bytesType);
            return;
        }
    }

    // Bytes methods - mirror the codegen dispatch in CallMethods.cpp. Without a
    // result type here the call node's `type` stays null, so boxing it into Any
    // (assertEqual, dict/list elements, Union params) falls back to TAG_INT and
    // a pointer-returning method like `b.decode()` renders as a raw integer.
    if (objType->kind() == Type::Kind::Bytes) {
        // -> str
        if (node.attribute == "decode" || node.attribute == "hex") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->strType);
            return;
        }
        // -> bytes (no-arg transforms)
        if (node.attribute == "upper" || node.attribute == "lower" ||
            node.attribute == "strip" || node.attribute == "lstrip" ||
            node.attribute == "rstrip") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                impl_->bytesType);
            return;
        }
        // -> bool predicates
        if (node.attribute == "isdigit" || node.attribute == "isalpha" ||
            node.attribute == "isalnum" || node.attribute == "isspace" ||
            node.attribute == "startswith" || node.attribute == "endswith") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->bytesType},
                impl_->boolType);
            return;
        }
        // -> int (positions / counts)
        if (node.attribute == "find" || node.attribute == "rfind" ||
            node.attribute == "count" || node.attribute == "index" ||
            node.attribute == "rindex") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->bytesType},
                impl_->intType);
            return;
        }
    }

    // List methods
    if (objType->kind() == Type::Kind::List) {
        auto& lt = static_cast<ListType&>(*objType);
        if (node.attribute == "append" || node.attribute == "insert" ||
            node.attribute == "remove") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{lt.elementType},
                impl_->noneType);
            return;
        }
        // extend takes another LIST of the element type, not a single element
        // (xs.extend(ys) / xs.extend([..])). Grouping it with append above
        // registered the parameter as `T`, so every extend() failed type-check
        // ("list[T] not assignable to T"). The codegen already lowers extend to
        // dragon_list_extend(self, other); only the signature was wrong.
        if (node.attribute == "extend") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{
                    std::make_shared<ListType>(lt.elementType)},
                impl_->noneType);
            return;
        }
        // `pop`/`popleft` (deque is modeled as ListType) return the ELEMENT type,
        // not Any. An unhandled popleft falls through to Any, which forces
        // boxing and breaks generic callers' type inference (cmd #3: an op
        // that knows its element type must never report Any).
        if (node.attribute == "pop" || node.attribute == "popleft") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                lt.elementType);
            return;
        }
        if (node.attribute == "sort" || node.attribute == "reverse" ||
            node.attribute == "clear") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                impl_->noneType);
            return;
        }
        if (node.attribute == "copy") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                std::make_shared<ListType>(lt.elementType));
            return;
        }
        // C4: set binary ops return a new set (sets are modeled as ListType,
        // like copy). Without a result type the inline call node is Unknown, so
        // `len(a.union(b))`/`print(a.union(b))` can't tell it's a set and fall
        // through to dragon_str_len / list repr. The arg is another set of the
        // same element type. (Lists don't define these; a list receiver calling
        // them still fails at codegen - sets and lists share the ListType model.)
        if (node.attribute == "union" || node.attribute == "intersection" ||
            node.attribute == "difference" || node.attribute == "symmetric_difference") {
            auto setT = std::make_shared<ListType>(lt.elementType);
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{setT}, setT);
            return;
        }
        if (node.attribute == "count" || node.attribute == "index") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{lt.elementType},
                impl_->intType);
            return;
        }
    }

    // Dict methods
    if (objType->kind() == Type::Kind::Dict) {
        auto& dt = static_cast<DictType&>(*objType);
        if (node.attribute == "get" || node.attribute == "pop" ||
            node.attribute == "setdefault") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{dt.keyType},
                dt.valueType);
            return;
        }
        if (node.attribute == "keys") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                std::make_shared<ListType>(dt.keyType));
            return;
        }
        if (node.attribute == "values") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                std::make_shared<ListType>(dt.valueType));
            return;
        }
        if (node.attribute == "items") {
            // list[tuple[K, V]] - feeds comprehension/for-loop unpack binding
            // (`{k: v for k, v in d.items()}`), which reads the tuple element
            // types to type each unpack var. Without this, items() typed
            // unknown and the comp produced dict[<unknown>, V] - rejecting a
            // correctly-annotated binding.
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                std::make_shared<ListType>(std::make_shared<TupleType>(
                    std::vector<std::shared_ptr<Type>>{dt.keyType,
                                                       dt.valueType})));
            return;
        }
        if (node.attribute == "clear" || node.attribute == "update") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                impl_->noneType);
            return;
        }
        if (node.attribute == "copy") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                std::make_shared<DictType>(dt.keyType, dt.valueType));
            return;
        }
        // A non-method attribute on a dict is dot-key access (`d.x` == `d["x"]`,
        // a sugar the runtime already supports). Type it as the VALUE type, not
        // Any (cmd #3: honest types) - so `ds[0].x` flows typed and a generic
        // caller can infer instead of seeing Any. (Method names above shadow a
        // same-named key in dot position, as before.)
        if (dt.valueType && dt.valueType->kind() != Type::Kind::Unknown) {
            node.type = dt.valueType;
            return;
        }
    }

    node.type = impl_->unknownType;
}

void TypeChecker::visit(SubscriptExpr& node) {
    auto objType = inferType(node.object.get());
    auto idxType = inferType(node.index.get());

    // D044 - unbounded-`T` restriction: a `T`-typed value can't be subscripted
    // (`t[i]`), since the checker can't prove `T` is indexable without a bound.
    if (objType && objType->kind() == Type::Kind::TypeVar) {
        auto& tv = static_cast<TypeVarType&>(*objType);
        if (tv.bound) {
            // Bounds - subscript resolves against the bound (e.g. a `list`/`dict`
            // bound, or a class bound defining `__getitem__`).
            objType = tv.bound;
        } else {
            error(node.location(), "cannot subscript a value of unbounded type "
                  "parameter '" + tv.name + "'; declare a bound (`" + tv.name +
                  ": SomeClass`) to subscript it");
            node.type = impl_->unknownType;
            return;
        }
    }

    // A function value has no elements. `argv[1]` (sys.argv is a FUNCTION;
    // the list is `argv()[1]`) must be rejected here - falling through to
    // Unknown compiles into a garbage read out of the function value. The
    // error message states the remedy.
    // An explicit generic instantiation (`first[int](xs)`) never gets
    // here: visit(CallExpr) consumes its SubscriptExpr callee before the
    // callee is visited as a value, so only real value subscripts remain.
    if (objType && objType->kind() == Type::Kind::Function) {
        if (auto* fname = dynamic_cast<NameExpr*>(node.object.get())) {
            error(node.location(), "cannot subscript the function '" + fname->name +
                  "'; call it first: '" + fname->name + "()[...]'");
        } else {
            error(node.location(), "cannot subscript a function value; call it first");
        }
        node.type = impl_->unknownType;
        return;
    }

    // Check if this is a slice operation
    bool isSlice = dynamic_cast<SliceExpr*>(node.index.get()) != nullptr;

    if (objType->kind() == Type::Kind::List) {
        if (isSlice) {
            // Slicing a list returns a list of the same element type
            node.type = objType;
        } else {
            node.type = static_cast<ListType&>(*objType).elementType;
        }
        return;
    }
    if (objType->kind() == Type::Kind::Dict) {
        auto& dt = static_cast<DictType&>(*objType);
        // Key-type check (read AND write - the assign path inferTypes the target
        // subscript too). A dict is monomorphic in its key type, so indexing a
        // dict[int,V] with a str (or dict[str,V] with an int) is a TYPE error,
        // not a runtime KeyError / late LLVM-verify crash. Skip when either side
        // is Unknown (stay conservative), when the key type is Any (accepts any
        // key), and for slices (a slice is not a key).
        if (!isSlice && idxType && dt.keyType &&
            idxType->kind() != Type::Kind::Unknown &&
            dt.keyType->kind() != Type::Kind::Unknown &&
            dt.keyType->kind() != Type::Kind::Any &&
            !idxType->isAssignableTo(*dt.keyType)) {
            error(node.location(), "dict has key type '" + dt.keyType->toString() +
                  "' but is indexed with '" + idxType->toString() + "'");
        }
        node.type = dt.valueType;
        return;
    }
    // TypedDict: subscript with string literal returns per-key type
    if (objType->kind() == Type::Kind::Instance) {
        auto& inst = static_cast<InstanceType&>(*objType);
        if (inst.classType && inst.classType->isTypedDict) {
            if (auto* strKey = dynamic_cast<StringLiteral*>(node.index.get())) {
                std::string key = strKey->value;
                // Strip quotes if present
                if (key.size() >= 2 && (key.front() == '"' || key.front() == '\''))
                    key = key.substr(1, key.size() - 2);
                auto it = inst.classType->fields.find(key);
                if (it != inst.classType->fields.end()) {
                    node.type = it->second;
                    return;
                }
                error(node.location(), "TypedDict '" + inst.classType->name +
                      "' has no key '" + key + "'");
            }
            node.type = impl_->unknownType;
            return;
        }
    }
    if (objType->kind() == Type::Kind::Tuple) {
        // Propagate per-index element type when index is a constant int.
        // Without this, codegen at the SubscriptExpr site can't know the
        // tuple element's kind, can't IntToPtr the i64 returned by
        // dragon_tuple_get back to a native pointer, and the assignment
        // RC-overwrite skips the borrowed-incref -> double-free on cleanup
        // when both the source tuple and the new local decref the same heap
        // pointer. D030 requires values flow at native types; this is the
        // typechecker side of that contract.
        auto& tt = static_cast<TupleType&>(*objType);
        if (auto* lit = dynamic_cast<IntegerLiteral*>(node.index.get())) {
            int64_t i = lit->value;
            if (i < 0) i += (int64_t)tt.elementTypes.size();
            if (i >= 0 && i < (int64_t)tt.elementTypes.size()) {
                node.type = tt.elementTypes[i];
                return;
            }
        }
        node.type = impl_->unknownType;
        return;
    }
    if (objType->kind() == Type::Kind::Str) {
        // Both str[i] and str[a:b] return str
        node.type = impl_->strType;
        return;
    }
    if (objType->kind() == Type::Kind::Bytes) {
        // bytes[a:b] slice -> bytes; bytes[i] index -> int (a byte, Python
        // parity). Without this the slice expr typed as `unknown`, so a method
        // chained on it (e.g. `b[a:b].decode()`) mis-dispatched / mis-boxed.
        node.type = isSlice ? impl_->bytesType : impl_->intType;
        return;
    }
    if (objType->kind() == Type::Kind::Any) {
        // Subscripting an Any value. The element type is statically
        // unknown, so the result is itself Any (a box); codegen lowers this to
        // dragon_box_subscript, which dispatches on the runtime tag. Mirrors
        // Python's `Any[...] -> Any`.
        node.type = objType;
        return;
    }

    node.type = impl_->unknownType;
}

void TypeChecker::visit(SliceExpr& node) {
    if (node.lower) inferType(node.lower.get());
    if (node.upper) inferType(node.upper.get());
    if (node.step) inferType(node.step.get());
    node.type = impl_->unknownType;
}

// Unify a literal's already-inferred element types into ONE concrete element
// type, so a homogeneous list/set monomorphizes to native storage (i64[]/f64[]/
// ptr[]) - commandment #1, the hot path is sacred. Takes the common type via
// subtyping (`[True, 1]` -> int when bool <: int), NOT just elements[0]: a mixed
// literal (`[1, "a"]`) would otherwise silently mistype as list[<first>] and
// store the others' bits wrong. No common concrete type -> Unknown, which the
// inferred-binding (`:=`) check rejects ("annotate"), while an explicit
// list[Any] annotation overrides this via tryExpectedTypeLiteral. An Unknown
// element is permissive (skipped) so one untyped element doesn't poison the rest.
static std::shared_ptr<Type> unifyLiteralElements(
    const std::vector<std::unique_ptr<Expr>>& elems,
    const std::shared_ptr<Type>& unknown) {
    std::shared_ptr<Type> u;
    for (const auto& e : elems) {
        auto t = e ? e->type : nullptr;
        if (!t || t->kind() == Type::Kind::Unknown) continue;
        if (!u) { u = t; continue; }
        // EXACT match required, not subtyping. A subtype unify would fold
        // `[1, 2.0]` to a numeric type, but the list-literal codegen does not
        // bit-coerce mismatched scalar elements (int bits read as f64 = garbage),
        // so a "widened" list silently miscompiles. Demanding one identical
        // element type keeps every inferred list/set on a native, correctly-built
        // monomorphic path; anything else is Unknown -> annotate (e.g. an explicit
        // `list[float]` / `list[Any]`). Subtype/numeric coercion is the
        // annotation's job, where codegen already coerces per the target type.
        if (!t->equals(*u)) return unknown;             // mixed -> ambiguous
    }
    return u ? u : unknown;
}

void TypeChecker::visit(ListExpr& node) {
    if (node.elements.empty()) {
        // If type was already set via contextual typing (annotation propagation), keep it
        if (!node.type || node.type->kind() == Type::Kind::Unknown)
            node.type = std::make_shared<ListType>(impl_->unknownType);
        return;
    }
    for (auto& e : node.elements) inferType(e.get());
    node.type = std::make_shared<ListType>(
        unifyLiteralElements(node.elements, impl_->unknownType));
}

void TypeChecker::visit(TupleExpr& node) {
    std::vector<std::shared_ptr<Type>> elemTypes;
    for (auto& e : node.elements) {
        elemTypes.push_back(inferType(e.get()));
    }
    node.type = std::make_shared<TupleType>(std::move(elemTypes));
}

void TypeChecker::visit(DictExpr& node) {
    if (node.entries.empty()) {
        if (!node.type || node.type->kind() == Type::Kind::Unknown)
            node.type = std::make_shared<DictType>(impl_->unknownType, impl_->unknownType);
        return;
    }
    // Infer the dict's key/value types from the first entry that pins them - an
    // explicit `k: v` OR a `**spread` source (whose dict type seeds K/V). Seeding
    // from spread sources is what makes a leading/all-spread literal (`{**d}`,
    // `{**a, **b}`) type correctly instead of `dict[<unknown>, <unknown>]`.
    std::shared_ptr<Type> keyType = impl_->unknownType;
    std::shared_ptr<Type> valType = impl_->unknownType;
    for (auto& [k, v] : node.entries) {
        if (k) { keyType = inferType(k.get()); valType = inferType(v.get()); break; }
        if (v) {  // spread entry `**src`: seed K/V from the source dict's types
            auto vt = inferType(v.get());
            if (vt && vt->kind() == Type::Kind::Dict) {
                auto& dt = static_cast<DictType&>(*vt);
                keyType = dt.keyType;
                valType = dt.valueType;
                break;
            }
        }
    }
    for (auto& [k, v] : node.entries) {
        if (k) {
            auto kt = inferType(k.get());
            // Dicts are monomorphic in their key type - every key must share one
            // type. A mixed literal like {1: .., "1": ..} would otherwise infer
            // only the first key's type and miscompile (the runtime dict can't
            // hold both int and str keys; codegen LLVM-verify-fails). Reject it
            // cleanly here. Skip when either side is Unknown or the inferred key
            // type is Any (a deliberately heterogeneous annotation).
            if (kt && keyType &&
                kt->kind() != Type::Kind::Unknown &&
                keyType->kind() != Type::Kind::Unknown &&
                keyType->kind() != Type::Kind::Any &&
                !kt->isAssignableTo(*keyType)) {
                error(k->location(), "dict literal mixes key types '" +
                      keyType->toString() + "' and '" + kt->toString() +
                      "' - a dict is monomorphic in its key type");
            }
        }
        if (v) inferType(v.get());
    }
    node.type = std::make_shared<DictType>(keyType, valType);
}

void TypeChecker::visit(SetExpr& node) {
    if (node.elements.empty()) {
        if (!node.type || node.type->kind() == Type::Kind::Unknown)
            node.type = std::make_shared<ListType>(impl_->unknownType);
        return;
    }
    for (auto& e : node.elements) inferType(e.get());
    node.type = std::make_shared<ListType>(
        unifyLiteralElements(node.elements, impl_->unknownType));
}

// Element type of an iterable: list[T]/set -> T, dict -> key type, else unknown.
// Comprehensions and for-stmts use this to type their loop variables.
static std::shared_ptr<Type> iterableElementType(
    const std::shared_ptr<Type>& iter,
    const std::shared_ptr<Type>& unknown) {
    if (!iter) return unknown;
    if (iter->kind() == Type::Kind::List) {
        return static_cast<ListType&>(*iter).elementType;
    }
    if (iter->kind() == Type::Kind::Dict) {
        return static_cast<DictType&>(*iter).keyType;
    }
    return unknown;
}

// Bind one or more comprehension/for-clause loop variables given an iterable
// type. Single var -> element type. Multi var (e.g. dict comp `for k, v in
// items.items()`) -> unpack from a tuple element when possible, else unknown.
void TypeChecker::bindCompLoopVars(
    const std::vector<std::string>& names,
    const std::shared_ptr<Type>& iterType) {
    auto elem = iterableElementType(iterType, impl_->unknownType);
    if (names.size() == 1) {
        impl_->define(names[0], elem);
        return;
    }
    if (elem && elem->kind() == Type::Kind::Tuple) {
        auto& tup = static_cast<TupleType&>(*elem);
        for (size_t i = 0; i < names.size(); ++i) {
            auto t = i < tup.elementTypes.size() ? tup.elementTypes[i]
                                                 : impl_->unknownType;
            impl_->define(names[i], t);
        }
        return;
    }
    for (auto& n : names) impl_->define(n, impl_->unknownType);
}

// Type-check the extra `for ... in ... if ...` clauses of a comprehension.
// Each clause runs in the same lexical scope opened by the outer comprehension
// so its loop variables remain visible to inner clauses and the body.
void TypeChecker::checkCompExtraClauses(std::vector<CompClause>& clauses) {
    for (auto& c : clauses) {
        auto cIter = c.iterable ? inferType(c.iterable.get()) : impl_->unknownType;
        bindCompLoopVars(c.varNames, cIter);
        if (c.condition) inferType(c.condition.get());
    }
}

void TypeChecker::visit(ListCompExpr& node) {
    impl_->pushScope();
    auto iterType = node.iterable ? inferType(node.iterable.get()) : impl_->unknownType;
    bindCompLoopVars({node.varName}, iterType);
    if (node.condition) inferType(node.condition.get());
    checkCompExtraClauses(node.extraClauses);
    if (node.element) {
        auto elemType = inferType(node.element.get());
        node.type = std::make_shared<ListType>(elemType);
    } else {
        node.type = std::make_shared<ListType>(impl_->unknownType);
    }
    impl_->popScope();
}

void TypeChecker::visit(DictCompExpr& node) {
    impl_->pushScope();
    auto iterType = node.iterable ? inferType(node.iterable.get()) : impl_->unknownType;
    bindCompLoopVars(node.varNames, iterType);
    if (node.condition) inferType(node.condition.get());
    checkCompExtraClauses(node.extraClauses);
    auto keyType = node.key ? inferType(node.key.get()) : impl_->unknownType;
    auto valType = node.value ? inferType(node.value.get()) : impl_->unknownType;
    node.type = std::make_shared<DictType>(keyType, valType);
    impl_->popScope();
}

void TypeChecker::visit(SetCompExpr& node) {
    impl_->pushScope();
    auto iterType = node.iterable ? inferType(node.iterable.get()) : impl_->unknownType;
    bindCompLoopVars({node.varName}, iterType);
    if (node.condition) inferType(node.condition.get());
    checkCompExtraClauses(node.extraClauses);
    if (node.element) {
        inferType(node.element.get());
    }
    // Set type is unparameterized today - the element kind is exercised but
    // not surfaced in the result type.
    node.type = impl_->unknownType;
    impl_->popScope();
}

void TypeChecker::visit(GeneratorExpr& node) {
    impl_->pushScope();
    auto iterType = node.iterable ? inferType(node.iterable.get()) : impl_->unknownType;
    bindCompLoopVars({node.varName}, iterType);
    if (node.condition) inferType(node.condition.get());
    checkCompExtraClauses(node.extraClauses);
    if (node.element) {
        auto elemType = inferType(node.element.get());
        node.type = std::make_shared<ListType>(elemType);
    } else {
        node.type = std::make_shared<ListType>(impl_->unknownType);
    }
    impl_->popScope();
}

void TypeChecker::visit(LambdaExpr& node) {
    std::vector<std::shared_ptr<Type>> paramTypes;
    for (auto& p : node.params) {
        auto pType = resolveType(p.type.get());
        paramTypes.push_back(pType);
    }
    auto retType = resolveType(node.returnType.get());
    node.type = std::make_shared<FunctionType>(paramTypes, retType);

    // Type-check the body, exactly as visit(FunctionDecl) does for a def.
    // Before this walk existed nothing inside a lambda was ever type-checked:
    // `bad: int = "boy"` compiled clean, and a generic method call in a
    // handler (`db.all[dict[str, Any]](sql)`) was never stamped/retargeted by
    // the D044 worklist - codegen then either errored loudly ("no codegen
    // dispatch path matched", the bare-inference form) or, worse, resolved
    // nothing and produced an empty default silently (the explicit-args
    // form). The guard runs the walk once per checker instance (a def body
    // gets the same once-per-walk cadence); node.type above is still set on
    // every visit.
    if (!impl_->checkedLambdaBodies.insert(&node).second) return;

    impl_->pushScope();
    impl_->returnTypeStack.push_back(retType);
    for (size_t i = 0; i < node.params.size(); ++i) {
        impl_->define(node.params[i].name, paramTypes[i]);
    }
    if (node.body) {
        // Expression lambda: the body IS the return value - same
        // Unknown-gated assignability check as visit(ReturnStmt).
        auto bodyType = inferType(node.body.get());
        if (retType->kind() != Type::Kind::Unknown &&
            bodyType->kind() != Type::Kind::Unknown &&
            !bodyType->isAssignableTo(*retType)) {
            error(node.location(), "lambda body type '" + bodyType->toString() +
                  "' does not match declared return type '" +
                  retType->toString() + "'");
        }
    }
    for (auto& s : node.bodyStmts) {
        s->accept(*this);
    }
    impl_->returnTypeStack.pop_back();
    impl_->popScope();
}

void TypeChecker::visit(IfExpr& node) {
    inferType(node.condition.get());

    // isinstance(N, T) narrowing for the ternary's branches. Mirrors the
    // codegen-side narrowing in Expressions.cpp:visit(IfExpr) so that
    // `expr.method() if isinstance(u, T) else u` (the canonical Python
    // pattern when u: T | OtherT) sees u as T in the then-branch and as
    // the union's other member in the else-branch. Without this, the
    // then-branch's method call types as Unknown and the result is
    // unassignable to the obvious target type - see binascii.unhexlify's
    // first attempt for the failing shape.
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

    std::string narrowName;
    std::shared_ptr<Type> narrowThenType;
    std::shared_ptr<Type> narrowElseType;
    if (auto* call = dynamic_cast<CallExpr*>(node.condition.get())) {
        if (auto* callee = dynamic_cast<NameExpr*>(call->callee.get())) {
            if (callee->name == "isinstance" && call->args.size() == 2) {
                if (auto* argName = dynamic_cast<NameExpr*>(call->args[0].get())) {
                    auto curType = impl_->lookup(argName->name);
                    auto narrowT = narrowedTypeFromExpr(call->args[1].get());
                    if (curType && narrowT && curType->kind() == Type::Kind::Union) {
                        narrowName = argName->name;
                        narrowThenType = narrowT;
                        narrowElseType = subtractFromUnion(curType, narrowT);
                    }
                }
            }
        }
    }

    std::shared_ptr<Type> thenType;
    std::shared_ptr<Type> elseType;
    if (!narrowName.empty()) {
        impl_->pushScope();
        impl_->define(narrowName, narrowThenType);
        thenType = inferType(node.thenExpr.get());
        impl_->popScope();
        impl_->pushScope();
        impl_->define(narrowName, narrowElseType ? narrowElseType : impl_->unknownType);
        elseType = inferType(node.elseExpr.get());
        impl_->popScope();
    } else {
        thenType = inferType(node.thenExpr.get());
        elseType = inferType(node.elseExpr.get());
    }

    // Result is union of both branches (collapses when equal)
    if (thenType->equals(*elseType)) {
        node.type = thenType;
    } else {
        std::vector<std::shared_ptr<Type>> types = {thenType, elseType};
        node.type = std::make_shared<UnionType>(std::move(types));
    }
}

void TypeChecker::visit(AwaitExpr& node) {
    auto opType = inferType(node.operand.get());
    if (opType && opType->kind() == Type::Kind::Task) {
        // await Task[T] -> T
        node.type = static_cast<TaskType&>(*opType).resultType;
    } else if (!opType || opType->kind() == Type::Kind::Unknown) {
        // Operand type couldn't be determined (unresolved callee, etc.) -
        // stay permissive rather than emit a spurious error.
        node.type = impl_->unknownType;
    } else {
        // Awaiting a non-Task is the ADR-016 error: a synchronous function
        // returns its value directly and has no Task to unwrap.
        error(node.location(),
              "'await' requires a Task expression (an 'async def' call or a "
              "'fire' handle), got '" + opType->toString() + "'");
        node.type = impl_->unknownType;
    }
}

void TypeChecker::visit(FireExpr& node) {
    if (node.operand) {
        // fire fn(args) -> Task[<result type of fn>]
        auto opType = inferType(node.operand.get());
        node.type = std::make_shared<TaskType>(
            opType ? opType : std::static_pointer_cast<Type>(impl_->unknownType));
        return;
    }
    // fire { block } -> Task[<block's return type>], or Task[None] if it
    // produces no value. Scan the block's top-level returns for the type.
    std::shared_ptr<Type> blockRet = impl_->noneType;
    for (auto& s : node.bodyStmts) {
        s->accept(*this);
        if (auto* ret = dynamic_cast<ReturnStmt*>(s.get())) {
            if (ret->value && ret->value->type) blockRet = ret->value->type;
        }
    }
    node.type = std::make_shared<TaskType>(blockRet);
}

void TypeChecker::visit(YieldExpr& node) {
    if (node.value) inferType(node.value.get());
    node.type = impl_->unknownType;
}

void TypeChecker::visit(StarredExpr& node) {
    if (node.value) inferType(node.value.get());
    node.type = impl_->unknownType;
}

//===----------------------------------------------------------------------===//
// Statement Visitors
//===----------------------------------------------------------------------===//

// Contextual typing: propagate an annotation type into an empty container literal
// so that `x: list[str] = []` types the RHS as list[str] instead of list[<unknown>].
// True if `t` is a container annotation whose element/value type is Any:
// list[Any] / set[Any] (both ListType) or dict[K, Any]. Such targets are
// backed by a box container at runtime, so a literal assigned to them must be
// retyped to force the box representation (see AnnAssignStmt / AssignStmt).

} // namespace dragon
