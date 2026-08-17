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

static bool aggregateElemSupported(const std::string& fn, Type::Kind ek) {
    bool numeric = ek == Type::Kind::Int || ek == Type::Kind::Float ||
                   ek == Type::Kind::Bool || ek == Type::Kind::Any ||
                   ek == Type::Kind::Unknown;
    return numeric || (fn != "sum" && ek == Type::Kind::Str);
}

void TypeChecker::visit(CallExpr& node) {
    auto expectedType = impl_->currentExpectedType;
    impl_->currentExpectedType = nullptr;

    if (auto* calleeName = dynamic_cast<NameExpr*>(node.callee.get())) {
        if (calleeName->name == "list" && node.args.size() == 1)
            impl_->rangeValueOkExprs.insert(node.args[0].get());
        if (calleeName->name == "range" &&
            !impl_->rangeValueOkExprs.count(&node)) {
            error(node.location(),
                  "range() has no runtime value here: use it as a loop "
                  "iterable or materialize it with list(range(...))");
        }
        if (calleeName->name == "isinstance" && node.args.size() == 2) {
            bool typeArgOk = false;
            if (auto* tn = dynamic_cast<NameExpr*>(node.args[1].get())) {
                typeArgOk = tn->name == "int" || tn->name == "float" ||
                            tn->name == "bool" || tn->name == "str" ||
                            tn->name == "bytes" || tn->name == "list" ||
                            tn->name == "dict" || tn->name == "tuple" ||
                            tn->name == "set" ||
                            impl_->typeNames.count(tn->name) != 0 ||
                            lookupTypeParam(tn->name) != nullptr;
            }
            if (!typeArgOk) {
                error(node.location(),
                      "isinstance's second argument must be a type or class "
                      "name known at compile time");
            }
        }
    }

    for (auto& arg : node.args) {
        inferType(arg.get());
    }
    for (auto& [name, arg] : node.kwArgs) {
        inferType(arg.get());
    }

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

    if (auto* tpCallee = dynamic_cast<NameExpr*>(node.callee.get())) {
        if (auto tv = lookupTypeParam(tpCallee->name)) {
            node.type = tv;
            return;
        }
    }

    const Expr* savedMethodOk = impl_->methodRefOkExpr;
    impl_->methodRefOkExpr = node.callee.get();
    auto calleeType = inferType(node.callee.get());
    impl_->methodRefOkExpr = savedMethodOk;

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
            std::vector<int> matched;
            std::vector<int> exactMatched;
            for (size_t ci = 0; ci < cands->size(); ++ci) {
                auto& ft = static_cast<FunctionType&>(*(*cands)[ci]);
                if (ft.paramTypes.size() != node.args.size()) continue;
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
            if (calleeIsTypedDict && !hasStarArg) {
                node.type = std::make_shared<InstanceType>(
                    std::static_pointer_cast<ClassType>(calleeType));
                return;
            }

            if (posAfterStar >= 0) {
                error(node.args[posAfterStar]->location(),
                      "positional argument after `*` spread is not allowed");
                node.type = impl_->anyType;
                return;
            }

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
                        paramTypes = ift.paramTypes;
                        requiredParams = ift.requiredParams;
                        targetVarArg = ift.hasVarArg;
                        haveMeta = ift.hasArgMeta;
                    }
                }
            } else {
                node.type = impl_->unknownType;
                return;
            }

            if (hasKwSpread) {
                std::shared_ptr<Type> kwElemType;
                if (calleeType->kind() == Type::Kind::Function) {
                    auto& ft = static_cast<FunctionType&>(*calleeType);
                    if (ft.hasKwArg && ft.hasArgMeta && !ft.paramTypes.empty())
                        kwElemType = ft.paramTypes.back();
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

            if (haveMeta && !targetVarArg) {
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
                }
            }
            node.type = retType ? retType : impl_->anyType;
            return;
        }
    }

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
            if (kw.first.empty()) continue;
            auto it = std::find(ft.paramNames.begin(), ft.paramNames.end(), kw.first);
            if (it == ft.paramNames.end()) {
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
        for (size_t i = 0; i < ft.requiredParams && i < nParams; ++i) {
            if (!filled[i]) {
                error(node.location(),
                      dispName + " missing required argument '" + ft.paramNames[i] + "'");
                err = true; break;
            }
        }
        return err;
    };

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
            if (ak == Type::Kind::None_ || ak == Type::Kind::Union ||
                pk == Type::Kind::Union)
                continue;
            if (isContainer(ak) && ak == pk) {
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
                if (pt->kind() == Type::Kind::Contract &&
                    at->kind() == Type::Kind::Instance) {
                    auto& ct = static_cast<ContractType&>(*pt);
                    auto& inst = static_cast<InstanceType&>(*at);
                    auto problems =
                        contractConformanceProblems(*inst.classType, ct);
                    std::string head =
                        "argument " + std::to_string(i + 1) + " of type '" +
                        at->toString() + "' is not assignable to parameter "
                        "type '" + pt->toString() + "'";
                    if (problems.empty()) {
                        std::string argName = "value";
                        if (auto* nm =
                                dynamic_cast<NameExpr*>(node.args[i].get()))
                            argName = nm->name;
                        std::string promise = ct.display;
                        if (!promise.empty() && promise.front() == '{')
                            promise = promise.substr(1, promise.size() - 2);
                        error(node.args[i]->location(), head + ". " +
                              inst.classType->name + " has a matching method "
                              "set but no declared conformance - cast at the "
                              "call site ('" + argName + " as " + ct.display +
                              "') or promise it on the class ('class " +
                              inst.classType->name + " -> " + promise + "')");
                    } else {
                        std::string msg = head + ":";
                        for (auto& pr : problems) msg += " " + pr + ";";
                        msg.pop_back();
                        error(node.args[i]->location(), msg);
                    }
                    continue;
                }
                error(node.args[i]->location(),
                      "argument " + std::to_string(i + 1) + " of type '" +
                      at->toString() + "' is not assignable to parameter "
                      "type '" + pt->toString() + "'");
            }
        }
    };

    if (calleeType->kind() == Type::Kind::Contract) {
        error(node.location(), "cannot construct a contract ('" +
              calleeType->toString() + "'); instantiate a conforming class "
              "instead");
        node.type = impl_->unknownType;
        return;
    }

    if (calleeType->kind() == Type::Kind::Function) {
        auto& ft = static_cast<FunctionType&>(*calleeType);
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
        {
            std::string dispName;
            bool isBareName = false;
            if (auto* cn = dynamic_cast<NameExpr*>(node.callee.get())) {
                dispName = "function '" + cn->name + "'";
                isBareName = true;
            } else if (auto* ae = dynamic_cast<AttributeExpr*>(node.callee.get())) {
                dispName = "method '" + ae->attribute + "'";
            }
            if (!dispName.empty() && !(isBareName && ft.isMethod) &&
                validateCall(ft, dispName)) {
                node.type = ft.returnType;
                return;
            }
        }
        if ((dynamic_cast<NameExpr*>(node.callee.get()) ||
             dynamic_cast<AttributeExpr*>(node.callee.get())) &&
            node.kwArgs.empty() && node.args.size() == ft.paramTypes.size()) {
            checkPositionalArgs(ft);
        }
        if (auto* cn = dynamic_cast<NameExpr*>(node.callee.get())) {
            if ((cn->name == "sorted" || cn->name == "reversed") &&
                node.args.size() == 1 && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::List) {
                node.type = std::make_shared<ListType>(
                    static_cast<ListType&>(*node.args[0]->type).elementType);
                return;
            }
            if ((cn->name == "min" || cn->name == "max") &&
                node.args.size() == 1 && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::List) {
                auto elem = static_cast<ListType&>(*node.args[0]->type).elementType;
                if (elem) {
                    if (!aggregateElemSupported(cn->name, elem->kind())) {
                        error(node.location(),
                              cn->name + "() is not supported for list[" +
                              elem->toString() + "] elements");
                    }
                    node.type = elem;
                    return;
                }
            }
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
        if (ct.constructorCount <= 1) {
            auto initIt = ct.methods.find("__init__");
            if (initIt != ct.methods.end() && initIt->second &&
                initIt->second->kind() == Type::Kind::Function) {
                auto& ift = static_cast<FunctionType&>(*initIt->second);
                validateCall(ift, "class '" + ct.name + "' constructor");
                if (node.kwArgs.empty() &&
                    node.args.size() == ift.paramTypes.size())
                    checkPositionalArgs(ift);
            }
        }
        node.type = std::make_shared<InstanceType>(
            std::static_pointer_cast<ClassType>(calleeType));
        return;
    }

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

    if (calleeType && !dynamic_cast<NameExpr*>(node.callee.get()) &&
        !dynamic_cast<AttributeExpr*>(node.callee.get())) {
        switch (calleeType->kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::Str:
        case Type::Kind::Bytes:
        case Type::Kind::None_:
        case Type::Kind::List:
        case Type::Kind::Dict:
        case Type::Kind::Set:
        case Type::Kind::Tuple:
        case Type::Kind::Instance:
            error(node.location(), "cannot call a value of type '" +
                                       calleeType->toString() + "'");
            node.type = impl_->unknownType;
            return;
        default:
            break;
        }
    }

    if (auto* name = dynamic_cast<NameExpr*>(node.callee.get())) {
        const auto& n = name->name;
        if (n == "chr" || n == "hex" || n == "oct" || n == "bin" || n == "str" ||
            n == "repr" || n == "ascii" || n == "format") {
            node.type = impl_->strType;
            return;
        }
        if (n == "ord" || n == "len" || n == "round" ||
            n == "hash" || n == "id" || n == "int" ||
            n == "__float_bits" || n == "__float32_bits") {
            node.type = impl_->intType;
            return;
        }
        if (n == "abs") {
            if (!node.args.empty() && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::Float) {
                node.type = impl_->floatType;
            } else {
                node.type = impl_->intType;
            }
            return;
        }
        if (n == "float" || n == "__float_from_bits" || n == "__float32_from_bits") {
            node.type = impl_->floatType;
            return;
        }
        if (n == "bool" || n == "isinstance" || n == "issubclass" ||
            n == "callable" || n == "hasattr" || n == "any" || n == "all" ||
            n == "__exc_matches") {
            node.type = impl_->boolType;
            return;
        }
        if (n == "list" || n == "sorted" || n == "reversed") {
            std::shared_ptr<Type> elem = impl_->anyType;
            if (!node.args.empty() && node.args[0]->type &&
                node.args[0]->type->kind() == Type::Kind::List)
                elem = static_cast<ListType&>(*node.args[0]->type).elementType;
            node.type = std::make_shared<ListType>(elem ? elem : impl_->anyType);
            return;
        }
        if (n == "filter" || n == "enumerate" || n == "zip") {
            node.type = std::make_shared<ListType>(impl_->anyType);
            return;
        }
        if (n == "divmod") {
            node.type = std::make_shared<TupleType>(
                std::vector<std::shared_ptr<Type>>{impl_->intType, impl_->intType});
            return;
        }
        if (n == "tuple" && node.args.size() == 1) {
            if (auto* le = dynamic_cast<ListExpr*>(node.args[0].get())) {
                std::shared_ptr<Type> elem = impl_->anyType;
                if (node.args[0]->type &&
                    node.args[0]->type->kind() == Type::Kind::List) {
                    auto et = static_cast<ListType&>(*node.args[0]->type).elementType;
                    if (et) elem = et;
                }
                node.type = std::make_shared<TupleType>(
                    std::vector<std::shared_ptr<Type>>(le->elements.size(), elem));
                return;
            }
        }
        if (n == "min" || n == "max" || n == "sum") {
            if (!node.args.empty() && node.args[0]->type) {
                if (node.args[0]->type->kind() == Type::Kind::List) {
                    auto elem = static_cast<ListType&>(*node.args[0]->type).elementType;
                    if (elem && !aggregateElemSupported(n, elem->kind())) {
                        error(node.location(),
                              n + "() is not supported for list[" + elem->toString() +
                              "] elements");
                    }
                    node.type = elem;
                } else {
                    node.type = node.args[0]->type;
                }
            } else {
                node.type = impl_->intType;
            }
            return;
        }
        if (n == "print") {
            node.type = impl_->noneType;
            return;
        }
        if (n == "input") {
            node.type = impl_->strType;
            return;
        }
        if (n == "pow") {
            node.type = impl_->intType;
            return;
        }
        if (n == "range") {
            node.type = std::make_shared<ListType>(impl_->intType);
            return;
        }
    }

    node.type = impl_->unknownType;
}

void TypeChecker::visit(AttributeExpr& node) {
    resolveAttributeExpr(node);
    if (!node.type || node.type->kind() != Type::Kind::Function) return;
    if (impl_->methodRefOkExpr == &node) return;
    auto objT = node.object ? node.object->type : nullptr;
    if (!objT) return;
    switch (objT->kind()) {
        case Type::Kind::Int:   case Type::Kind::Float: case Type::Kind::Bool:
        case Type::Kind::Str:   case Type::Kind::Bytes:
        case Type::Kind::List:  case Type::Kind::Dict:  case Type::Kind::Set:
        case Type::Kind::Tuple: case Type::Kind::Task:  case Type::Kind::Lock:
            error(node.location(), "method '" + node.attribute + "' of '" +
                  objT->toString() + "' is not a value; call it (`." +
                  node.attribute + "(...)`), or wrap it in a lambda to pass "
                  "it as a Callable");
            node.type = impl_->unknownType;
            break;
        default:
            break;
    }
}

void TypeChecker::visit(AsCastExpr& node) {
    auto opType = inferType(node.operand.get());
    auto ct = resolveContractSet(node.contracts, node.location(), true);
    if (!ct) { node.type = impl_->unknownType; return; }

    if (opType && opType->kind() == Type::Kind::Contract) {
        auto& src = static_cast<ContractType&>(*opType);
        for (auto* need : ct->atoms) {
            if (std::find(src.atoms.begin(), src.atoms.end(), need) ==
                src.atoms.end()) {
                error(node.location(), "'as' goes upward only: '" +
                      src.display + "' does not include contract '" +
                      need->name + "' - narrow with isinstance instead");
                node.type = impl_->unknownType;
                return;
            }
        }
        node.resolvedDecls = ct->atoms;
        node.type = ct;
        return;
    }

    if (!opType || opType->kind() != Type::Kind::Instance) {
        error(node.location(), "'as' asserts contract conformance on a class "
              "instance; the operand is '" +
              (opType ? opType->toString() : std::string("<unknown>")) + "'");
        node.type = impl_->unknownType;
        return;
    }

    auto& inst = static_cast<InstanceType&>(*opType);
    auto problems = contractConformanceProblems(*inst.classType, *ct);
    if (!problems.empty()) {
        std::string msg = "'" + inst.classType->name +
                          "' does not satisfy contract '" + ct->display + "'";
        for (auto& p : problems) msg += "; " + p;
        error(node.location(), msg);
        node.type = impl_->unknownType;
        return;
    }
    node.resolvedDecls = ct->atoms;
    if (inst.classType->decl) {
        auto& cc = inst.classType->decl->conformedContracts;
        for (auto* a : ct->atoms)
            if (std::find(cc.begin(), cc.end(), a) == cc.end())
                cc.push_back(a);
    }
    node.type = ct;
}

void TypeChecker::visit(ContractSetTypeExpr&) {}

void TypeChecker::resolveAttributeExpr(AttributeExpr& node) {
    const Expr* savedMethodOk = impl_->methodRefOkExpr;
    if (node.attribute == "__doc__")
        impl_->methodRefOkExpr = node.object.get();
    auto objType = inferType(node.object.get());
    impl_->methodRefOkExpr = savedMethodOk;

    if (objType && objType->kind() == Type::Kind::TypeVar) {
        auto& tv = static_cast<TypeVarType&>(*objType);
        if (tv.bound) {
            objType = tv.bound;
        } else {
            error(node.location(), "cannot access '" + node.attribute +
                  "' on a value of unbounded type parameter '" + tv.name +
                  "'; declare a bound (`" + tv.name +
                  ": SomeClass`) to access its members");
            node.type = impl_->unknownType;
            return;
        }
    }

    if (objType && objType->kind() == Type::Kind::Contract) {
        auto& ct = static_cast<ContractType&>(*objType);
        auto mIt = ct.methods.find(node.attribute);
        if (mIt != ct.methods.end()) {
            node.type = mIt->second;
            return;
        }
        error(node.location(), "contract '" + ct.display +
              "' declares no method '" + node.attribute + "'");
        node.type = impl_->unknownType;
        return;
    }

    if (objType->kind() == Type::Kind::Any) {
        error(node.location(),
              "cannot access '" + node.attribute +
              "' on a value of type `Any`; annotate the concrete type (e.g. "
              "`list[Task[int]]` rather than a bare `list`) so the member "
              "resolves - Dragon does not dispatch members on `Any`");
        node.type = impl_->unknownType;
        return;
    }

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

    if (objType->kind() == Type::Kind::Module) {
        auto& mt = static_cast<ModuleType&>(*objType);
        auto subIt = mt.submodules.find(node.attribute);
        if (subIt != mt.submodules.end()) {
            checkModuleNamePrivacy(mt, node.attribute, node.location());
            node.type = subIt->second;
            return;
        }
        auto expIt = mt.exports.find(node.attribute);
        if (expIt != mt.exports.end()) {
            checkModuleNamePrivacy(mt, node.attribute, node.location());
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
        bool declared = false;
        const ClassType* declaringViaName = nullptr;
        for (const ClassType* cls = inst.classType.get(); cls; ) {
            auto it = cls->fields.find(node.attribute);
            if (it != cls->fields.end()) {
                checkMemberPrivacy(cls, node.attribute, node.location());
                node.type = it->second;
                return;
            }
            auto mit = cls->methods.find(node.attribute);
            if (mit != cls->methods.end()) {
                checkMemberPrivacy(cls, node.attribute, node.location());
                if (impl_->methodRefOkExpr != &node) {
                    error(node.location(), "method '" + node.attribute +
                          "' of class '" + cls->name +
                          "' is not a value; call it (`." + node.attribute +
                          "(...)`), or wrap it in a lambda to pass it as a "
                          "Callable");
                    node.type = impl_->unknownType;
                    return;
                }
                node.type = mit->second;
                return;
            }
            if (cls->declaredFieldNames.count(node.attribute)) {
                declared = true;
                if (!declaringViaName) declaringViaName = cls;
            }
            cls = (cls->parentClass && cls->parentClass->kind() == Type::Kind::Class)
                      ? static_cast<const ClassType*>(cls->parentClass.get())
                      : nullptr;
        }
        if (declared && declaringViaName)
            checkMemberPrivacy(declaringViaName, node.attribute, node.location());
        if (!declared) {
            error(node.location(), "type '" + inst.classType->name +
                  "' has no attribute '" + node.attribute + "'");
            node.type = impl_->unknownType;
            return;
        }
    }

    if (objType->kind() == Type::Kind::Class) {
        auto& ct = static_cast<ClassType&>(*objType);
        bool declared = false;
        const ClassType* declaringViaName = nullptr;
        for (const ClassType* cls = &ct; cls; ) {
            auto mit = cls->methods.find(node.attribute);
            if (mit != cls->methods.end()) {
                checkMemberPrivacy(cls, node.attribute, node.location());
                if (impl_->methodRefOkExpr != &node) {
                    error(node.location(), "method '" + node.attribute +
                          "' of class '" + cls->name +
                          "' is not a value; call it (`" + ct.name + "." +
                          node.attribute + "(...)`), or wrap it in a lambda to "
                          "pass it as a Callable");
                    node.type = impl_->unknownType;
                    return;
                }
                node.type = mit->second;
                return;
            }
            auto fit = cls->fields.find(node.attribute);
            if (fit != cls->fields.end()) {
                checkMemberPrivacy(cls, node.attribute, node.location());
                node.type = fit->second;
                return;
            }
            if (cls->declaredFieldNames.count(node.attribute)) {
                declared = true;
                if (!declaringViaName) declaringViaName = cls;
            }
            cls = (cls->parentClass && cls->parentClass->kind() == Type::Kind::Class)
                      ? static_cast<const ClassType*>(cls->parentClass.get())
                      : nullptr;
        }
        if (declared && declaringViaName)
            checkMemberPrivacy(declaringViaName, node.attribute, node.location());
        if (!declared) {
            error(node.location(), "class '" + ct.name +
                  "' has no attribute '" + node.attribute + "'");
            node.type = impl_->unknownType;
            return;
        }
    }

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

    if (objType->kind() == Type::Kind::Lock) {
        if (node.attribute == "acquire") {
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

    if (objType->kind() == Type::Kind::Str) {
        if (node.attribute == "format") {
            error(node.location(),
                  "str.format() is not supported - use an f-string instead, "
                  "e.g. f\"{value}\" (Dragon's one obvious way to interpolate)");
            node.type = impl_->unknownType;
            return;
        }
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
        if (node.attribute == "find" || node.attribute == "index" ||
            node.attribute == "rfind" || node.attribute == "rindex" ||
            node.attribute == "count") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->intType);
            return;
        }
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
        if (node.attribute == "partition" || node.attribute == "rpartition") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                std::make_shared<TupleType>(std::vector<std::shared_ptr<Type>>{
                    impl_->strType, impl_->strType, impl_->strType}));
            return;
        }
        if (node.attribute == "split" || node.attribute == "rsplit" ||
            node.attribute == "splitlines") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                std::make_shared<ListType>(impl_->strType));
            return;
        }
        if (node.attribute == "encode") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->bytesType);
            return;
        }
    }

    if (objType->kind() == Type::Kind::Bytes) {
        if (node.attribute == "decode" || node.attribute == "hex") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->strType},
                impl_->strType);
            return;
        }
        if (node.attribute == "upper" || node.attribute == "lower" ||
            node.attribute == "strip" || node.attribute == "lstrip" ||
            node.attribute == "rstrip") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{},
                impl_->bytesType);
            return;
        }
        if (node.attribute == "isdigit" || node.attribute == "isalpha" ||
            node.attribute == "isalnum" || node.attribute == "isspace" ||
            node.attribute == "startswith" || node.attribute == "endswith") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->bytesType},
                impl_->boolType);
            return;
        }
        if (node.attribute == "find" || node.attribute == "rfind" ||
            node.attribute == "count" || node.attribute == "index" ||
            node.attribute == "rindex") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{impl_->bytesType},
                impl_->intType);
            return;
        }
    }

    if (objType->kind() == Type::Kind::List) {
        auto& lt = static_cast<ListType&>(*objType);
        if (node.attribute == "append" || node.attribute == "insert" ||
            node.attribute == "remove") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{lt.elementType},
                impl_->noneType);
            return;
        }
        if (node.attribute == "extend") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{
                    std::make_shared<ListType>(lt.elementType)},
                impl_->noneType);
            return;
        }
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

    if (objType->kind() == Type::Kind::Set) {
        auto& st = static_cast<SetType&>(*objType);
        auto setT = std::make_shared<SetType>(st.elementType);
        if (node.attribute == "union" || node.attribute == "intersection" ||
            node.attribute == "difference" ||
            node.attribute == "symmetric_difference") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{setT}, setT);
            return;
        }
        if (node.attribute == "copy") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, setT);
            return;
        }
        if (node.attribute == "add" || node.attribute == "remove" ||
            node.attribute == "discard") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{st.elementType},
                impl_->noneType);
            return;
        }
        if (node.attribute == "clear") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, impl_->noneType);
            return;
        }
        if (node.attribute == "issubset" || node.attribute == "issuperset" ||
            node.attribute == "isdisjoint") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{setT}, impl_->boolType);
            return;
        }
        if (node.attribute == "pop") {
            node.type = std::make_shared<FunctionType>(
                std::vector<std::shared_ptr<Type>>{}, st.elementType);
            return;
        }
    }

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

    if (objType && objType->kind() == Type::Kind::TypeVar) {
        auto& tv = static_cast<TypeVarType&>(*objType);
        if (tv.bound) {
            objType = tv.bound;
        } else {
            error(node.location(), "cannot subscript a value of unbounded type "
                  "parameter '" + tv.name + "'; declare a bound (`" + tv.name +
                  ": SomeClass`) to subscript it");
            node.type = impl_->unknownType;
            return;
        }
    }

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

    bool isSlice = dynamic_cast<SliceExpr*>(node.index.get()) != nullptr;

    if (objType->kind() == Type::Kind::List) {
        if (isSlice) {
            node.type = objType;
        } else {
            node.type = static_cast<ListType&>(*objType).elementType;
        }
        return;
    }
    if (objType->kind() == Type::Kind::Dict) {
        auto& dt = static_cast<DictType&>(*objType);
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
    if (objType->kind() == Type::Kind::Instance) {
        auto& inst = static_cast<InstanceType&>(*objType);
        if (inst.classType && inst.classType->isTypedDict) {
            if (auto* strKey = dynamic_cast<StringLiteral*>(node.index.get())) {
                std::string key = strKey->value;
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
        // Propagate per-index element type for a constant int index; without this,
        // codegen can't IntToPtr the returned i64 correctly, and the RC-overwrite skips a needed incref, causing a double-free.
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
        node.type = impl_->strType;
        return;
    }
    if (objType->kind() == Type::Kind::Bytes) {
        node.type = isSlice ? impl_->bytesType : impl_->intType;
        return;
    }
    if (objType->kind() == Type::Kind::Any) {
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

static std::shared_ptr<Type> unifyLiteralElements(
    const std::vector<std::unique_ptr<Expr>>& elems,
    const std::shared_ptr<Type>& unknown) {
    std::shared_ptr<Type> u;
    for (const auto& e : elems) {
        auto t = e ? e->type : nullptr;
        if (!t || t->kind() == Type::Kind::Unknown) continue;
        if (!u) { u = t; continue; }
        if (!t->equals(*u)) return unknown;
    }
    return u ? u : unknown;
}

void TypeChecker::visit(ListExpr& node) {
    if (node.elements.empty()) {
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
    std::shared_ptr<Type> keyType = impl_->unknownType;
    std::shared_ptr<Type> valType = impl_->unknownType;
    for (auto& [k, v] : node.entries) {
        if (k) { keyType = inferType(k.get()); valType = inferType(v.get()); break; }
        if (v) {
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
            node.type = std::make_shared<SetType>(impl_->unknownType);
        return;
    }
    for (auto& e : node.elements) inferType(e.get());
    node.type = std::make_shared<SetType>(
        unifyLiteralElements(node.elements, impl_->unknownType));
}

static std::shared_ptr<Type> iterableElementType(
    const std::shared_ptr<Type>& iter,
    const std::shared_ptr<Type>& unknown) {
    if (!iter) return unknown;
    if (iter->kind() == Type::Kind::List) {
        return static_cast<ListType&>(*iter).elementType;
    }
    if (iter->kind() == Type::Kind::Set) {
        return static_cast<SetType&>(*iter).elementType;
    }
    if (iter->kind() == Type::Kind::Dict) {
        return static_cast<DictType&>(*iter).keyType;
    }
    return unknown;
}

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

void TypeChecker::checkCompExtraClauses(std::vector<CompClause>& clauses) {
    for (auto& c : clauses) {
        if (c.iterable) impl_->rangeValueOkExprs.insert(c.iterable.get());
        auto cIter = c.iterable ? inferType(c.iterable.get()) : impl_->unknownType;
        bindCompLoopVars(c.varNames, cIter);
        if (c.condition) inferType(c.condition.get());
    }
}

void TypeChecker::visit(ListCompExpr& node) {
    impl_->pushScope();
    if (node.iterable) impl_->rangeValueOkExprs.insert(node.iterable.get());
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
    if (node.iterable) impl_->rangeValueOkExprs.insert(node.iterable.get());
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
    if (node.iterable) impl_->rangeValueOkExprs.insert(node.iterable.get());
    auto iterType = node.iterable ? inferType(node.iterable.get()) : impl_->unknownType;
    bindCompLoopVars({node.varName}, iterType);
    if (node.condition) inferType(node.condition.get());
    checkCompExtraClauses(node.extraClauses);
    if (node.element) {
        node.type = std::make_shared<SetType>(inferType(node.element.get()));
    } else {
        node.type = std::make_shared<SetType>(impl_->unknownType);
    }
    impl_->popScope();
}

void TypeChecker::visit(GeneratorExpr& node) {
    impl_->pushScope();
    if (node.iterable) impl_->rangeValueOkExprs.insert(node.iterable.get());
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

    if (!impl_->checkedLambdaBodies.insert(&node).second) return;

    impl_->pushScope();
    impl_->returnTypeStack.push_back(retType);
    for (size_t i = 0; i < node.params.size(); ++i) {
        impl_->define(node.params[i].name, paramTypes[i]);
    }
    if (node.body) {
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
        node.type = static_cast<TaskType&>(*opType).resultType;
    } else if (!opType || opType->kind() == Type::Kind::Unknown) {
        node.type = impl_->unknownType;
    } else {
        error(node.location(),
              "'await' requires a Task expression (an 'async def' call or a "
              "'fire' handle), got '" + opType->toString() + "'");
        node.type = impl_->unknownType;
    }
}

void TypeChecker::visit(FireExpr& node) {
    if (node.operand) {
        auto opType = inferType(node.operand.get());
        if (opType && (opType->kind() == Type::Kind::Any ||
                       opType->kind() == Type::Kind::Union)) {
            error(node.location(), "cannot fire a call returning '" +
                  opType->toString() +
                  "': a task result crosses the spawn boundary monomorphized; "
                  "annotate the callee's concrete return type");
        }
        node.type = std::make_shared<TaskType>(
            opType ? opType : std::static_pointer_cast<Type>(impl_->unknownType));
        return;
    }
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

}
