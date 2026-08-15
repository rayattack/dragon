/// Dragon CodeGen - Class Declaration
#include "../CodeGenImpl.h"
#include "ClassesShared.h"

#include <functional>

namespace dragon {


void CodeGen::visit(ClassDecl& node) {
    // D044: a generic class template (`class Foo[T]`) has free type vars and is never
    // lowered; only its stamped monomorphic instantiations reach codegen here.
    if (!node.typeParams.empty()) return;

    // D044 cross-module generics: a stamped instantiation is owned by its defining
    // module, not the instantiation site; currentModuleName is overridden here and restored on every return path.
    const std::string _savedModuleForGeneric = impl_->currentModuleName;
    struct RestoreModule {
        std::string* slot; std::string saved; bool active;
        ~RestoreModule() { if (active) *slot = saved; }
    } _restoreModule{&impl_->currentModuleName, _savedModuleForGeneric,
                     !node.genericHomeModule.empty()};
    if (!node.genericHomeModule.empty())
        impl_->currentModuleName = node.genericHomeModule;

    // Per-module class symbol: the key for EVERY metadata map below and the
    // prefix for every class-owned LLVM symbol.
    const std::string clsSym = Impl::mangleClass(impl_->currentModuleName, node.name);

    // TypedDict is backed by DragonDict at runtime, not a struct; check this
    // before classNames.insert or TypedDict gets treated as a struct class.
    {
        bool isTypedDict = false;
        for (auto& base : node.bases) {
            if (auto* bn = dynamic_cast<NameExpr*>(base.get()))
                if (bn->name == "TypedDict") isTypedDict = true;
        }
        if (isTypedDict) {
            impl_->typedDictClassesBySym.insert(clsSym);
            impl_->classOwningModule[node.name] = impl_->currentModuleName;

            // Collect field name -> VarKind from annotated fields in class body
            for (auto& stmt : node.body) {
                if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
                    if (auto* fieldName = dynamic_cast<NameExpr*>(ann->target.get())) {
                        auto fk = impl_->typeExprToTypeKind(ann->annotation.get());
                        impl_->typedDictFieldKindsBySym[clsSym][fieldName->name] = fk;
                    }
                }
            }

            // TypedDict "construction" is handled at CallExpr level (dict_literal or
            // key=val args become a tagged dict there), not here.
            return; // Skip normal class struct/method generation
        }
    }

    // Register class name for constructor dispatch (after TypedDict check)
    impl_->classNames.insert(node.name);
    impl_->classOwningModule[node.name] = impl_->currentModuleName;

    // Stash the class docstring so the descriptor_create call site can pass it
    // through to the runtime - powers `Cls.__doc__` / `instance.__doc__`.
    if (node.docstring)
        impl_->classDocstringsBySym[clsSym] = *node.docstring;

    // Methods are emitted inline here (not via visit(FunctionDecl)), so their
    // docstrings must be stashed here to power `instance.method.__doc__` (Attributes.cpp).
    for (auto& bodyStmt : node.body) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(bodyStmt.get())) {
            if (fd->docstring)
                impl_->methodDocstringsBySym[clsSym][fd->name] = *fd->docstring;
        }
    }

    // Track the parent for super(): accept both `Base` and `pkg.Base` (cross-module
    // inheritance); store the bare name, classOwningModule resolves its module later.
    if (!node.bases.empty()) {
        std::string baseBareName;
        if (auto* baseName = dynamic_cast<NameExpr*>(node.bases[0].get())) {
            baseBareName = baseName->name;
        } else if (auto* baseAttr = dynamic_cast<AttributeExpr*>(node.bases[0].get())) {
            baseBareName = baseAttr->attribute;
        }
        if (!baseBareName.empty()) {
            // Parent chain is sym -> sym: resolve the base in the defining
            // module's context (alias-aware) at registration time.
            impl_->classParentNamesBySym[clsSym] = impl_->classSymPrefix(baseBareName);
        }
    }

    // D024: collect user class decorators for later application at the class's
    // source position; `@dataclass`/`@NamedTuple` are handled below instead.
    if (!node.decorators.empty()) {
        std::vector<Expr*> userDecs;
        for (auto& dec : node.decorators) {
            if (auto* ne = dynamic_cast<NameExpr*>(dec.get())) {
                if (ne->name == "dataclass" || ne->name == "NamedTuple") continue;
            }
            userDecs.push_back(dec.get());
        }
        if (!userDecs.empty()) {
            impl_->decoratedClassesBySym.insert(clsSym);
            impl_->classDecoratorExprsBySym[clsSym] = std::move(userDecs);
        }
    }

    // @dataclass/NamedTuple synthesis already ran in forwardDeclareClasses; the
    // synthesized __init__ is now a regular member of node.body.

    // PEP 526 field decls (`names: list[str]`) populate classFieldListElemKinds so
    // `obj.names[0]` carries the right VarKind; without it subscript defaults to int.
    for (auto& bs : node.body) {
        auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get());
        if (!ann || ann->isStatic) continue;
        auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
        if (!tgt) continue;
        auto* generic = dynamic_cast<GenericTypeExpr*>(ann->annotation.get());
        if (!generic || generic->typeArgs.empty()) continue;
        auto* base = dynamic_cast<NamedTypeExpr*>(generic->base.get());
        if (!base) continue;
        const std::string& baseName = base->name;
        // D030 §5: derive Type::Kind directly from the annotation - no VarKind hop.
        Type::Kind ek = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
        if (baseName == "list" || baseName == "List") {
            impl_->classFieldListElemKindsBySym[clsSym][tgt->name] = ek;
            // For list[ClassName] also track the class name so iteration /
            // subscript can resolve attribute access on the elements.
            if (auto* elemNamed = dynamic_cast<NamedTypeExpr*>(generic->typeArgs[0].get())) {
                if (impl_->classNames.count(elemNamed->name))
                    impl_->classFieldListElemClassNameBySym[clsSym][tgt->name] = elemNamed->name;
            }
        } else if ((baseName == "dict" || baseName == "Dict") && generic->typeArgs.size() >= 2) {
            // `data: dict[K, V]` records key/value Type::Kind so `obj.data["k"]` routes
            // through the typed dict op and int-keyed dicts dispatch to dragon_dict_int_*.
            Type::Kind kk = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
            Type::Kind vk = impl_->typeExprToTypeKind(generic->typeArgs[1].get());
            impl_->classFieldDictKeyKindsBySym[clsSym][tgt->name] = kk;
            impl_->classFieldDictValueKindsBySym[clsSym][tgt->name] = vk;
        }
    }

    // Scan every __init__ overload for self.field assignments; the struct type
    // holds the UNION of all fields across all constructors.
    struct FieldInfo { std::string name; llvm::Type* type; Impl::VarKind kind; };
    std::vector<FieldInfo> fields;
    std::set<std::string> seenFields; // avoid duplicates

    std::vector<FunctionDecl*> allInitDecls;
    for (auto& stmt : node.body) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get())) {
            if (fd->name == "__init__") allInitDecls.push_back(fd);
        }
    }

    // Helper lambda to extract fields from a single __init__ body
    auto extractFields = [&](FunctionDecl* initDecl) {
        // Build param name->index map for RHS type inference from constructor params
        std::unordered_map<std::string, size_t> paramNameToIdx;
        for (size_t pi = 0; pi < initDecl->params.size(); ++pi) {
            paramNameToIdx[initDecl->params[pi].name] = pi;
        }

        // Map local-var -> (LLVM type, VarKind) from earlier typed AnnAssigns, so
        // `self.f = local` gets its real heap type; else it falls back to i64/Other and dangles once the local's scope exit decrefs it.
        std::unordered_map<std::string, std::pair<llvm::Type*, Impl::VarKind>> localTypes;
        for (auto& bodyStmt : initDecl->body) {
            if (auto* ann = dynamic_cast<AnnAssignStmt*>(bodyStmt.get())) {
                if (auto* nm = dynamic_cast<NameExpr*>(ann->target.get())) {
                    if (ann->annotation) {
                        localTypes[nm->name] = {
                            impl_->typeExprToLLVM(ann->annotation.get()),
                            impl_->typeExprToKind(ann->annotation.get())
                        };
                    }
                }
            }
        }

        // Pattern matchers for compound RHS expressions used as field seeds.
        // Returns (LLVM type, VarKind, true) if a heap kind was inferred.
        auto inferFromExpr = [&](Expr* value) -> std::tuple<llvm::Type*, Impl::VarKind, bool> {
            // `[seed] * N` or `N * [seed]` - list-multiplication. The result
            // is always a list at runtime.
            if (auto* bin = dynamic_cast<BinaryExpr*>(value)) {
                if (bin->op.type() == TokenType::STAR) {
                    bool lhsList = dynamic_cast<ListExpr*>(bin->left.get()) != nullptr;
                    bool rhsList = dynamic_cast<ListExpr*>(bin->right.get()) != nullptr;
                    if (lhsList || rhsList) {
                        return {impl_->i8PtrType, Impl::VarKind::List, true};
                    }
                }
            }
            // A NameExpr may be an earlier typed local or a ctor param; surface its
            // type so `self.inner = initial` tracks self.inner as a class pointer, not i64.
            if (auto* nm = dynamic_cast<NameExpr*>(value)) {
                auto lit = localTypes.find(nm->name);
                if (lit != localTypes.end() &&
                    Impl::isHeapKind(lit->second.second)) {
                    return {lit->second.first, lit->second.second, true};
                }
                auto pit = paramNameToIdx.find(nm->name);
                if (pit != paramNameToIdx.end()) {
                    auto& param = initDecl->params[pit->second];
                    if (param.type) {
                        llvm::Type* pt = impl_->typeExprToLLVM(param.type.get());
                        Impl::VarKind pk = impl_->typeExprToKind(param.type.get());
                        if (Impl::isHeapKind(pk)) {
                            return {pt, pk, true};
                        }
                    }
                }
            }
            // A Callable-typed RHS (e.g. `self.f = mk(10)`) seeds the field as a
            // refcounted Closure; without this it stayed VarKind::Other and every instance leaked its closure (+ env) since dealloc never decref'd it.
            if (value && value->type &&
                value->type->kind() == Type::Kind::Function) {
                return {impl_->i8PtrType, Impl::VarKind::Closure, true};
            }
            return {impl_->i64Type, Impl::VarKind::Other, false};
        };

        // Recursive walker: extract self-field assigns from this stmt and any nested
        // compound statement; without recursion, a field only assigned inside if/while/for gets no struct slot and reads resolve to index 0.
        std::function<void(Stmt*)> walkStmt = [&](Stmt* bodyStmtRaw) {
            if (!bodyStmtRaw) return;
            if (auto* ifs = dynamic_cast<IfStmt*>(bodyStmtRaw)) {
                for (auto& s : ifs->thenBody) walkStmt(s.get());
                for (auto& clause : ifs->elifClauses) {
                    for (auto& s : clause.second) walkStmt(s.get());
                }
                for (auto& s : ifs->elseBody) walkStmt(s.get());
                return;
            }
            if (auto* ws = dynamic_cast<WhileStmt*>(bodyStmtRaw)) {
                for (auto& s : ws->body) walkStmt(s.get());
                for (auto& s : ws->elseBody) walkStmt(s.get());
                return;
            }
            if (auto* fs = dynamic_cast<ForStmt*>(bodyStmtRaw)) {
                for (auto& s : fs->body) walkStmt(s.get());
                for (auto& s : fs->elseBody) walkStmt(s.get());
                return;
            }
            if (auto* ts = dynamic_cast<TryStmt*>(bodyStmtRaw)) {
                for (auto& s : ts->tryBody) walkStmt(s.get());
                for (auto& h : ts->handlers) {
                    for (auto& s : h.body) walkStmt(s.get());
                }
                for (auto& s : ts->elseBody) walkStmt(s.get());
                for (auto& s : ts->finallyBody) walkStmt(s.get());
                return;
            }
            if (auto* withs = dynamic_cast<WithStmt*>(bodyStmtRaw)) {
                for (auto& s : withs->body) walkStmt(s.get());
                return;
            }
            Stmt* bodyStmt = bodyStmtRaw;
            if (auto* assign = dynamic_cast<AssignStmt*>(bodyStmt)) {
                for (auto& target : assign->targets) {
                    if (auto* attrExpr = dynamic_cast<AttributeExpr*>(target.get())) {
                        if (auto* selfName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                            if (selfName->name == "self" && !seenFields.count(attrExpr->attribute)) {
                                seenFields.insert(attrExpr->attribute);
                                // Try to infer type from annotation, then from RHS, default to i64
                                llvm::Type* fieldType = impl_->i64Type;
                                auto fieldKind = Impl::VarKind::Other;
                                if (assign->typeAnnotation) {
                                    fieldType = impl_->typeExprToLLVM(assign->typeAnnotation.get());
                                    fieldKind = impl_->typeExprToKind(assign->typeAnnotation.get());
                                } else if (assign->value) {
                                    // RHS is a ctor param with a type annotation: use that type
                                    // (common pattern: self(name: str) { self.name = name }).
                                    if (auto* rhsName = dynamic_cast<NameExpr*>(assign->value.get())) {
                                        auto pit = paramNameToIdx.find(rhsName->name);
                                        if (pit != paramNameToIdx.end()) {
                                            auto& param = initDecl->params[pit->second];
                                            if (param.type) {
                                                fieldType = impl_->typeExprToLLVM(param.type.get());
                                                fieldKind = impl_->typeExprToKind(param.type.get());
                                                // A class-named param type: record it so `obj.field.x`
                                                // resolves the right struct layout; else resolveExprClassName returns "" and the load falls through to ConstantInt 0.
                                                if (fieldKind == Impl::VarKind::ClassInstance) {
                                                    if (auto* nt = dynamic_cast<NamedTypeExpr*>(param.type.get())) {
                                                        std::string cn = impl_->resolveAnnotationClassName(nt->name);
                                                        if (!cn.empty()) {
                                                            impl_->classFieldClassNameBySym[clsSym][attrExpr->attribute] = cn;
                                                        }
                                                    }
                                                }
                                                // Bug A: preserve the Callable signature so `obj.field(args)`
                                                // can build a real FunctionType and branch on the closure tag (capturing closure vs bare fn ptr).
                                                if (auto* cte = dynamic_cast<CallableTypeExpr*>(param.type.get())) {
                                                    impl_->classFieldCallableTypeBySym
                                                        [clsSym][attrExpr->attribute] =
                                                        impl_->callableTypeExprToFnType(cte);
                                                }
                                            }
                                        }
                                    }
                                    // A string-literal init (`self.x = ""`) must record kind Str, NOT
                                    // StrLiteral: isHeapKind(StrLiteral) is false, so a later `self.x = heapStr` skips incref/decref in storeWithRCOverwrite and the field UAFs.
                                    else if (dynamic_cast<StringLiteral*>(assign->value.get())) {
                                        auto* strLit = dynamic_cast<StringLiteral*>(assign->value.get());
                                        if (strLit->isBytes) {
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
                                        } else {
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::Str;
                                        }
                                    } else if (dynamic_cast<FloatLiteral*>(assign->value.get())) {
                                        fieldType = impl_->f64Type;
                                        fieldKind = Impl::VarKind::Float;
                                    } else if (dynamic_cast<BooleanLiteral*>(assign->value.get())) {
                                        fieldType = impl_->i1Type;
                                        fieldKind = Impl::VarKind::Bool;
                                    } else if (dynamic_cast<ListExpr*>(assign->value.get())) {
                                        fieldType = impl_->i8PtrType;
                                        fieldKind = Impl::VarKind::List;
                                    } else if (dynamic_cast<DictExpr*>(assign->value.get())) {
                                        fieldType = impl_->i8PtrType;
                                        fieldKind = Impl::VarKind::Dict;
                                    } else if (dynamic_cast<TupleExpr*>(assign->value.get())) {
                                        fieldType = impl_->i8PtrType;
                                        fieldKind = Impl::VarKind::Tuple;
                                    } else if (dynamic_cast<SetExpr*>(assign->value.get())) {
                                        fieldType = impl_->i8PtrType;
                                        fieldKind = Impl::VarKind::Set;
                                    } else if (auto* callExpr = dynamic_cast<CallExpr*>(assign->value.get())) {
                                        // RHS is a builtin type-name ctor or user function; D030: builtin
                                        // names map directly, user functions look up their declared return type in the LLVM module.
                                        if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                                            const std::string& cn = calleeName->name;
                                            if (cn == "bytes") {
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
                                            } else if (cn == "str") {
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::Str;
                                            } else if (cn == "list") {
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::List;
                                            } else if (cn == "dict") {
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::Dict;
                                            } else if (cn == "set") {
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::Set;
                                            } else if (cn == "tuple") {
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::Tuple;
                                            } else if (cn == "float") {
                                                fieldType = impl_->f64Type;
                                                fieldKind = Impl::VarKind::Float;
                                            } else if (cn == "bool") {
                                                fieldType = impl_->i1Type;
                                                fieldKind = Impl::VarKind::Bool;
                                            } else if (cn == "Lock" && !impl_->classNames.count("Lock")) {
                                                // Intrinsic Lock field (`self._lock = Lock()`): tag it "__Lock"
                                                // like the local-var path, or acquire()/release() silently no-op (found via the concurrent-mutation detector on Router._storage_lock).
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::Other;
                                                impl_->classFieldClassNameBySym[clsSym][attrExpr->attribute] = "__Lock";
                                            } else if (impl_->classNames.count(cn)) {
                                                // `self.x = Foo(args)`: track kind + class name so
                                                // `obj.x.field` resolves Foo's struct layout instead of falling through to ConstantInt 0.
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::ClassInstance;
                                                impl_->classFieldClassNameBySym[clsSym][attrExpr->attribute] = cn;
                                            } else if (auto* userFn = impl_->module->getFunction(cn)) {
                                                // User-defined function: trust its declared return type.
                                                fieldType = userFn->getReturnType();
                                                if (fieldType == impl_->f64Type) fieldKind = Impl::VarKind::Float;
                                                else if (fieldType == impl_->i1Type) fieldKind = Impl::VarKind::Bool;
                                                else if (fieldType->isPointerTy()) {
                                                    // Disambiguate ptr-shaped kinds (bytes vs list vs str vs dict)
                                                    // via the AST type set by the TypeChecker.
                                                    if (callExpr->type) {
                                                        switch (callExpr->type->kind()) {
                                                            case Type::Kind::Bytes: fieldKind = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
                                                                break;
                                                            case Type::Kind::Str:   fieldKind = Impl::VarKind::Str;   break;
                                                            case Type::Kind::List:  fieldKind = Impl::VarKind::List;  break;
                                                            case Type::Kind::Dict:  fieldKind = Impl::VarKind::Dict;  break;
                                                            case Type::Kind::Tuple: fieldKind = Impl::VarKind::Tuple; break;
                                                            case Type::Kind::Set:   fieldKind = Impl::VarKind::Set;   break;
                                                            default:                fieldKind = Impl::VarKind::Other; break;
                                                        }
                                                    } else {
                                                        fieldKind = Impl::VarKind::Other;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                // Final fallback for compound RHS the dynamic_cast chain missed:
                                // fixes `self.x = [0] * N` and `self.x = local` (local: list[T]) without forcing every callsite to annotate.
                                if (fieldKind == Impl::VarKind::Other) {
                                    auto [t, k, ok] = inferFromExpr(assign->value.get());
                                    if (ok) { fieldType = t; fieldKind = k; }
                                }
                                // Last-ditch: lift the TypeChecker's inferred RHS type, catching
                                // `self.x = MODULE_CONST` where the const is typed but just a bare NameExpr at AST level.
                                if (fieldKind == Impl::VarKind::Other &&
                                    assign->value && assign->value->type) {
                                    switch (assign->value->type->kind()) {
                                        case Type::Kind::Str:
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::Str;
                                            break;
                                        case Type::Kind::Bytes:
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
                                            break;
                                        case Type::Kind::List:
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::List;
                                            break;
                                        case Type::Kind::Dict:
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::Dict;
                                            break;
                                        case Type::Kind::Tuple:
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::Tuple;
                                            break;
                                        case Type::Kind::Set:
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::Set;
                                            break;
                                        case Type::Kind::Float:
                                            fieldType = impl_->f64Type;
                                            fieldKind = Impl::VarKind::Float;
                                            break;
                                        case Type::Kind::Bool:
                                            fieldType = impl_->i1Type;
                                            fieldKind = Impl::VarKind::Bool;
                                            break;
                                        default: break;
                                    }
                                }
                                // D030: static type is authoritative. A static-factory init
                                // (`self.sock = TcpStream.open(...)`) slips past the direct-ctor check, leaving classFieldClassName unset and `self.sock.fd` reading the wrong offset; recover the class from the RHS InstanceType here.
                                if (assign->value && assign->value->type &&
                                    assign->value->type->kind() == Type::Kind::Instance &&
                                    !impl_->classFieldClassNameBySym[clsSym].count(attrExpr->attribute)) {
                                    if (auto* inst = dynamic_cast<InstanceType*>(assign->value->type.get())) {
                                        if (inst->classType && impl_->classNames.count(inst->classType->name)) {
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::ClassInstance;
                                            impl_->classFieldClassNameBySym[clsSym][attrExpr->attribute] = inst->classType->name;
                                        }
                                    }
                                }
                                fields.push_back({attrExpr->attribute, fieldType, fieldKind});
                            }
                        }
                    }
                }
            }
            // AnnAssignStmt: self.x: int = expr
            if (auto* annAssign = dynamic_cast<AnnAssignStmt*>(bodyStmt)) {
                if (auto* attrExpr = dynamic_cast<AttributeExpr*>(annAssign->target.get())) {
                    if (auto* selfName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                        if (selfName->name == "self" && !seenFields.count(attrExpr->attribute)) {
                            seenFields.insert(attrExpr->attribute);
                            llvm::Type* fieldType = impl_->i64Type;
                            auto fieldKind = Impl::VarKind::Other;
                            if (annAssign->annotation) {
                                fieldType = impl_->typeExprToLLVM(annAssign->annotation.get());
                                fieldKind = impl_->typeExprToKind(annAssign->annotation.get());
                                // Niche-opt `Class | None` lowers to a bare ptr that must read as
                                // ClassInstance of `Class`, or `obj.field.x`/`!= none` miscompile to const-0/dragon_str_eq; gated on ptr lowering so boxed unions are untouched.
                                if (fieldType == impl_->i8PtrType) {
                                    const std::string ucn =
                                        impl_->typeExprUnionClassName(annAssign->annotation.get());
                                    if (!ucn.empty()) {
                                        fieldKind = Impl::VarKind::ClassInstance;
                                        impl_->classFieldClassNameBySym[clsSym][attrExpr->attribute] = ucn;
                                    }
                                }
                                // Bug A: same Callable-field tracking for the
                                // explicit `self.handler: Callable[...]` form.
                                if (auto* cte = dynamic_cast<CallableTypeExpr*>(annAssign->annotation.get())) {
                                    impl_->classFieldCallableTypeBySym
                                        [clsSym][attrExpr->attribute] =
                                        impl_->callableTypeExprToFnType(cte);
                                }
                                // Annotated form of the intrinsic Lock field (`self._lock: Lock
                                // = Lock()`); same "__Lock" tagging as the cn == "Lock" ctor-scan branch above.
                                if (auto* lockNamed = dynamic_cast<NamedTypeExpr*>(
                                        annAssign->annotation.get())) {
                                    if (lockNamed->name == "Lock" &&
                                        !impl_->classNames.count("Lock")) {
                                        fieldType = impl_->i8PtrType;
                                        fieldKind = Impl::VarKind::Other;
                                        impl_->classFieldClassNameBySym
                                            [clsSym][attrExpr->attribute] = "__Lock";
                                    }
                                }
                                // `self.x: list[T] = ...`: record elem kind/class so for-in and
                                // subscript dispatch with the right LLVM type; else an empty list grown via append sizes its loop var as i64 and pointer payloads come back zero-shaped.
                                if (auto* generic = dynamic_cast<GenericTypeExpr*>(annAssign->annotation.get())) {
                                    if (auto* baseN = dynamic_cast<NamedTypeExpr*>(generic->base.get())) {
                                        if ((baseN->name == "list" || baseN->name == "List") &&
                                            !generic->typeArgs.empty()) {
                                            // D030 §5: direct Type::Kind from the annotation.
                                            Type::Kind ek = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
                                            impl_->classFieldListElemKindsBySym[clsSym][attrExpr->attribute] = ek;
                                            if (auto* elemNamed = dynamic_cast<NamedTypeExpr*>(generic->typeArgs[0].get())) {
                                                std::string cn = impl_->resolveAnnotationClassName(elemNamed->name);
                                                if (!cn.empty()) {
                                                    impl_->classFieldListElemClassNameBySym[clsSym][attrExpr->attribute] = cn;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            fields.push_back({attrExpr->attribute, fieldType, fieldKind});
                        }
                    }
                }
            }
        };  // end walkStmt lambda

        for (auto& bodyStmtUP : initDecl->body) {
            walkStmt(bodyStmtUP.get());
        }
    };

    // Extract fields from ALL __init__ overloads (union of all fields)
    for (auto* initDecl : allInitDecls) {
        extractFields(initDecl);
    }

    // Per-instance field defaults: (name, initializer Expr*) from value-bearing
    // non-static class-body decls; emitNewBody evaluates each fresh per instance. Expr* is borrowed from node.body, valid for this visit().
    std::vector<std::pair<std::string, Expr*>> perInstanceDefaults;

    // Every class-body field decl (`name: T`) gets a struct slot even if the ctor never
    // assigns it, else assignment elsewhere is silently dropped and reads segfault on null; a value-bearing decl (`x: int = 5`) also records its initializer for emitNewBody to evaluate fresh per instance (avoiding Python's mutable-default footgun; dataclass fields are exempt).
    for (auto& bs : node.body) {
        auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get());
        if (!ann || ann->isStatic || !ann->annotation) continue;
        auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
        if (!tgt) continue;
        // Record the per-instance default (read-only - emitNewBody visits the
        // node from each _new function; we do NOT move ann->value out).
        if (ann->value)
            perInstanceDefaults.push_back({tgt->name, ann->value.get()});
        llvm::Type* declType = impl_->typeExprToLLVM(ann->annotation.get());
        Impl::VarKind declKind = impl_->typeExprToKind(ann->annotation.get());
        // When the ctor already seeded this field with a less-precise kind (e.g.
        // `self.f = mk()` infers Other), the class-body annotation is authoritative: upgrade stale Other to the annotated heap kind, or a `f: Callable` field stays out of the destructor and every instance leaks its closure.
        if (seenFields.count(tgt->name)) {
            if (Impl::isHeapKind(declKind)) {
                for (auto& fld : fields) {
                    if (fld.name == tgt->name && fld.kind == Impl::VarKind::Other) {
                        fld.kind = declKind;
                        fld.type = declType;
                    }
                }
            } else if (declKind != Impl::VarKind::Other) {
                // Scalar annotations fix fld.type too (e.g. `price: float` seeded from
                // an untyped RHS stayed i64, corrupting the f64 bit pattern on read). fld.kind deliberately stays Other: upgrading to Int/Float/Bool enables acyclic skip-tracking, which surfaces a known call-arg-temp RC leak until the C3 plan fixes it first.
                for (auto& fld : fields) {
                    if (fld.name == tgt->name && fld.kind == Impl::VarKind::Other) {
                        fld.type = declType;
                    }
                }
            }
            continue;
        }
        seenFields.insert(tgt->name);
        fields.push_back({tgt->name, declType, declKind});
    }

    // Persist per-instance defaults so a subclass's emitNewBody can walk the parent
    // chain; recording here (not in emitNewBody) makes lookup order-independent even when a base class is declared after its subclass in source.
    impl_->classPerInstanceDefaultsBySym[clsSym] = perInstanceDefaults;

    // Inherit parent fields first, at the parent's indices (prefix-compatible layout);
    // without this, a subclass __init__ that relies on parent defaults produces a struct missing those fields, and access through a parent-typed ref reads/writes past the allocation (dragon_decref segfaults on garbage). Dedupe by name, keeping the parent's slot position.
    {
        auto parentIt = impl_->classParentNamesBySym.find(clsSym);
        if (parentIt != impl_->classParentNamesBySym.end()) {
            const std::string& parentName = parentIt->second;
            auto pIdxIt = impl_->classFieldIndicesBySym.find(parentName);
            auto pTyIt  = impl_->classFieldTypesBySym.find(parentName);
            auto pKindIt = impl_->classFieldKindsBySym.find(parentName);
            if (pIdxIt != impl_->classFieldIndicesBySym.end() &&
                pTyIt  != impl_->classFieldTypesBySym.end()) {
                // Recover the parent's field order from its name->index map.
                std::vector<std::pair<unsigned, std::string>> parentOrdered;
                for (auto& [fname, fidx] : pIdxIt->second)
                    parentOrdered.push_back({fidx, fname});
                std::sort(parentOrdered.begin(), parentOrdered.end());

                std::vector<FieldInfo> merged;
                std::set<std::string> mergedSeen;
                for (auto& [fidx, fname] : parentOrdered) {
                    (void)fidx;
                    // classFieldIndices/classFieldTypes must agree on the parent's field
                    // set; if they drift, diagnose and skip rather than throw from .at().
                    auto tIt = pTyIt->second.find(fname);
                    if (tIt == pTyIt->second.end()) {
                        impl_->addError(
                            "internal: missing field type for inherited '" +
                                fname + "' of parent '" + parentName +
                                "' while emitting class '" + node.name + "'",
                            node.location());
                        continue;
                    }
                    llvm::Type* ft = tIt->second;
                    Impl::VarKind fk = Impl::VarKind::Other;
                    if (pKindIt != impl_->classFieldKindsBySym.end()) {
                        auto kIt = pKindIt->second.find(fname);
                        if (kIt != pKindIt->second.end()) fk = kIt->second;
                    }
                    merged.push_back({fname, ft, fk});
                    mergedSeen.insert(fname);
                }
                // Append the subclass's OWN fields (those not inherited).
                for (auto& f : fields) {
                    if (mergedSeen.count(f.name)) continue;
                    merged.push_back(f);
                    mergedSeen.insert(f.name);
                }
                fields = std::move(merged);

                // Propagate parent list/dict elem-kind + class-name metadata so
                // inherited-field subscript/iteration resolves through the subclass too.
                auto copyMap = [&](auto& dst, auto& srcMap) {
                    auto sit = srcMap.find(parentName);
                    if (sit == srcMap.end()) return;
                    // Snapshot the parent's inner map first: dst[clsSym] may rehash
                    // and invalidate `sit` when dst == srcMap.
                    auto parentEntry = sit->second;
                    auto& dstEntry = dst[clsSym];
                    for (auto& kv : parentEntry)
                        if (!dstEntry.count(kv.first)) dstEntry[kv.first] = kv.second;
                };
                copyMap(impl_->classFieldListElemKindsBySym, impl_->classFieldListElemKindsBySym);
                copyMap(impl_->classFieldClassNameBySym, impl_->classFieldClassNameBySym);
            }
        }
    }

    // D030: a class-body annotation is authoritative. extractFields only records a
    // field's class from a direct ctor or class-typed param; a factory call or typed-local RHS leaves kind=Other/classFieldClassName unset, so `self.a.method()` resolves to ConstantInt 0. Re-derive declared class fields from their annotation here regardless of init form.
    for (auto& bs : node.body) {
        auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get());
        if (!ann || ann->isStatic || !ann->annotation) continue;
        auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
        if (!tgt) continue;
        auto* named = dynamic_cast<NamedTypeExpr*>(ann->annotation.get());
        if (!named) continue;  // generics handled by the scan above
        std::string cn = impl_->resolveAnnotationClassName(named->name);
        if (cn.empty()) continue;
        impl_->classFieldClassNameBySym[clsSym][tgt->name] = cn;
        for (auto& f : fields) {
            if (f.name == tgt->name) {
                f.type = impl_->i8PtrType;
                f.kind = Impl::VarKind::ClassInstance;
                break;
            }
        }
    }

    // For backward compat: single initDecl pointer (used in single-ctor path)
    FunctionDecl* initDecl = allInitDecls.empty() ? nullptr : allInitDecls[0];
    size_t ctorCount = allInitDecls.size();
    bool isMultiCtor = (ctorCount > 1);

    // Struct layout (D026 vtable + GC): with RC, header is index 0=refcount(i64,
    // init 1), 1=type_tag(i64, DRAGON_TAG_CLASS=7), 2=vtable ptr; user fields offset by 3. Without GC, fields start at index 0.
    unsigned headerOffset = (impl_->options.gcMode == GCMode::RC) ? 3 : 0;
    std::vector<llvm::Type*> fieldTypes;
    if (impl_->options.gcMode == GCMode::RC) {
        fieldTypes.push_back(impl_->i64Type);   // index 0: refcount
        fieldTypes.push_back(impl_->i64Type);   // index 1: type_tag + gc_flags (packed into i64)
        fieldTypes.push_back(impl_->i8PtrType); // index 2: vtable pointer (D026)
    }
    for (auto& f : fields) fieldTypes.push_back(f.type);
    // Reuse the struct type from the layout pre-pass: calling StructType::create
    // twice for the same class mints a duplicate named struct (`%Class.0`).
    llvm::StructType* structType = nullptr;
    if (auto it = impl_->classStructTypesBySym.find(clsSym);
        it != impl_->classStructTypesBySym.end()) {
        structType = it->second;
    } else {
        structType = llvm::StructType::create(*impl_->context, fieldTypes, clsSym);
        impl_->classStructTypesBySym[clsSym] = structType;
    }

    // Store field->index, field->type, and field->VarKind mappings (shifted by headerOffset for GC header)
    for (unsigned i = 0; i < fields.size(); ++i) {
        impl_->classFieldIndicesBySym[clsSym][fields[i].name] = i + headerOffset;
        impl_->classFieldTypesBySym[clsSym][fields[i].name] = fields[i].type;
        impl_->classFieldKindsBySym[clsSym][fields[i].name] = fields[i].kind;
    }
    // Own positional field order for match destructuring, via the same AST helper
    // the TypeChecker uses for ClassType::fieldOrder (keeps both stages' mapping identical).
    impl_->classFieldOrderBySym[clsSym] = instanceFieldOrder(node);

    // Track list element types for class fields (from constructor param type annotations)
    // This enables correct for-in iteration over self.field where field is list[str] etc.
    for (auto* initDecl : allInitDecls) {
        std::unordered_map<std::string, size_t> paramNameToIdx;
        for (size_t pi = 0; pi < initDecl->params.size(); ++pi) {
            paramNameToIdx[initDecl->params[pi].name] = pi;
        }
        // `self.x: list[Callable[[A], R]] = []` parses as AnnAssignStmt, which the
        // AssignStmt-with-typeAnnotation scan below misses.
        for (auto& bodyStmt : initDecl->body) {
            auto* annAssign = dynamic_cast<AnnAssignStmt*>(bodyStmt.get());
            if (!annAssign) continue;
            auto* attrExpr = dynamic_cast<AttributeExpr*>(annAssign->target.get());
            if (!attrExpr) continue;
            auto* selfName = dynamic_cast<NameExpr*>(attrExpr->object.get());
            if (!selfName || selfName->name != "self") continue;
            auto* generic = dynamic_cast<GenericTypeExpr*>(annAssign->annotation.get());
            if (!generic || generic->typeArgs.empty()) continue;
            auto* base = dynamic_cast<NamedTypeExpr*>(generic->base.get());
            if (!base) continue;
            if (base->name == "list") {
                // Record element kind so existing for-loop dispatch sees the field
                // as a typed list rather than defaulting to Int.
                auto elemVK = impl_->typeExprToKind(generic->typeArgs[0].get());
                Type::Kind ek = Type::Kind::Int;
                if (elemVK == Impl::VarKind::Str) ek = Type::Kind::Str;
                else if (elemVK == Impl::VarKind::Float) ek = Type::Kind::Float;
                else if (elemVK == Impl::VarKind::Bool) ek = Type::Kind::Bool;
                else if (elemVK == Impl::VarKind::ClassInstance) ek = Type::Kind::Instance;
                impl_->classFieldListElemKindsBySym[clsSym][attrExpr->attribute] = ek;
                // Callable element: stash FunctionType for ForLoop to register
                // callableTypes for the loop variable.
                if (auto* cte = dynamic_cast<CallableTypeExpr*>(generic->typeArgs[0].get())) {
                    impl_->classFieldListElemCallableTypeBySym
                        [clsSym][attrExpr->attribute] =
                        impl_->callableTypeExprToFnType(cte);
                }
            } else if (base->name == "dict" && generic->typeArgs.size() >= 2) {
                // Record value kind so `obj.field["k"]` routes through the typed dict op;
                // else it falls through to dragon_dict_get returning i64, mismatching PHI types when used in a ternary with a str/float/list.
                Type::Kind vk = impl_->typeExprToTypeKind(generic->typeArgs[1].get());
                impl_->classFieldDictValueKindsBySym[clsSym][attrExpr->attribute] = vk;
            }
        }
        for (auto& bodyStmt : initDecl->body) {
            auto* assign = dynamic_cast<AssignStmt*>(bodyStmt.get());
            if (!assign) continue;
            for (auto& target : assign->targets) {
                auto* attrExpr = dynamic_cast<AttributeExpr*>(target.get());
                if (!attrExpr) continue;
                auto* selfName = dynamic_cast<NameExpr*>(attrExpr->object.get());
                if (!selfName || selfName->name != "self") continue;
                auto fkIt = impl_->classFieldKindsBySym[clsSym].find(attrExpr->attribute);
                bool fieldIsList = (fkIt != impl_->classFieldKindsBySym[clsSym].end() && fkIt->second == Impl::VarKind::List);
                // Also check if the field is assigned from a function returning list[T]
                if (!fieldIsList && assign->value) {
                    if (auto* rhsCall = dynamic_cast<CallExpr*>(assign->value.get())) {
                        if (auto* cn = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                            // Search dep modules + entry module: classes commonly call
                            // helpers defined in the same file (e.g. _split_path in stdlib/http/server.dr).
                            std::vector<dragon::Module*> searchModules = impl_->depModulePtrs;
                            if (impl_->entryModulePtr)
                                searchModules.push_back(impl_->entryModulePtr);
                            for (auto* dep : searchModules) {
                                for (auto& ds : dep->body) {
                                    if (auto* fd = dynamic_cast<FunctionDecl*>(ds.get())) {
                                        if (fd->name == cn->name && fd->returnType) {
                                            if (auto* g = dynamic_cast<GenericTypeExpr*>(fd->returnType.get())) {
                                                if (auto* base = dynamic_cast<NamedTypeExpr*>(g->base.get())) {
                                                    if (base->name == "list") fieldIsList = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (!fieldIsList) continue;
                // Check if RHS is a parameter with list[T] type annotation
                if (auto* rhsName = dynamic_cast<NameExpr*>(assign->value.get())) {
                    auto pit = paramNameToIdx.find(rhsName->name);
                    if (pit != paramNameToIdx.end()) {
                        auto& param = initDecl->params[pit->second];
                        if (param.type) {
                            if (auto* generic = dynamic_cast<GenericTypeExpr*>(param.type.get())) {
                                if (!generic->typeArgs.empty()) {
                                    auto elemVK = impl_->typeExprToKind(generic->typeArgs[0].get());
                                    Type::Kind ek = Type::Kind::Int;
                                    if (elemVK == Impl::VarKind::Str) ek = Type::Kind::Str;
                                    else if (elemVK == Impl::VarKind::Float) ek = Type::Kind::Float;
                                    impl_->classFieldListElemKindsBySym[clsSym][attrExpr->attribute] = ek;
                                }
                            }
                        }
                    }
                }
                // Check if RHS is a function call with list[T] return type
                if (auto* rhsCall = dynamic_cast<CallExpr*>(assign->value.get())) {
                    if (auto* calleeName = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                        // Search dep modules + entry module: classes commonly call
                        // helpers defined in the same file (e.g. _split_path in stdlib/http/server.dr).
                        std::vector<dragon::Module*> searchModules = impl_->depModulePtrs;
                        if (impl_->entryModulePtr)
                            searchModules.push_back(impl_->entryModulePtr);
                        for (auto* dep : searchModules) {
                            for (auto& depStmt : dep->body) {
                                if (auto* fd = dynamic_cast<FunctionDecl*>(depStmt.get())) {
                                    if (fd->name == calleeName->name && fd->returnType) {
                                        if (auto* generic = dynamic_cast<GenericTypeExpr*>(fd->returnType.get())) {
                                            if (!generic->typeArgs.empty()) {
                                                auto elemVK = impl_->typeExprToKind(generic->typeArgs[0].get());
                                                Type::Kind ek = Type::Kind::Int;
                                                if (elemVK == Impl::VarKind::Str) ek = Type::Kind::Str;
                                                else if (elemVK == Impl::VarKind::Float) ek = Type::Kind::Float;
                                                impl_->classFieldListElemKindsBySym[clsSym][attrExpr->attribute] = ek;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // Check explicit annotation on assignment: self.x: list[str] = ...
                if (assign->typeAnnotation) {
                    if (auto* generic = dynamic_cast<GenericTypeExpr*>(assign->typeAnnotation.get())) {
                        if (!generic->typeArgs.empty()) {
                            auto elemVK = impl_->typeExprToKind(generic->typeArgs[0].get());
                            Type::Kind ek = Type::Kind::Int;
                            if (elemVK == Impl::VarKind::Str) ek = Type::Kind::Str;
                            else if (elemVK == Impl::VarKind::Float) ek = Type::Kind::Float;
                            impl_->classFieldListElemKindsBySym[clsSym][attrExpr->attribute] = ek;
                            // list[Callable[[...], R]] field - record the
                            // element FunctionType for for-loop call dispatch.
                            if (auto* cte = dynamic_cast<CallableTypeExpr*>(
                                    generic->typeArgs[0].get())) {
                                impl_->classFieldListElemCallableTypeBySym
                                    [clsSym][attrExpr->attribute] =
                                    impl_->callableTypeExprToFnType(cte);
                            }
                        }
                    }
                }
            }
        }
        // Second pass: propagate dict[K,V] value kinds for fields initialized from a
        // typed __init__ param (`self.params = params`); the AnnAssignStmt scan above misses this form, and without it subscripts PHI-mismatch str/ptr in ternaries.
        for (auto& bodyStmt : initDecl->body) {
            auto* assign = dynamic_cast<AssignStmt*>(bodyStmt.get());
            if (!assign) continue;
            for (auto& target : assign->targets) {
                auto* attrExpr = dynamic_cast<AttributeExpr*>(target.get());
                if (!attrExpr) continue;
                auto* selfName = dynamic_cast<NameExpr*>(attrExpr->object.get());
                if (!selfName || selfName->name != "self") continue;
                if (impl_->classFieldDictValueKindsBySym[clsSym].count(attrExpr->attribute))
                    continue;
                auto fkIt = impl_->classFieldKindsBySym[clsSym].find(attrExpr->attribute);
                if (fkIt == impl_->classFieldKindsBySym[clsSym].end() ||
                    fkIt->second != Impl::VarKind::Dict)
                    continue;
                GenericTypeExpr* dictType = nullptr;
                if (assign->typeAnnotation)
                    dictType = dynamic_cast<GenericTypeExpr*>(assign->typeAnnotation.get());
                if (!dictType && assign->value) {
                    if (auto* rhsName = dynamic_cast<NameExpr*>(assign->value.get())) {
                        auto pit = paramNameToIdx.find(rhsName->name);
                        if (pit != paramNameToIdx.end()) {
                            auto& param = initDecl->params[pit->second];
                            if (param.type)
                                dictType = dynamic_cast<GenericTypeExpr*>(param.type.get());
                        }
                    }
                }
                // Dict-literal RHS: infer V from entry value kinds, only if all agree
                // (heterogeneous dicts stay polymorphic, no false-positive typing).
                if (!dictType && assign->value) {
                    if (auto* dictLit = dynamic_cast<DictExpr*>(assign->value.get())) {
                        if (!dictLit->entries.empty()) {
                            auto inferKind = [](Expr* e) -> Type::Kind {
                                if (dynamic_cast<StringLiteral*>(e)) return Type::Kind::Str;
                                if (dynamic_cast<IntegerLiteral*>(e)) return Type::Kind::Int;
                                if (dynamic_cast<FloatLiteral*>(e)) return Type::Kind::Float;
                                if (dynamic_cast<BooleanLiteral*>(e)) return Type::Kind::Bool;
                                if (dynamic_cast<ListExpr*>(e)) return Type::Kind::List;
                                if (dynamic_cast<DictExpr*>(e)) return Type::Kind::Dict;
                                return Type::Kind::Unknown;
                            };
                            Type::Kind firstVK = inferKind(dictLit->entries[0].second.get());
                            bool consistent = (firstVK != Type::Kind::Unknown);
                            for (size_t i = 1; consistent && i < dictLit->entries.size(); ++i) {
                                if (inferKind(dictLit->entries[i].second.get()) != firstVK)
                                    consistent = false;
                            }
                            if (consistent) {
                                impl_->classFieldDictValueKindsBySym[clsSym][attrExpr->attribute] = firstVK;
                                continue;
                            }
                        }
                    }
                }
                if (!dictType || dictType->typeArgs.size() < 2) continue;
                auto* base = dynamic_cast<NamedTypeExpr*>(dictType->base.get());
                if (!base || base->name != "dict") continue;
                // D030 §5: direct Type::Kind from the annotation.
                Type::Kind vk = impl_->typeExprToTypeKind(dictType->typeArgs[1].get());
                impl_->classFieldDictValueKindsBySym[clsSym][attrExpr->attribute] = vk;
            }
        }
    }

    // Layout pre-pass stops here (struct type, field metadata registered); bailing
    // before non-idempotent work (static globals, ctor/method bodies, vtables) lets those run once per class, later, after every class's layout is visible (fixes forward-reference field access).
    if (impl_->classLayoutPass) return;

    // Each static field (AnnAssignStmt with isStatic=true) becomes a global
    // @ClassName_fieldName.
    for (auto& stmt : node.body) {
        auto* annAssign = dynamic_cast<AnnAssignStmt*>(stmt.get());
        if (!annAssign || !annAssign->isStatic) continue;

        auto* target = dynamic_cast<NameExpr*>(annAssign->target.get());
        if (!target) continue;

        std::string globalName = clsSym + "_" + target->name;
        llvm::Type* fieldType = impl_->typeExprToLLVM(annAssign->annotation.get());

        // Determine constant initializer for the global
        llvm::Constant* initVal = nullptr;
        bool needsRuntimeInit = false;

        if (annAssign->value) {
            // Try to resolve a compile-time constant from the initializer
            if (auto* intLit = dynamic_cast<IntegerLiteral*>(annAssign->value.get())) {
                initVal = llvm::ConstantInt::get(impl_->i64Type, intLit->value);
            } else if (auto* floatLit = dynamic_cast<FloatLiteral*>(annAssign->value.get())) {
                initVal = llvm::ConstantFP::get(impl_->f64Type, floatLit->value);
            } else if (auto* boolLit = dynamic_cast<BooleanLiteral*>(annAssign->value.get())) {
                if (fieldType == impl_->i1Type) {
                    initVal = llvm::ConstantInt::get(impl_->i1Type, boolLit->value ? 1 : 0);
                } else {
                    initVal = llvm::ConstantInt::get(impl_->i64Type, boolLit->value ? 1 : 0);
                }
            } else if (auto* strLit = dynamic_cast<StringLiteral*>(annAssign->value.get())) {
                // String literals need runtime init (global string ptr)
                needsRuntimeInit = true;
            } else {
                // Non-literal expression: defer to runtime initialization
                needsRuntimeInit = true;
            }
        }

        if (!initVal) {
            initVal = llvm::Constant::getNullValue(fieldType);
        }

        auto* gv = new llvm::GlobalVariable(
            *impl_->module, fieldType, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage, initVal, globalName);
        impl_->staticFieldGlobalsBySym[clsSym][target->name] = gv;

        // A non-trivial initializer is deferred to runtime init in the main preamble,
        // emitted when this AnnAssignStmt is visited later.
    }

    // Helper lambda: emit the body of a single __init__ (ClassName___init__, or
    // ClassName___init___N for multi-ctor) given its FunctionDecl and LLVM Function.
    auto emitInitBody = [&](FunctionDecl* decl, llvm::Function* initFunc) {
        if (!initFunc || !initFunc->empty()) return;

        auto* prevFunc = impl_->currentFunction;
        auto* prevBlock = impl_->builder->GetInsertBlock();
        std::string prevClassName = impl_->currentClassName;

        impl_->currentFunction = initFunc;
        impl_->currentClassName = node.name;
        auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", initFunc);
        impl_->builder->SetInsertPoint(entry);
        impl_->pushScope();
        Impl::VarMetaScope _varMeta(*impl_);

        // First arg is always self (ptr)
        auto initFuncType = initFunc->getFunctionType();
        auto argIt = initFunc->arg_begin();

        argIt->setName("self");
        auto* selfAlloca = impl_->createEntryAlloca(initFunc, "self", impl_->i8PtrType);
        impl_->builder->CreateStore(&*argIt, selfAlloca);
        impl_->setVar("self", selfAlloca, Impl::VarKind::ClassInstance);
        // GC: self is borrowed - _new() owns the instance
        impl_->scopes.back().borrowed.insert("self");
        ++argIt;

        // Remaining params (skip self in Dragon params based on hasImplicitSelf)
        size_t paramStart = decl->hasImplicitSelf ? 0 : 1;
        for (size_t i = paramStart; i < decl->params.size(); ++i) {
            std::string pname = decl->params[i].name;
            argIt->setName(pname);
            unsigned argIdx = 1 + (unsigned)(i - paramStart);
            auto* alloca = impl_->createEntryAlloca(initFunc, pname, initFuncType->getParamType(argIdx));
            impl_->builder->CreateStore(&*argIt, alloca);
            auto paramKind = impl_->typeExprToKind(decl->params[i].type.get());
            impl_->setVar(pname, alloca, paramKind);
            // GC: params are borrowed unless `own`. An `own` ctor param is moved in;
            // if stored, the slot is nulled so scope-exit free is a no-op, else scope exit releases it (ASan A/B-proven on consumed-not-stored).
            if (Impl::isHeapKind(paramKind) && !decl->params[i].isOwn)
                impl_->scopes.back().borrowed.insert(pname);
            // Track class-typed params for field access (e.g. other.x in dunders)
            if (auto* namedType = dynamic_cast<NamedTypeExpr*>(decl->params[i].type.get())) {
                if (impl_->classNames.count(namedType->name))
                    impl_->varClassNames[pname] = namedType->name;
            }
            ++argIt;
        }

        for (auto& bodyStmt : decl->body) {
            bodyStmt->accept(*this);
        }

        // GC: emit scope cleanup before implicit return
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->emitScopeCleanup();
            impl_->builder->CreateRetVoid();
        }

        impl_->popScope();
        impl_->currentClassName = prevClassName;
        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
    };

    // Phase 5: Create class_id global early so _new can use it
    llvm::GlobalVariable* classIdGlobal = nullptr;
    if (impl_->options.gcMode == GCMode::RC) {
        classIdGlobal = new llvm::GlobalVariable(
            *impl_->module, impl_->i64Type, false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(impl_->i64Type, 0),
            "__class_id_" + clsSym);
        impl_->classIdGlobalsBySym[clsSym] = classIdGlobal;
    }

    // Decision 026: Forward-declare vtable global (initializer set after method emission).
    llvm::GlobalVariable* vtableGlobal = nullptr;
    if (impl_->options.gcMode == GCMode::RC) {
        auto vtOrdIt = impl_->classVtableMethodOrderBySym.find(clsSym);
        if (vtOrdIt != impl_->classVtableMethodOrderBySym.end() && !vtOrdIt->second.empty()) {
            auto* vtableArrayType = llvm::ArrayType::get(impl_->i8PtrType, vtOrdIt->second.size());
            vtableGlobal = new llvm::GlobalVariable(
                *impl_->module, vtableArrayType, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantAggregateZero::get(vtableArrayType),
                clsSym + "__vtable");
        }
    }

    // Does `initDecl` assign `self.<fname>` unconditionally at top level? Used to skip
    // a default the ctor would immediately overwrite; a conditional/nested assign does NOT count, since the untaken branch still needs the default.
    auto ctorAssignsFieldTopLevel = [](FunctionDecl* initDecl,
                                       const std::string& fname) -> bool {
        if (!initDecl) return false;
        auto matchesSelfField = [&](Expr* target) -> bool {
            auto* attr = dynamic_cast<AttributeExpr*>(target);
            if (!attr || attr->attribute != fname) return false;
            auto* obj = dynamic_cast<NameExpr*>(attr->object.get());
            return obj && obj->name == "self";
        };
        for (auto& stmt : initDecl->body) {
            if (auto* a = dynamic_cast<AssignStmt*>(stmt.get())) {
                for (auto& t : a->targets)
                    if (matchesSelfField(t.get())) return true;
            } else if (auto* aa = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
                if (matchesSelfField(aa->target.get())) return true;
            }
        }
        return false;
    };

    // Acyclic-class opt: fields all scalars/strings (heap leaves, no object refs) can
    // never cycle, so skip dragon_gc_track (avoids per-object gc_lock); dealloc/gc_visit still handle a miss as an external ref. Whitelist is conservative: List/Dict/Set/Tuple/ClassInstance/Union/Closure/Generator exclude a class (bytes shares VarKind::List, can't be told apart).
    bool classIsAcyclic = (impl_->options.gcMode == GCMode::RC);
    for (auto& f : fields) {
        if (f.kind != Impl::VarKind::Int && f.kind != Impl::VarKind::Float &&
            f.kind != Impl::VarKind::Bool && f.kind != Impl::VarKind::Str &&
            f.kind != Impl::VarKind::StrLiteral) {
            classIsAcyclic = false;
            break;
        }
    }

    // Stack-construction eligibility (stricter than acyclic): every field must be
    // a scalar (a str field needs a decref Phase 1 never emits); requires a single non-self-leaking ctor and no per-instance defaults anywhere in the ancestor chain (the stack path skips default stores, so an inherited default would silently vanish).
    bool anyInheritedDefaults = false;
    {
        auto pit = impl_->classParentNamesBySym.find(clsSym);
        std::string cur = (pit != impl_->classParentNamesBySym.end()) ? pit->second : "";
        while (!cur.empty()) {
            auto dit = impl_->classPerInstanceDefaultsBySym.find(cur);
            if (dit != impl_->classPerInstanceDefaultsBySym.end() && !dit->second.empty()) {
                anyInheritedDefaults = true;
                break;
            }
            auto nit = impl_->classParentNamesBySym.find(cur);
            cur = (nit != impl_->classParentNamesBySym.end()) ? nit->second : "";
        }
    }
    {
        bool scalarOnly = (impl_->options.gcMode == GCMode::RC) &&
                          perInstanceDefaults.empty() && !anyInheritedDefaults &&
                          !isMultiCtor;
        if (scalarOnly) {
            for (auto& f : fields) {
                if (f.kind != Impl::VarKind::Int && f.kind != Impl::VarKind::Float &&
                    f.kind != Impl::VarKind::Bool) { scalarOnly = false; break; }
            }
        }
        if (scalarOnly && initDecl) {
            for (auto& st : initDecl->body)
                if (impl_->stmtEscapes(st.get(), "self")) { scalarOnly = false; break; }
        }
        if (scalarOnly)
            impl_->stackEligibleClassesBySym.insert(clsSym);
    }

    // Helper lambda: emit a _new that mallocs the struct, calls __init__, returns the
    // pointer. `initDeclForSkip` is the ctor scanned to suppress redundant default stores (null for the synthesized no-ctor path).
    auto emitNewBody = [&](llvm::Function* newFunc, const std::string& initFuncName,
                           FunctionDecl* initDeclForSkip) {
        if (!newFunc || !newFunc->empty()) return;

        // Record the ctor's owned-arg kinds under the _new symbol so the call site can
        // release owned heap temps (mirrors funcParamKinds; __init__ is skipped by the method-decl loop, so derive it directly here).
        if (impl_->options.gcMode == GCMode::RC && initDeclForSkip) {
            size_t pStart = initDeclForSkip->hasImplicitSelf ? 0 : 1;
            std::vector<Impl::VarKind> ck;
            std::vector<bool> cowns;
            for (size_t pi = pStart; pi < initDeclForSkip->params.size(); ++pi) {
                ck.push_back(impl_->typeExprToKind(initDeclForSkip->params[pi].type.get()));
                cowns.push_back(initDeclForSkip->params[pi].isOwn);
            }
            impl_->funcParamKinds[newFunc->getName().str()] = std::move(ck);
            impl_->funcParamOwns[newFunc->getName().str()] = std::move(cowns);
        }

        auto* prevFunc = impl_->currentFunction;
        auto* prevBlock = impl_->builder->GetInsertBlock();

        impl_->currentFunction = newFunc;
        auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", newFunc);
        impl_->builder->SetInsertPoint(entry);

        auto* mallocFunc = impl_->module->getFunction("malloc");
        if (!mallocFunc) {
            auto* mallocType = llvm::FunctionType::get(impl_->i8PtrType, {impl_->i64Type}, false);
            mallocFunc = llvm::Function::Create(
                mallocType, llvm::Function::ExternalLinkage, "malloc", impl_->module.get());
        }

        // Allocate memory for the struct (includes GC header fields when RC enabled)
        // Use DataLayout if available, otherwise compute as (numFields + headerOffset) * 8
        uint64_t structSize = (fields.size() + headerOffset) * 8;
        if (impl_->module->getDataLayout().getPointerSize() > 0) {
            structSize = impl_->module->getDataLayout().getTypeAllocSize(structType);
        }
        auto* sizeVal = llvm::ConstantInt::get(impl_->i64Type, structSize);
        auto* self = impl_->builder->CreateCall(mallocFunc, {sizeVal}, "self");

        // Zero-initialize the struct so uninitialized fields don't cause
        // stale pointer decrefs when __init__ overwrites them with RC.
        auto* memsetFunc = impl_->module->getFunction("memset");
        if (!memsetFunc) {
            auto* memsetType = llvm::FunctionType::get(impl_->i8PtrType,
                {impl_->i8PtrType, llvm::Type::getInt32Ty(*impl_->context), impl_->i64Type}, false);
            memsetFunc = llvm::Function::Create(
                memsetType, llvm::Function::ExternalLinkage, "memset", impl_->module.get());
        }
        impl_->builder->CreateCall(memsetFunc,
            {self, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0), sizeVal});

        // GC Phase 3: Initialize refcount and type_tag header fields
        if (impl_->options.gcMode == GCMode::RC) {
            // Store refcount = 1 at struct index 0
            auto* rcGEP = impl_->builder->CreateStructGEP(structType, self, 0, "rc_ptr");
            impl_->builder->CreateStore(
                llvm::ConstantInt::get(impl_->i64Type, 1), rcGEP);
            // Packed i64 header (little-endian): byte0=type_tag(7), byte1=gc_flags(0x80=
            // GC_FLAG_HEAP_OBJ), bytes2-3=class_id, bytes4-7=gc_track_idx(-1). HEAP_OBJ MUST be set: dragon_mark_shared_deep uses it as the heap sentinel; pre-fix instances had gc_flags=0, indistinguishable from a stack bag of bytes.
            auto* tagGEP = impl_->builder->CreateStructGEP(structType, self, 1, "tag_ptr");
            // Base: type_tag=7, gc_flags=HEAP_OBJ(0x80), gc_track_idx=-1
            uint64_t baseWord = 7ULL | (0x80ULL << 8) | (0xFFFFFFFF00000000ULL);
            llvm::Value* headerWord = llvm::ConstantInt::get(impl_->i64Type, baseWord);
            // OR in the class_id at bits 16..31
            if (classIdGlobal) {
                auto* cid = impl_->builder->CreateLoad(impl_->i64Type, classIdGlobal, "cid");
                auto* cidShifted = impl_->builder->CreateShl(cid,
                    llvm::ConstantInt::get(impl_->i64Type, 16), "cid_shifted");
                headerWord = impl_->builder->CreateOr(headerWord, cidShifted, "hdr_with_cid");
            }
            impl_->builder->CreateStore(headerWord, tagGEP);
            // Track for cycle collection, skipped for acyclic classes (classIsAcyclic):
            // they can't cycle, so RC alone reclaims them and gc_lock is pure overhead; GC_FLAG_TRACKED stays clear and gc_track_idx=-1 so decref/dealloc never touches the tracked set.
            if (!classIsAcyclic) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_gc_track"], {self});
            }

            // Decision 026: Store vtable pointer at struct offset 2
            if (vtableGlobal) {
                auto* vtGEP = impl_->builder->CreateStructGEP(structType, self, 2, "vt_ptr");
                impl_->builder->CreateStore(vtableGlobal, vtGEP);
            }
        }

        // Per-instance defaults (`x: int = 5`) evaluate fresh into this instance's
        // slot at every construction path; __init__ then overrides via an RC-correct store (decrefs the default). Struct is pre-memset to 0, so the plain CreateStore here needs no old-value decref; defaults apply base-first then derived-last so an override always wins (Python semantics).
        std::vector<std::pair<std::string, Expr*>> orderedDefaults;
        {
            // Collect ancestors innermost->outermost, then reverse to base-first.
            std::vector<std::string> chain;
            auto pit = impl_->classParentNamesBySym.find(clsSym);
            std::string cur = (pit != impl_->classParentNamesBySym.end()) ? pit->second : "";
            while (!cur.empty()) {
                chain.push_back(cur);
                auto nit = impl_->classParentNamesBySym.find(cur);
                cur = (nit != impl_->classParentNamesBySym.end()) ? nit->second : "";
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                auto dit = impl_->classPerInstanceDefaultsBySym.find(*it);
                if (dit != impl_->classPerInstanceDefaultsBySym.end())
                    for (auto& entry : dit->second) orderedDefaults.push_back(entry);
            }
            // This class's own defaults last (derived overrides base).
            for (auto& entry : perInstanceDefaults) orderedDefaults.push_back(entry);
            // Dedupe by field name, keeping only the LAST (most-derived) entry: a plain
            // last-write-wins store would evaluate a shadowed base default and leak its +1 heap value (pinned by test_rc_ctor_defaults.dr).
            {
                std::unordered_set<std::string> seenFromEnd;
                std::vector<std::pair<std::string, Expr*>> deduped;
                for (auto it = orderedDefaults.rbegin();
                     it != orderedDefaults.rend(); ++it)
                    if (seenFromEnd.insert(it->first).second)
                        deduped.push_back(*it);
                std::reverse(deduped.begin(), deduped.end());
                orderedDefaults = std::move(deduped);
            }
        }
        if (!orderedDefaults.empty()) {
            impl_->pushScope();  // defaults are class-body exprs: isolate from any ctor params/self
            for (auto& [fname, valExpr] : orderedDefaults) {
                if (ctorAssignsFieldTopLevel(initDeclForSkip, fname)) continue;
                // Resolve the slot via the subclass's merged layout so an inherited
                // default lands at the parent's original offset.
                auto idxIt = impl_->classFieldIndicesBySym[clsSym].find(fname);
                if (idxIt == impl_->classFieldIndicesBySym[clsSym].end()) continue;
                auto tyIt = impl_->classFieldTypesBySym[clsSym].find(fname);
                if (tyIt == impl_->classFieldTypesBySym[clsSym].end()) continue;
                auto* fieldPtr = impl_->builder->CreateStructGEP(
                    structType, self, idxIt->second, fname + "_def");
                valExpr->accept(*this);
                llvm::Value* dv = impl_->coerceArg(impl_->lastValue, tyIt->second);
                impl_->builder->CreateStore(dv, fieldPtr);
            }
            impl_->popScope();
        }

        // Call __init__(self, params...)
        auto* initFunc = impl_->module->getFunction(initFuncName);
        if (initFunc) {
            std::vector<llvm::Value*> initArgs = {self};
            for (auto& arg : newFunc->args()) {
                initArgs.push_back(&arg);
            }
            impl_->builder->CreateCall(initFunc, initArgs);
        }

        impl_->builder->CreateRet(self);

        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
    };

    if (!isMultiCtor) {
        if (initDecl) {
            std::string initFuncName = clsSym + "___init__";
            emitInitBody(initDecl, impl_->module->getFunction(initFuncName));
        } else {
            // Synthesized default ctor (no explicit `def()`): delegates to the parent's
            // zero-arg ctor when one exists (Python parity for `class Foo(Base): pass`).
            std::string initFuncName = clsSym + "___init__";
            if (auto* synthInit = impl_->module->getFunction(initFuncName)) {
                auto* prevFunc = impl_->currentFunction;
                auto* prevBlock = impl_->builder->GetInsertBlock();
                impl_->currentFunction = synthInit;
                auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", synthInit);
                impl_->builder->SetInsertPoint(entry);
                // Delegate to the parent's zero-arg __init__ if present. The
                // parent entry IS its sym, so the symbol lookup is direct.
                auto parentIt = impl_->classParentNamesBySym.find(clsSym);
                if (parentIt != impl_->classParentNamesBySym.end()) {
                    llvm::Function* parentInit = impl_->module->getFunction(
                        parentIt->second + "___init__");
                    if (parentInit && parentInit->arg_size() == 1) {
                        impl_->builder->CreateCall(parentInit, {synthInit->getArg(0)});
                    }
                }
                impl_->builder->CreateRetVoid();
                impl_->currentFunction = prevFunc;
                if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
            }
        }
        {
            std::string newFuncName = clsSym + "_new";
            std::string initFuncName = clsSym + "___init__";
            // initDecl is null for the synthesized no-ctor path -> no skip.
            emitNewBody(impl_->module->getFunction(newFuncName), initFuncName, initDecl);
        }
    } else {
        for (auto* decl : allInitDecls) {
            int ctorIdx = decl->constructorIndex >= 0 ? decl->constructorIndex
                          : (int)(&decl - &allInitDecls[0]);
            std::string suffix = "_" + std::to_string(ctorIdx);
            std::string initFuncName = clsSym + "___init__" + suffix;
            std::string newFuncName = clsSym + "_new" + suffix;

            emitInitBody(decl, impl_->module->getFunction(initFuncName));
            emitNewBody(impl_->module->getFunction(newFuncName), initFuncName, decl);
        }
    }

    // Generate per-class __dealloc__ and register class_id.
    if (impl_->options.gcMode == GCMode::RC) {
        bool hasHeapFields = false;
        for (auto& f : fields) {
            if (Impl::isHeapKind(f.kind)) { hasHeapFields = true; break; }
        }
        // Bug A: a Callable field may hold a heap DragonClosure this instance owns;
        // dealloc must drop that ref via the tag-aware decref or it leaks.
        bool hasCallableFields = false;
        {
            auto cfIt = impl_->classFieldCallableTypeBySym.find(clsSym);
            if (cfIt != impl_->classFieldCallableTypeBySym.end() && !cfIt->second.empty()) {
                hasCallableFields = true;
                hasHeapFields = true; // ensures the dealloc body is emitted below
            }
        }

        // docs/001-memory.md: a raw-handle own field (Lock, TLS ctx, ...) releases at
        // dealloc via the v1 alloc->releaser registry, keyed on the ctor's `self.f = <alloc>()` callee. Heap-typed own fields need nothing extra (heap-field loop below handles them); an undeducible releaser is a compile error, never a silent leak.
        std::vector<std::pair<std::string, std::string>> ownRawReleasers;
        {
            std::unordered_set<std::string> ownFieldNames;
            for (auto& member : node.body)
                if (auto* ann = dynamic_cast<AnnAssignStmt*>(member.get()))
                    if (ann->isOwn)
                        if (auto* nm = dynamic_cast<NameExpr*>(ann->target.get()))
                            ownFieldNames.insert(nm->name);
            if (!ownFieldNames.empty()) {
                static const std::unordered_map<std::string, std::string>
                    kOwnReleaserRegistry = {
                        {"Lock", "dragon_lock_destroy"},
                        {"dragon_lock_new", "dragon_lock_destroy"},
                        {"dragon_tls_ctx_new", "dragon_tls_ctx_free"},
                        // A raw malloc'd blob owned by a field pairs with
                        // libc free (Router._conn_cell's atomic counter).
                        {"malloc", "free"},
                    };
                // Derive each own field's alloc callee from ctor stores (E8 guarantees
                // fresh-expression stores, so `self.f = <call>()` is the only shape).
                std::unordered_map<std::string, std::string> fieldAllocCallee;
                std::function<void(const std::vector<std::unique_ptr<Stmt>>&)>
                    scanBody = [&](const std::vector<std::unique_ptr<Stmt>>& body) {
                        for (auto& st : body) {
                            Expr* target = nullptr;
                            Expr* value = nullptr;
                            if (auto* as = dynamic_cast<AssignStmt*>(st.get())) {
                                if (as->targets.size() == 1)
                                    target = as->targets[0].get();
                                value = as->value.get();
                            } else if (auto* an =
                                           dynamic_cast<AnnAssignStmt*>(st.get())) {
                                target = an->target.get();
                                value = an->value.get();
                            } else if (auto* iff = dynamic_cast<IfStmt*>(st.get())) {
                                scanBody(iff->thenBody);
                                for (auto& el : iff->elifClauses) scanBody(el.second);
                                scanBody(iff->elseBody);
                                continue;
                            }
                            if (!target || !value) continue;
                            auto* at = dynamic_cast<AttributeExpr*>(target);
                            if (!at) continue;
                            auto* obj = dynamic_cast<NameExpr*>(at->object.get());
                            if (!obj || obj->name != "self") continue;
                            if (!ownFieldNames.count(at->attribute)) continue;
                            if (auto* call = dynamic_cast<CallExpr*>(value))
                                if (auto* callee =
                                        dynamic_cast<NameExpr*>(call->callee.get()))
                                    fieldAllocCallee[at->attribute] = callee->name;
                        }
                    };
                for (auto& member : node.body)
                    if (auto* fn = dynamic_cast<FunctionDecl*>(member.get()))
                        if (fn->name == "__init__") scanBody(fn->body);
                // A handle arriving via an `own` param was allocated elsewhere, so the
                // ctor-store scan can't name its releaser; the same class.field-keyed registry covers it.
                static const std::unordered_map<std::string, std::string>
                    kOwnFieldReleaserRegistry = {
                        {"SSLSocket._conn", "dragon_tls_conn_free"},
                    };
                for (const auto& fname : ownFieldNames) {
                    // Heap-kind own fields are covered by the field loop below.
                    bool isHeapField = false;
                    for (auto& f : fields)
                        if (f.name == fname && Impl::isHeapKind(f.kind)) {
                            isHeapField = true;
                            break;
                        }
                    if (isHeapField) continue;
                    auto acIt = fieldAllocCallee.find(fname);
                    std::string releaser;
                    if (acIt != fieldAllocCallee.end()) {
                        auto rIt = kOwnReleaserRegistry.find(acIt->second);
                        if (rIt != kOwnReleaserRegistry.end())
                            releaser = rIt->second;
                    }
                    if (releaser.empty()) {
                        auto frIt = kOwnFieldReleaserRegistry.find(
                            node.name + "." + fname);
                        if (frIt != kOwnFieldReleaserRegistry.end())
                            releaser = frIt->second;
                    }
                    if (releaser.empty()) {
                        impl_->addError(
                            "own field '" + fname + "' of class '" + node.name +
                                "' has no registered releaser: the constructor "
                                "must assign it from a registered allocator "
                                "(Lock(), dragon_tls_ctx_new, ...) so the "
                                "compiler can generate the release",
                            node.location());
                        continue;
                    }
                    ownRawReleasers.emplace_back(fname, releaser);
                }
            }
        }

        // __dragon_dealloc_<mod>__<ClassName>(void* self), for classes with heap fields;
        // mangled like every other per-class symbol so same-named classes in different modules stay distinct.
        std::string deallocName = "__dragon_dealloc_" + clsSym;
        auto* deallocFnType = llvm::FunctionType::get(
            impl_->voidType, {impl_->i8PtrType}, false);
        auto* deallocFn = llvm::Function::Create(
            deallocFnType, llvm::Function::InternalLinkage, deallocName, impl_->module.get());

        if (hasHeapFields || !ownRawReleasers.empty()) {
            auto* prevFunc = impl_->currentFunction;
            auto* prevBlock = impl_->builder->GetInsertBlock();
            impl_->currentFunction = deallocFn;
            auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", deallocFn);
            impl_->builder->SetInsertPoint(entry);

            auto* self = &*deallocFn->arg_begin();
            self->setName("self");

            // Own raw-handle fields release FIRST (depth-first chain of custody,
            // docs/002 3.2); null-gated so a handle already close()'d releases exactly once.
            for (auto& [fname, releaser] : ownRawReleasers) {
                auto idxIt = impl_->classFieldIndicesBySym[clsSym].find(fname);
                auto tyIt = impl_->classFieldTypesBySym[clsSym].find(fname);
                if (idxIt == impl_->classFieldIndicesBySym[clsSym].end() ||
                    tyIt == impl_->classFieldTypesBySym[clsSym].end())
                    continue;
                auto* gep = impl_->builder->CreateStructGEP(
                    structType, self, idxIt->second, fname + "_own_ptr");
                auto* val = impl_->builder->CreateLoad(
                    tyIt->second, gep, fname + "_own");
                llvm::Value* p = val;
                if (p->getType()->isIntegerTy())
                    p = impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
                else if (p->getType() != impl_->i8PtrType)
                    p = impl_->builder->CreateBitCast(p, impl_->i8PtrType);
                auto* relFn = impl_->getOrDeclareRuntime(releaser,
                    llvm::FunctionType::get(impl_->voidType, {impl_->i8PtrType},
                                            false));
                auto* nonNull = impl_->builder->CreateICmpNE(
                    p, llvm::ConstantPointerNull::get(
                           llvm::cast<llvm::PointerType>(p->getType())),
                    fname + "_own_set");
                auto* relBB = llvm::BasicBlock::Create(
                    *impl_->context, fname + ".own.rel", deallocFn);
                auto* contBB = llvm::BasicBlock::Create(
                    *impl_->context, fname + ".own.cont", deallocFn);
                impl_->builder->CreateCondBr(nonNull, relBB, contBB);
                impl_->builder->SetInsertPoint(relBB);
                impl_->builder->CreateCall(relFn, {p});
                impl_->builder->CreateBr(contBB);
                impl_->builder->SetInsertPoint(contBB);
            }

            // GEP + decref every heap-typed field; track which names are handled here
            // (a Callable field is VarKind::Closure, decref'd via dragon_decref_callable) so the Callable-specific pass below doesn't double-decref and UAF.
            std::set<std::string> deallocHandled;
            for (auto& f : fields) {
                if (!Impl::isHeapKind(f.kind)) continue;
                unsigned idx = impl_->classFieldIndicesBySym[clsSym][f.name];
                auto* gep = impl_->builder->CreateStructGEP(structType, self, idx, f.name + "_ptr");
                auto* val = impl_->builder->CreateLoad(f.type, gep, f.name + "_val");
                deallocHandled.insert(f.name);
                if (f.kind == Impl::VarKind::Str) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref_str"], {val});
                } else if (f.kind == Impl::VarKind::Closure) {
                    // tag-gated drop - frees a real closure (+ its env) and
                    // no-ops on a bare fn ptr (no header).
                    llvm::Value* p = val;
                    if (!p->getType()->isPointerTy())
                        p = impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_callable"], {p});
                } else if (f.kind == Impl::VarKind::Union) {
                    // Any/Union field is a {i64 tag, i64 payload} box; inttoptr on it trips
                    // the verifier, so extract tag+payload and route through emitUnionDecref (heap-kind tags only).
                    auto* tag = impl_->boxTag(val, f.name + ".tag");
                    auto* payload = impl_->boxPayloadI64(val, f.name + ".payload");
                    impl_->emitUnionDecref(payload, tag);
                } else {
                    // List, Dict, Tuple, Set, Bytes, ClassInstance all use dragon_decref
                    llvm::Value* ptrVal = val;
                    if (!val->getType()->isPointerTy())
                        ptrVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ptrVal});
                }
            }
            // Bug A: tag-aware decref on Callable fields (no-op for bare fn ptrs), only
            // for ones NOT already handled by the heap-field loop above (an inference miss still needs its single decref here).
            if (hasCallableFields) {
                auto cfIt = impl_->classFieldCallableTypeBySym.find(clsSym);
                if (cfIt != impl_->classFieldCallableTypeBySym.end()) {
                    for (auto& [fname, _ftype] : cfIt->second) {
                        if (deallocHandled.count(fname)) continue;
                        auto fIdxIt = impl_->classFieldIndicesBySym[clsSym].find(fname);
                        if (fIdxIt == impl_->classFieldIndicesBySym[clsSym].end())
                            continue;
                        unsigned idx = fIdxIt->second;
                        auto* gep2 = impl_->builder->CreateStructGEP(
                            structType, self, idx, fname + "_ptr");
                        auto* fldType =
                            impl_->classFieldTypesBySym[clsSym][fname];
                        auto* val2 = impl_->builder->CreateLoad(
                            fldType, gep2, fname + "_val");
                        llvm::Value* p = val2;
                        if (p->getType()->isIntegerTy())
                            p = impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
                        else if (p->getType() != impl_->i8PtrType &&
                                 p->getType()->isPointerTy())
                            p = impl_->builder->CreateBitCast(p, impl_->i8PtrType);
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_decref_callable"], {p});
                    }
                }
            }
            impl_->builder->CreateRetVoid();

            impl_->currentFunction = prevFunc;
            if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
        } else {
            // No heap fields: empty dealloc (just return void)
            auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", deallocFn);
            llvm::IRBuilder<> tmpB(entry);
            tmpB.CreateRetVoid();
        }

        // __dragon_traverse_ClassName(self, visit_fn, arg): visits fields that can
        // form cycles (containers + class instances), NOT strings.
        std::string traverseName = "__dragon_traverse_" + clsSym;
        auto* visitFnPtrType = llvm::FunctionType::get(
            impl_->voidType, {impl_->i8PtrType, impl_->i8PtrType}, false);
        auto* traverseFnType = llvm::FunctionType::get(
            impl_->voidType,
            {impl_->i8PtrType, llvm::PointerType::get(*impl_->context, 0), impl_->i8PtrType},
            false);
        auto* traverseFn = llvm::Function::Create(
            traverseFnType, llvm::Function::InternalLinkage, traverseName, impl_->module.get());

        bool hasCyclicFields = false;
        for (auto& f : fields) {
            if (f.kind == Impl::VarKind::List || f.kind == Impl::VarKind::Dict ||
                f.kind == Impl::VarKind::Tuple || f.kind == Impl::VarKind::Set ||
                f.kind == Impl::VarKind::ClassInstance ||
                // A Closure field can capture `self` back (self.cb = lambda {...self...}),
                // a real instance->closure->env->self cycle the collector must see.
                f.kind == Impl::VarKind::Closure) {
                hasCyclicFields = true;
                break;
            }
        }

        if (hasCyclicFields) {
            auto* prevFunc2 = impl_->currentFunction;
            auto* prevBlock2 = impl_->builder->GetInsertBlock();
            impl_->currentFunction = traverseFn;
            auto* entry2 = llvm::BasicBlock::Create(*impl_->context, "entry", traverseFn);
            impl_->builder->SetInsertPoint(entry2);

            auto argIt2 = traverseFn->arg_begin();
            auto* selfArg = &*argIt2++;  selfArg->setName("self");
            auto* visitArg = &*argIt2++; visitArg->setName("visit");
            auto* visitData = &*argIt2;  visitData->setName("arg");

            // NULL-check each field before visit: the collector can fire mid-construction
            // (after gc_track, before __init__ populates fields), when a field is still memset-zeroed; gc_visit_reachable derefs offset 9 and segfaults on NULL otherwise.
            for (auto& f : fields) {
                if (f.kind != Impl::VarKind::List && f.kind != Impl::VarKind::Dict &&
                    f.kind != Impl::VarKind::Tuple && f.kind != Impl::VarKind::Set &&
                    f.kind != Impl::VarKind::ClassInstance &&
                    // Visit Closure fields so the collector subtracts the instance->closure
                    // ref; visit fns only deref a TRACKED child, so a bare-fn-ptr Callable (never tracked) is a safe hash-miss.
                    f.kind != Impl::VarKind::Closure) continue;
                unsigned idx = impl_->classFieldIndicesBySym[clsSym][f.name];
                auto* gep = impl_->builder->CreateStructGEP(structType, selfArg, idx, f.name + "_ptr");
                auto* val = impl_->builder->CreateLoad(f.type, gep, f.name + "_val");
                llvm::Value* ptrVal = val;
                if (!val->getType()->isPointerTy())
                    ptrVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                // if (ptrVal != null) { visit(ptrVal, visitData); }
                auto* notNull = impl_->builder->CreateICmpNE(
                    ptrVal,
                    llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context)),
                    f.name + "_nonnull");
                auto* visitBB = llvm::BasicBlock::Create(*impl_->context, f.name + "_visit", traverseFn);
                auto* contBB  = llvm::BasicBlock::Create(*impl_->context, f.name + "_cont",  traverseFn);
                impl_->builder->CreateCondBr(notNull, visitBB, contBB);
                impl_->builder->SetInsertPoint(visitBB);
                impl_->builder->CreateCall(
                    visitFnPtrType, visitArg, {ptrVal, visitData});
                impl_->builder->CreateBr(contBB);
                impl_->builder->SetInsertPoint(contBB);
            }
            impl_->builder->CreateRetVoid();

            impl_->currentFunction = prevFunc2;
            if (prevBlock2) impl_->builder->SetInsertPoint(prevBlock2);
        } else {
            auto* entry2 = llvm::BasicBlock::Create(*impl_->context, "entry", traverseFn);
            llvm::IRBuilder<> tmpB2(entry2);
            tmpB2.CreateRetVoid();
        }

        // __dragon_clear_ClassName(self): like dealloc, but zeros fields after decref
        // (cycle-collector clear_refs) so a later dragon_dealloc can't double-decref.
        std::string clearName = "__dragon_clear_" + clsSym;
        auto* clearFnType = llvm::FunctionType::get(
            impl_->voidType, {impl_->i8PtrType}, false);
        auto* clearFn = llvm::Function::Create(
            clearFnType, llvm::Function::InternalLinkage, clearName, impl_->module.get());

        if (hasHeapFields) {
            auto* prevFunc3 = impl_->currentFunction;
            auto* prevBlock3 = impl_->builder->GetInsertBlock();
            impl_->currentFunction = clearFn;
            auto* entry3 = llvm::BasicBlock::Create(*impl_->context, "entry", clearFn);
            impl_->builder->SetInsertPoint(entry3);

            auto* selfArg3 = &*clearFn->arg_begin();
            selfArg3->setName("self");

            for (auto& f : fields) {
                if (!Impl::isHeapKind(f.kind)) continue;
                unsigned idx = impl_->classFieldIndicesBySym[clsSym][f.name];
                auto* gep = impl_->builder->CreateStructGEP(structType, selfArg3, idx, f.name + "_ptr");
                auto* val = impl_->builder->CreateLoad(f.type, gep, f.name + "_val");
                // Decref the field value
                if (f.kind == Impl::VarKind::Str) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref_str"], {val});
                } else if (f.kind == Impl::VarKind::Closure) {
                    // tag-gated drop, safe on a bare fn ptr (see dealloc).
                    llvm::Value* p = val;
                    if (!p->getType()->isPointerTy())
                        p = impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_callable"], {p});
                } else if (f.kind == Impl::VarKind::Union) {
                    // Any / Union box field - same handling as dealloc above.
                    auto* tag = impl_->boxTag(val, f.name + ".tag");
                    auto* payload = impl_->boxPayloadI64(val, f.name + ".payload");
                    impl_->emitUnionDecref(payload, tag);
                } else {
                    llvm::Value* ptrVal = val;
                    if (!val->getType()->isPointerTy())
                        ptrVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ptrVal});
                }
                // Zero the field to prevent double-decref when dealloc runs later
                llvm::Value* zero = llvm::Constant::getNullValue(f.type);
                impl_->builder->CreateStore(zero, gep);
            }
            impl_->builder->CreateRetVoid();

            impl_->currentFunction = prevFunc3;
            if (prevBlock3) impl_->builder->SetInsertPoint(prevBlock3);
        } else {
            auto* entry3 = llvm::BasicBlock::Create(*impl_->context, "entry", clearFn);
            llvm::IRBuilder<> tmpB3(entry3);
            tmpB3.CreateRetVoid();
        }

        // __dragon_mark_shared_ClassName(self, worklist): BFS walker for SHARED
        // propagation (d018-shared-refcount.md); visits ALL heap fields including strings, unlike __dragon_traverse_X which only visits cyclic fields.
        std::string markSharedName = "__dragon_mark_shared_" + clsSym;
        auto* markSharedFnType = llvm::FunctionType::get(
            impl_->voidType, {impl_->i8PtrType, impl_->i8PtrType}, false);
        auto* markSharedFn = llvm::Function::Create(
            markSharedFnType, llvm::Function::InternalLinkage,
            markSharedName, impl_->module.get());

        if (hasHeapFields) {
            auto* prevFunc4 = impl_->currentFunction;
            auto* prevBlock4 = impl_->builder->GetInsertBlock();
            impl_->currentFunction = markSharedFn;
            auto* entry4 = llvm::BasicBlock::Create(*impl_->context, "entry", markSharedFn);
            impl_->builder->SetInsertPoint(entry4);

            auto argIt4 = markSharedFn->arg_begin();
            auto* selfArg4 = &*argIt4++;     selfArg4->setName("self");
            auto* worklistArg = &*argIt4;    worklistArg->setName("worklist");

            for (auto& f : fields) {
                if (!Impl::isHeapKind(f.kind)) continue;
                unsigned idx = impl_->classFieldIndicesBySym[clsSym][f.name];
                auto* gep = impl_->builder->CreateStructGEP(structType, selfArg4, idx, f.name + "_ptr");
                auto* val = impl_->builder->CreateLoad(f.type, gep, f.name + "_val");
                if (f.kind == Impl::VarKind::Str) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_str"], {val});
                } else if (f.kind == Impl::VarKind::Union) {
                    // Any/Union box: only a heap-tagged payload needs shared marking; strip
                    // the tag, push the payload when heap (int/bool/float/None payloads are inline data, no RC).
                    auto* tag = impl_->boxTag(val, f.name + ".tag");
                    auto* payload = impl_->boxPayloadI64(val, f.name + ".payload");
                    auto* func = impl_->currentFunction;
                    auto* isStrBB = llvm::BasicBlock::Create(
                        *impl_->context, "union.mark.str", func);
                    auto* notStrBB = llvm::BasicBlock::Create(
                        *impl_->context, "union.mark.notstr", func);
                    auto* isHeapBB = llvm::BasicBlock::Create(
                        *impl_->context, "union.mark.heap", func);
                    auto* endBB = llvm::BasicBlock::Create(
                        *impl_->context, "union.mark.end", func);
                    auto* isStr = impl_->builder->CreateICmpEQ(
                        tag, llvm::ConstantInt::get(impl_->i64Type, 1), "is.str");
                    impl_->builder->CreateCondBr(isStr, isStrBB, notStrBB);
                    impl_->builder->SetInsertPoint(isStrBB);
                    auto* strPtr = impl_->builder->CreateIntToPtr(
                        payload, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_str"], {strPtr});
                    impl_->builder->CreateBr(endBB);
                    impl_->builder->SetInsertPoint(notStrBB);
                    auto* isHeap = impl_->builder->CreateICmpSGE(
                        tag, llvm::ConstantInt::get(impl_->i64Type, 5), "is.heap");
                    impl_->builder->CreateCondBr(isHeap, isHeapBB, endBB);
                    impl_->builder->SetInsertPoint(isHeapBB);
                    auto* heapPtr = impl_->builder->CreateIntToPtr(
                        payload, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_worklist_push"],
                        {worklistArg, heapPtr});
                    impl_->builder->CreateBr(endBB);
                    impl_->builder->SetInsertPoint(endBB);
                } else if (f.kind == Impl::VarKind::Closure) {
                    // A Callable field may be a bare fn ptr (no header) or a DragonClosure;
                    // route through dragon_mark_shared_callable (tag-gated like dragon_incref_callable), else the generic atomic-OR on gc_flags at offset 9 writes into read-only .text (SIGSEGV).
                    llvm::Value* ptrVal = val;
                    if (!val->getType()->isPointerTy())
                        ptrVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_callable"],
                        {worklistArg, ptrVal});
                } else {
                    llvm::Value* ptrVal = val;
                    if (!val->getType()->isPointerTy())
                        ptrVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_worklist_push"],
                        {worklistArg, ptrVal});
                }
            }
            impl_->builder->CreateRetVoid();

            impl_->currentFunction = prevFunc4;
            if (prevBlock4) impl_->builder->SetInsertPoint(prevBlock4);
        } else {
            auto* entry4 = llvm::BasicBlock::Create(*impl_->context, "entry", markSharedFn);
            llvm::IRBuilder<> tmpB4(entry4);
            tmpB4.CreateRetVoid();
        }

        // Decision 025: descriptor global (zero-init) is created before the
        // deferredClassInits push so the dci carries the pointer directly (main preamble avoids the last-wins bare-keyed classDescriptorGlobals map); symbol mangled per module so same-named classes get separate globals.
        llvm::GlobalVariable* descGlobalForDci = nullptr;
        {
            std::string descSymName = clsSym + "__descriptor";
            descGlobalForDci = impl_->module->getNamedGlobal(descSymName);
            if (!descGlobalForDci) {
                descGlobalForDci = new llvm::GlobalVariable(
                    *impl_->module, impl_->i64Type, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantInt::get(impl_->i64Type, 0),
                    descSymName);
            }
            // The bare-keyed map is best-effort for source-level lookups (e.g. the
            // decorator path); last-wins is fine since decorated same-named classes already need an alias to disambiguate.
            impl_->classDescriptorGlobalsBySym[clsSym] = descGlobalForDci;
        }

        // main() preamble stores dragon_class_register_dealloc(fn) -> classIdGlobal;
        // capture clsSym + the descriptor pointer here so it resolves the right symbol without the last-write-wins bare-keyed map.
        Impl::DeferredClassInit dci;
        dci.className = node.name;
        dci.classSymPrefix = clsSym;
        dci.owningModule = impl_->currentModuleName;
        dci.descriptorGlobal = descGlobalForDci;
        dci.deallocFn = deallocFn;
        dci.classIdGlobal = classIdGlobal;
        dci.traverseFn = traverseFn;
        dci.clearFn = clearFn;
        dci.markSharedFn = markSharedFn;
        impl_->deferredClassInits.push_back(std::move(dci));
    } else {
        // GC off: still create the descriptor global so `Cls.__name__` resolves; no
        // dci push since the main preamble's deferred init loop is GC-only.
        if (!impl_->classDescriptorGlobalsBySym.count(clsSym)) {
            auto* descGlobal = new llvm::GlobalVariable(
                *impl_->module, impl_->i64Type, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(impl_->i64Type, 0),
                clsSym + "__descriptor");
            impl_->classDescriptorGlobalsBySym[clsSym] = descGlobal;
        }
    }

    for (auto& stmt : node.body) {
        auto* methodDecl = dynamic_cast<FunctionDecl*>(stmt.get());
        if (!methodDecl || methodDecl->name == "__init__") continue;
        // D044+ - a generic-method template is never lowered; only its stamped
        // instantiations (empty typeParams, appended to this body) are emitted.
        if (!methodDecl->typeParams.empty()) continue;

        std::string methodFuncName = clsSym + "_" + methodDecl->name;
        // ADR 010: emit each overload under its per-index symbol; without the suffix,
        // same-name overloads collided and only the first body was emitted.
        if (methodDecl->methodOverloadCount > 1 &&
            methodDecl->methodOverloadIndex >= 0)
            methodFuncName += "__ov" + std::to_string(methodDecl->methodOverloadIndex);
        auto* methodFunc = impl_->module->getFunction(methodFuncName);
        if (!methodFunc || !methodFunc->empty()) continue;

        // A generator method (`yield` in body) emits as a wrapper returning the
        // generator object with self threaded in (emitGeneratorFn); @classmethod generators are not yet supported.
        if (Impl::containsYield(methodDecl->body) && !methodDecl->isClassMethod) {
            bool genHasSelf = !methodDecl->isStatic;
            size_t genParamStart =
                methodDecl->isStatic ? 0 : (methodDecl->hasImplicitSelf ? 0 : 1);
            emitGeneratorFn(*methodDecl, methodFunc, methodFuncName,
                            genHasSelf, node.name, genParamStart);
            continue;
        }

        auto* prevFunc = impl_->currentFunction;
        auto* prevBlock = impl_->builder->GetInsertBlock();
        std::string prevClassName = impl_->currentClassName;

        impl_->currentFunction = methodFunc;
        impl_->currentClassName = node.name;
        auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", methodFunc);
        impl_->builder->SetInsertPoint(entry);
        impl_->pushScope();

        // Isolate union-member-kind tracking to this method body: without it, isinstance
        // ELSE-narrowing on a `T | str` param can't resolve the complement arm and stores the raw 16-byte box into a native field, corrupting memory. Save/clear/restore so entries don't leak between methods.
        Impl::VarMetaScope _varMeta(*impl_);
        impl_->unionMemberKinds.clear();

        auto methodFuncType = methodFunc->getFunctionType();
        auto argIt = methodFunc->arg_begin();

        if (methodDecl->isStatic) {
            // Static method: NO self parameter. LLVM args are user params.
            // @classmethod: skip first param (cls) since we don't pass it.
            size_t paramStart = methodDecl->isClassMethod ? 1 : 0;
            unsigned argIdx = 0;
            for (size_t i = paramStart; i < methodDecl->params.size(); ++i) {
                const auto& mp = methodDecl->params[i];
                if (mp.isVarArg && mp.name.empty()) continue;  // bare * separator
                std::string pname = mp.name;
                argIt->setName(pname);
                auto* alloca = impl_->createEntryAlloca(methodFunc, pname, methodFuncType->getParamType(argIdx));
                impl_->builder->CreateStore(&*argIt, alloca);
                if (mp.isVarArg) {
                    // *args: the packed monomorphized list[T] (see instance path).
                    impl_->setVar(pname, alloca, Impl::VarKind::List);
                    impl_->scopes.back().borrowed.insert(pname);
                    if (TypeExpr* elemTy = mp.type.get()) {
                        Impl::VarKind ek = impl_->typeExprToKind(elemTy);
                        impl_->varListElemKinds[pname] =
                            Impl::elemVarKindToTypeKind(ek);
                        if (ek == Impl::VarKind::Type)
                            impl_->varListElemIsType.insert(pname);
                        if (auto* nt = dynamic_cast<NamedTypeExpr*>(elemTy)) {
                            if (impl_->classNames.count(nt->name) ||
                                impl_->classFieldKindsBySym.count(impl_->classSymPrefix(nt->name)))
                                impl_->varListElemClassName[pname] = nt->name;
                        }
                    }
                    ++argIt; ++argIdx;
                    continue;
                }
                if (mp.isKwArg) {
                    // **kwargs: the packed dict[str, V].
                    impl_->setVar(pname, alloca, Impl::VarKind::Dict);
                    impl_->scopes.back().borrowed.insert(pname);
                    impl_->varDictKeyKinds[pname] = Type::Kind::Str;
                    impl_->varDictValueKinds[pname] = Impl::elemVarKindToTypeKind(
                        impl_->typeExprToKind(mp.type.get()));
                    ++argIt; ++argIdx;
                    continue;
                }
                auto paramKind = impl_->typeExprToKind(mp.type.get());
                impl_->setVar(pname, alloca, paramKind);
                // Track union member kinds so isinstance else-narrowing of a
                // union param resolves the complement arm (see save/clear above).
                if (paramKind == Impl::VarKind::Union)
                    impl_->unionMemberKinds[pname] =
                        impl_->typeExprToUnionMembers(mp.type.get());
                // GC: params are borrowed unless `own`; an `own` param is moved in and
                // released at scope exit if the body didn't move it onward.
                if (Impl::isHeapKind(paramKind) && !mp.isOwn)
                    impl_->scopes.back().borrowed.insert(pname);
                // Register Callable / ptr signature so calls through the param
                // resolve to the right LLVM function type (no `: ptr` workaround).
                impl_->trackPtrParam(pname, mp.type.get());
                // Track class-typed params for field access
                if (auto* namedType = dynamic_cast<NamedTypeExpr*>(mp.type.get())) {
                    if (impl_->classNames.count(namedType->name))
                        impl_->varClassNames[pname] = namedType->name;
                }
                ++argIt;
                ++argIdx;
            }
        } else {
            // Instance method: first arg is self (ptr)
            argIt->setName("self");
            auto* selfAlloca = impl_->createEntryAlloca(methodFunc, "self", impl_->i8PtrType);
            impl_->builder->CreateStore(&*argIt, selfAlloca);
            impl_->setVar("self", selfAlloca, Impl::VarKind::ClassInstance);
            // GC: self is borrowed - caller owns the instance
            impl_->scopes.back().borrowed.insert("self");
            ++argIt;

            // A `*args`/`**kwargs` param binds as a list/dict local like the free-function
            // prologue; the bare `*` keyword-only separator has no LLVM param and is skipped without advancing argIdx (self is 0), so it can't desync from argIt.
            size_t paramStart = methodDecl->hasImplicitSelf ? 0 : 1;
            unsigned argIdx = 1;  // after self
            for (size_t i = paramStart; i < methodDecl->params.size(); ++i) {
                const auto& mp = methodDecl->params[i];
                if (mp.isVarArg && mp.name.empty()) continue;  // bare * separator
                std::string pname = mp.name;
                argIt->setName(pname);
                auto* alloca = impl_->createEntryAlloca(
                    methodFunc, pname, methodFuncType->getParamType(argIdx));
                impl_->builder->CreateStore(&*argIt, alloca);
                if (mp.isVarArg) {
                    // *args: the packed monomorphized list[T]. Register the
                    // element kind so `for x in args` / `args[i]` read native T.
                    impl_->setVar(pname, alloca, Impl::VarKind::List);
                    impl_->scopes.back().borrowed.insert(pname);
                    if (TypeExpr* elemTy = mp.type.get()) {
                        Impl::VarKind ek = impl_->typeExprToKind(elemTy);
                        impl_->varListElemKinds[pname] =
                            Impl::elemVarKindToTypeKind(ek);
                        if (ek == Impl::VarKind::Type)
                            impl_->varListElemIsType.insert(pname);
                        if (auto* nt = dynamic_cast<NamedTypeExpr*>(elemTy)) {
                            if (impl_->classNames.count(nt->name) ||
                                impl_->classFieldKindsBySym.count(impl_->classSymPrefix(nt->name)))
                                impl_->varListElemClassName[pname] = nt->name;
                        }
                    }
                    ++argIt; ++argIdx;
                    continue;
                }
                if (mp.isKwArg) {
                    // **kwargs: the packed dict[str, V]. Keys are always str;
                    // the annotation is the per-VALUE type.
                    impl_->setVar(pname, alloca, Impl::VarKind::Dict);
                    impl_->scopes.back().borrowed.insert(pname);
                    impl_->varDictKeyKinds[pname] = Type::Kind::Str;
                    impl_->varDictValueKinds[pname] = Impl::elemVarKindToTypeKind(
                        impl_->typeExprToKind(mp.type.get()));
                    ++argIt; ++argIdx;
                    continue;
                }
                auto paramKind = impl_->typeExprToKind(mp.type.get());
                impl_->setVar(pname, alloca, paramKind);
                // Track union member kinds so isinstance else-narrowing of a
                // union param resolves the complement arm (see save/clear above).
                if (paramKind == Impl::VarKind::Union)
                    impl_->unionMemberKinds[pname] =
                        impl_->typeExprToUnionMembers(mp.type.get());
                // GC: params are borrowed except `own` (docs/002 2.8): the caller moved
                // its +1 in, so the callee releases it at scope exit unless the body moved it onward; without the `!isOwn` guard, a consumed-not-stored own param leaked one value per call (ASan A/B-proven).
                if (Impl::isHeapKind(paramKind) && !mp.isOwn)
                    impl_->scopes.back().borrowed.insert(pname);
                // Register Callable / ptr signature so calls through the param
                // resolve to the right LLVM function type (no `: ptr` workaround).
                impl_->trackPtrParam(pname, mp.type.get());
                // Track class-typed params for field access (e.g. other.x in dunders)
                if (auto* namedType = dynamic_cast<NamedTypeExpr*>(mp.type.get())) {
                    if (impl_->classNames.count(namedType->name))
                        impl_->varClassNames[pname] = namedType->name;
                }
                ++argIt; ++argIdx;
            }
        }

        for (auto& bodyStmt : methodDecl->body) {
            bodyStmt->accept(*this);
        }

        // GC: emit scope cleanup before implicit return
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->emitScopeCleanup();
            if (methodFunc->getReturnType() == impl_->voidType) {
                impl_->builder->CreateRetVoid();
            } else {
                impl_->builder->CreateRet(
                    llvm::Constant::getNullValue(methodFunc->getReturnType()));
            }
        }

        impl_->popScope();
        impl_->currentClassName = prevClassName;
        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
    }

    // D033: emit a per-method "bound thunk" (closure ABI: user_args..., env) that
    // loads self from env and forwards to the method, so dragon_getattr builds a bound callable without a runtime trampoline. Static methods get a NULL slot (raw fn ptr instead); classmethods reuse the thunk shape with the descriptor in env.
    {
        std::string thunkClsSym = Impl::mangleClass(impl_->currentModuleName, node.name);
        auto ownIt = impl_->classOwnMethodsBySym.find(clsSym);
        if (ownIt != impl_->classOwnMethodsBySym.end()) {
            for (auto& methodName : ownIt->second) {
                uint8_t kind = 0;
                auto kIt = impl_->classMethodKindsBySym[clsSym].find(methodName);
                if (kIt != impl_->classMethodKindsBySym[clsSym].end()) kind = kIt->second;
                if (kind == 1 || kind == 2) {
                    // Static/@classmethod: the emitted signature has no leading self/cls
                    // param, so no bind is needed (getattr returns the raw fn ptr); slot stays NULL to keep method_fn_ptrs indices aligned.
                    impl_->classMethodBoundThunksBySym[clsSym][methodName] = nullptr;
                    continue;
                }
                std::string methodSym = thunkClsSym + "_" + methodName;
                auto* methodFn = impl_->module->getFunction(methodSym);
                if (!methodFn) continue;
                auto* methodFnType = methodFn->getFunctionType();
                // Thunk type: same as method but drop first param (self) and
                // append i8* env. Returns same type as method.
                std::vector<llvm::Type*> thunkParams;
                // Skip param 0 (self) - it's reconstructed from env.
                for (unsigned i = 1; i < methodFnType->getNumParams(); i++) {
                    thunkParams.push_back(methodFnType->getParamType(i));
                }
                thunkParams.push_back(impl_->i8PtrType); // env
                auto* thunkType = llvm::FunctionType::get(
                    methodFnType->getReturnType(), thunkParams, false);
                std::string thunkSym = methodSym + "__bound";
                auto* thunkFn = impl_->module->getFunction(thunkSym);
                if (!thunkFn) {
                    thunkFn = llvm::Function::Create(
                        thunkType, llvm::Function::InternalLinkage,
                        thunkSym, impl_->module.get());
                }

                // Emit the thunk body. Save/restore the outer insert point.
                auto* prevFunc = impl_->currentFunction;
                auto* prevBlock = impl_->builder->GetInsertBlock();
                auto* thunkEntry = llvm::BasicBlock::Create(
                    *impl_->context, "entry", thunkFn);
                impl_->builder->SetInsertPoint(thunkEntry);
                impl_->currentFunction = thunkFn;

                // Env is the last param. Load self from env body at offset
                // sizeof(DragonEnv) = 24 bytes (16B header + 8B dealloc_fn).
                auto thunkArgIt = thunkFn->arg_begin();
                std::vector<llvm::Value*> userArgs;
                for (unsigned i = 0; i < thunkFn->arg_size() - 1; i++) {
                    thunkArgIt->setName("a" + std::to_string(i));
                    userArgs.push_back(&*thunkArgIt);
                    ++thunkArgIt;
                }
                llvm::Value* envArg = &*thunkArgIt;
                envArg->setName("env");
                // GEP into env body: ptr + 24 -> ptr-to-self-slot.
                auto* int8Ty = llvm::Type::getInt8Ty(*impl_->context);
                auto* selfSlotPtr = impl_->builder->CreateGEP(
                    int8Ty, envArg,
                    {llvm::ConstantInt::get(impl_->i64Type, 24)},
                    "self.slot");
                auto* selfVal = impl_->builder->CreateLoad(
                    impl_->i8PtrType, selfSlotPtr, "self");
                // Build the call args: self first, then user args.
                std::vector<llvm::Value*> callArgs;
                callArgs.push_back(selfVal);
                for (auto* a : userArgs) callArgs.push_back(a);
                if (methodFnType->getReturnType() == impl_->voidType) {
                    impl_->builder->CreateCall(methodFn, callArgs);
                    impl_->builder->CreateRetVoid();
                } else {
                    auto* result = impl_->builder->CreateCall(
                        methodFn, callArgs, "bound.call");
                    impl_->builder->CreateRet(result);
                }
                impl_->classMethodBoundThunksBySym[clsSym][methodName] = thunkFn;
                impl_->currentFunction = prevFunc;
                if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
            }
        }
    }

    // Decision 026: Set real vtable initializer now that all method bodies exist.
    if (vtableGlobal) {
        auto& vtableOrder = impl_->classVtableMethodOrderBySym[clsSym];
        std::vector<llvm::Constant*> vtableEntries;
        for (auto& methodName : vtableOrder) {
            // MRO lookup: check this class then walk the parent chain; symbols are
            // mangled per owning module (own class uses clsSym, parents resolve via classOwningModule).
            llvm::Function* func = impl_->resolveMethodFunction(
                impl_->currentModuleName, node.name, methodName);
            if (func)
                vtableEntries.push_back(func);
            else
                vtableEntries.push_back(llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context)));
        }
        auto* vtableArrayType = llvm::ArrayType::get(impl_->i8PtrType, vtableEntries.size());
        vtableGlobal->setInitializer(llvm::ConstantArray::get(vtableArrayType, vtableEntries));
    }

    // Runtime init for static fields with non-literal initializers must run inside a
    // function context (typically main); dependency-module classes processed before main() exists defer their init to the main() preamble.
    for (auto& stmt : node.body) {
        auto* annAssign = dynamic_cast<AnnAssignStmt*>(stmt.get());
        if (!annAssign || !annAssign->isStatic) continue;
        if (!annAssign->value) continue;

        auto* target = dynamic_cast<NameExpr*>(annAssign->target.get());
        if (!target) continue;

        // Check if this was already handled as a compile-time constant
        bool isLiteral = dynamic_cast<IntegerLiteral*>(annAssign->value.get()) ||
                         dynamic_cast<FloatLiteral*>(annAssign->value.get()) ||
                         dynamic_cast<BooleanLiteral*>(annAssign->value.get());
        if (isLiteral) continue;

        // Emit runtime initialization: evaluate the expression and store to the global
        auto sfIt = impl_->staticFieldGlobalsBySym.find(clsSym);
        if (sfIt == impl_->staticFieldGlobalsBySym.end()) continue;
        auto gvIt = sfIt->second.find(target->name);
        if (gvIt == sfIt->second.end()) continue;

        // Guard: if no valid insertion point (dep class before main), defer
        if (!impl_->currentFunction || !impl_->builder->GetInsertBlock()) {
            impl_->deferredStaticInits.push_back({annAssign->value.get(), gvIt->second});
            continue;
        }

        annAssign->value->accept(*this);
        llvm::Value* val = impl_->lastValue;
        llvm::Type* fieldType = gvIt->second->getValueType();

        // Type coercion to match the global's type
        if (val->getType() != fieldType) {
            if (fieldType == impl_->f64Type && val->getType() == impl_->i64Type)
                val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
            else if (fieldType == impl_->i64Type && val->getType() == impl_->i1Type)
                val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (fieldType == impl_->i64Type && val->getType() == impl_->f64Type)
                val = impl_->builder->CreateFPToSI(val, impl_->i64Type);
        }

        impl_->builder->CreateStore(val, gvIt->second);
    }
}


} // namespace dragon
