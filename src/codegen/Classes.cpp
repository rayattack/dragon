/// Dragon CodeGen - Class Declaration
#include "../CodeGenImpl.h"
#include "ClassesShared.h"

#include <functional>

namespace dragon {


void CodeGen::visit(ClassDecl& node) {
    // D044 - a generic class template (`class Foo[T]`) has free type vars and is
    // never lowered; only its stamped monomorphic instantiations (`Foo[int]`,
    // emitted with empty typeParams) reach codegen. Skip the template.
    if (!node.typeParams.empty()) return;

    // D044 cross-module generics: a stamped instantiation of a template defined
    // in another module is OWNED by, and resolves its body's bare names (sibling
    // functions, module globals) against, that defining module - not the
    // instantiation site. `classOwningModule` (line below), `classSymPrefix`, and
    // the call site (Assign.cpp's `resolveClassOwningModule`) all key off
    // `currentModuleName`, so overriding it here keeps symbol mangling and body
    // resolution consistent. The guard restores it on every return path; inert
    // for normal (non-stamped) classes.
    const std::string _savedModuleForGeneric = impl_->currentModuleName;
    struct RestoreModule {
        std::string* slot; std::string saved; bool active;
        ~RestoreModule() { if (active) *slot = saved; }
    } _restoreModule{&impl_->currentModuleName, _savedModuleForGeneric,
                     !node.genericHomeModule.empty()};
    if (!node.genericHomeModule.empty())
        impl_->currentModuleName = node.genericHomeModule;
    //--- TypedDict: backed by DragonDict at runtime, not a struct ---
    // Must check BEFORE classNames.insert to avoid treating TypedDict as a struct class.
    {
        bool isTypedDict = false;
        for (auto& base : node.bases) {
            if (auto* bn = dynamic_cast<NameExpr*>(base.get()))
                if (bn->name == "TypedDict") isTypedDict = true;
        }
        if (isTypedDict) {
            impl_->typedDictClasses.insert(node.name);

            // Collect field name -> VarKind from annotated fields in class body
            for (auto& stmt : node.body) {
                if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
                    if (auto* fieldName = dynamic_cast<NameExpr*>(ann->target.get())) {
                        auto fk = impl_->typeExprToTypeKind(ann->annotation.get());
                        impl_->typedDictFieldKinds[node.name][fieldName->name] = fk;
                    }
                }
            }

            // Generate constructor: ClassName(dict_literal) -> returns the dict as-is
            // OR ClassName(key=val, key=val) -> creates dict with tagged set
            // For now, TypedDict "construction" is handled at CallExpr level:
            // we detect calls to TypedDict class names and emit dict creation.
            return; // Skip normal class struct/method generation
        }
    }

    // Register class name for constructor dispatch (after TypedDict check)
    impl_->classNames.insert(node.name);
    impl_->classOwningModule[node.name] = impl_->currentModuleName;

    // Per-module class symbol prefix used everywhere we reach a class-owned
    // LLVM symbol below: struct type, vtable global, classId global,
    // descriptor global, dealloc/traverse/clear/markShared helpers, init/new
    // and method bodies. Two modules with same-named classes get distinct
    // mangled symbols so neither body is silently dropped at link time.
    const std::string clsSym = Impl::mangleClass(impl_->currentModuleName, node.name);

    // Stash the class docstring so the descriptor_create call site can pass it
    // through to the runtime - powers `Cls.__doc__` / `instance.__doc__`.
    if (node.docstring)
        impl_->classDocstrings[node.name] = *node.docstring;

    // Methods are emitted inline inside this function (not via the top-level
    // visit(FunctionDecl) path), so their docstrings need to be stashed here.
    // Powers `MyClass.method.__doc__` / `instance.method.__doc__` in
    // Attributes.cpp.
    for (auto& bodyStmt : node.body) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(bodyStmt.get())) {
            if (fd->docstring)
                impl_->methodDocstrings[node.name][fd->name] = *fd->docstring;
        }
    }

    // Track parent class for super() dispatch. Accept both bare names
    // (`Base`) and dotted-module references (`pkg.Base`) - the latter is
    // how cross-module inheritance lands in the AST. Storing the bare
    // name keeps the lookup uniform; classOwningModule resolves which
    // module the bare name came from when the descriptor is emitted.
    if (!node.bases.empty()) {
        std::string baseBareName;
        if (auto* baseName = dynamic_cast<NameExpr*>(node.bases[0].get())) {
            baseBareName = baseName->name;
        } else if (auto* baseAttr = dynamic_cast<AttributeExpr*>(node.bases[0].get())) {
            baseBareName = baseAttr->attribute;
        }
        if (!baseBareName.empty()) {
            impl_->classParentNames[node.name] = baseBareName;
        }
    }

    // D024 Phase 2 (6.11): collect user class decorators for later application
    // at the class's source position during entry-module-body iteration. Skip
    // compile-time-recognized decorators (`@dataclass`, `@NamedTuple`) - those
    // are handled below at codegen time.
    if (!node.decorators.empty()) {
        std::vector<Expr*> userDecs;
        for (auto& dec : node.decorators) {
            if (auto* ne = dynamic_cast<NameExpr*>(dec.get())) {
                if (ne->name == "dataclass" || ne->name == "NamedTuple") continue;
            }
            userDecs.push_back(dec.get());
        }
        if (!userDecs.empty()) {
            impl_->decoratedClasses.insert(node.name);
            impl_->classDecoratorExprs[node.name] = std::move(userDecs);
        }
    }

    // @dataclass / NamedTuple synthesis already ran during
    // forwardDeclareClasses. The synthesized __init__ is now a regular member
    // of node.body and will be processed by the rest of this visitor.

    // Class-body field declarations like `names: list[str]` (PEP 526
    // style) populate classFieldListElemKinds so subscript on the field
    // (`obj.names[0]`) carries the right VarKind. Without this, the subscript
    // path defaults to int and downstream method dispatch / print fails.
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
            impl_->classFieldListElemKinds[node.name][tgt->name] = ek;
            // For list[ClassName] also track the class name so iteration /
            // subscript can resolve attribute access on the elements.
            if (auto* elemNamed = dynamic_cast<NamedTypeExpr*>(generic->typeArgs[0].get())) {
                if (impl_->classNames.count(elemNamed->name))
                    impl_->classFieldListElemClassName[node.name][tgt->name] = elemNamed->name;
            }
        } else if ((baseName == "dict" || baseName == "Dict") && generic->typeArgs.size() >= 2) {
            // Class-body `data: dict[K, V]` - record the value Type::Kind so
            // SubscriptExpr on the field (`obj.data["k"]`, `obj.data[k]`) routes
            // through the typed runtime op (dragon_dict_get_str_ptr / _str_f64)
            // and the loop variable in `for x in obj.data` carries the right
            // native type. Mirrors the constructor-body AnnAssignStmt scan but
            // covers the PEP 526 class-body declaration form.
            // D030 §5: derive Type::Kind directly from the annotation - no VarKind hop.
            // D030 Phase 3.G: also record the key Type::Kind so int-keyed
            // class-field dicts dispatch to dragon_dict_int_* at the call site.
            Type::Kind kk = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
            Type::Kind vk = impl_->typeExprToTypeKind(generic->typeArgs[1].get());
            impl_->classFieldDictKeyKinds[node.name][tgt->name] = kk;
            impl_->classFieldDictValueKinds[node.name][tgt->name] = vk;
        }
    }

    //--- 2a: Extract fields from ALL __init__ bodies ---
    // Scan every __init__ overload for self.field assignments.
    // The struct type contains the UNION of all fields from all constructors.
    struct FieldInfo { std::string name; llvm::Type* type; Impl::VarKind kind; };
    std::vector<FieldInfo> fields;
    std::set<std::string> seenFields; // avoid duplicates

    // Collect all __init__ FunctionDecls
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

        // Build local-var -> (LLVM type, VarKind) from typed AnnAssign declarations
        // earlier in the same body. Lets `self.f = local` infer the field's heap
        // type from `local: list[int] = ...` even though `local` is not a param.
        // Without this, fields seeded only via locals fall back to i64/Other,
        // which freezes a wrong struct layout AND skips the incref on the
        // `self.f = local` store, so the field dangles after the local's scope
        // exit decrefs it.
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
            // NameExpr referring to an earlier typed local OR a constructor
            // parameter. Both are typed at the source level; the field-extract
            // pass needs to surface those types so a ctor like
            //  def(initial: Inner) { self.inner = initial }
            // tracks self.inner as a class-instance pointer rather than i64.
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
            // a Callable-typed RHS - e.g. `self.f = mk(10)` (a closure-
            // returning call) or any closure-valued expression - seeds the field
            // as a refcounted Closure. Without this the field-extract pass left a
            // closure field as VarKind::Other, so the synthesized destructor never
            // decref'd it and every instance leaked its closure (+ env). The
            // NameExpr/param branches above already cover `self.f = closure_param`;
            // this covers the value-producing forms (the static type is the truth).
            if (value && value->type &&
                value->type->kind() == Type::Kind::Function) {
                return {impl_->i8PtrType, Impl::VarKind::Closure, true};
            }
            return {impl_->i64Type, Impl::VarKind::Other, false};
        };

        // Recursive walker: extract self-field assigns from this stmt and any
        // compound statements nested inside it. Without recursion, fields
        // assigned inside if/elif/else (`def(){ if cond { self.x = ... }
        // else { self.x = ... } }`) wouldn't get registered, the struct
        // wouldn't have a slot, and downstream load/store would resolve to
        // index 0 - silently producing a 0-bit-pattern read.
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
            // Body of recognized leaf forms: AssignStmt and AnnAssignStmt
            // get the existing field-extract logic below. The big code block
            // is preserved intact; we just dispatch through this lambda.
            Stmt* bodyStmt = bodyStmtRaw;
            // (The original loop body follows - substituting `bodyStmt`
            //  for the previous loop variable.)
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
                                    // Infer from RHS: if RHS is a constructor parameter with a
                                    // type annotation, use that type. This handles the common
                                    // pattern: self(name: str) { self.name = name }
                                    if (auto* rhsName = dynamic_cast<NameExpr*>(assign->value.get())) {
                                        auto pit = paramNameToIdx.find(rhsName->name);
                                        if (pit != paramNameToIdx.end()) {
                                            auto& param = initDecl->params[pit->second];
                                            if (param.type) {
                                                fieldType = impl_->typeExprToLLVM(param.type.get());
                                                fieldKind = impl_->typeExprToKind(param.type.get());
                                                // Param has a NamedTypeExpr -> if it names a class,
                                                // record it so chained access (`obj.field.x`)
                                                // resolves to the right struct layout. Without
                                                // this resolveExprClassName(AttributeExpr) returns
                                                // "" and the load falls through to ConstantInt 0.
                                                if (fieldKind == Impl::VarKind::ClassInstance) {
                                                    if (auto* nt = dynamic_cast<NamedTypeExpr*>(param.type.get())) {
                                                        std::string cn = impl_->resolveAnnotationClassName(nt->name);
                                                        if (!cn.empty()) {
                                                            impl_->classFieldClassName[node.name][attrExpr->attribute] = cn;
                                                        }
                                                    }
                                                }
                                                // Bug A: preserve Callable signature so a
                                                // later `obj.field(args)` can build a real
                                                // FunctionType + branch on the closure tag
                                                // (capturing closure vs bare fn pointer).
                                                if (auto* cte = dynamic_cast<CallableTypeExpr*>(param.type.get())) {
                                                    impl_->classFieldCallableType
                                                        [node.name][attrExpr->attribute] =
                                                        impl_->callableTypeExprToFnType(cte);
                                                }
                                            }
                                        }
                                    }
                                    // Infer from RHS literal type. NB: a string-literal init
                                    // (`self.x = ""`) must record the field kind as Str, NOT
                                    // StrLiteral - the field is mutable and other methods may
                                    // reassign it to a heap str. If we record StrLiteral,
                                    // `isHeapKind(StrLiteral)` is false, so storeWithRCOverwrite
                                    // for a later `self.x = heapStr` skips both incref-new and
                                    // decref-old, the new heap value never gets the +1 ref it
                                    // needs to outlive the caller's scope, and the field ends
                                    // up pointing at freed memory after the caller returns.
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
                                        // RHS is a builtin type-name constructor or a user function.
                                        // The native LLVM type comes from the callee per D030: builtin
                                        // type names map directly; user functions are looked up in the
                                        // already-emitted LLVM module to read their declared return type.
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
                                                // Intrinsic Lock field: `self._lock = Lock()`.
                                                // Lock is not a user class, so without this the
                                                // field will stay untagged and `self._lock.acquire()`
                                                // / `with self._lock` will fall through to generic
                                                // paths that SILENTLY drop - the "lock" never locks
                                                // (found by the concurrent mutation dectector on
                                                // Router._storage_lock). Tag with the same "__Lock" sentinel
                                                // the local variable path uses so attribute receiver
                                                // dispatch reaches dragon_lock_acquire/release.
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::Other;
                                                impl_->classFieldClassName[node.name][attrExpr->attribute] = "__Lock";
                                            } else if (impl_->classNames.count(cn)) {
                                                // User class constructor: `self.x = Foo(args)`.
                                                // Track both kind and concrete class name so a
                                                // chained access `obj.x.field` can resolve to
                                                // Foo's struct layout instead of falling through
                                                // to ConstantInt 0.
                                                fieldType = impl_->i8PtrType;
                                                fieldKind = Impl::VarKind::ClassInstance;
                                                impl_->classFieldClassName[node.name][attrExpr->attribute] = cn;
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
                                // Final fallback: pattern matchers for compound
                                // expressions and typed-local references that
                                // none of the dynamic_cast checks above caught.
                                // This is what fixes `self.x = [0] * N` and
                                // `self.x = local` where `local: list[T]`
                                // without forcing every callsite to annotate.
                                if (fieldKind == Impl::VarKind::Other) {
                                    auto [t, k, ok] = inferFromExpr(assign->value.get());
                                    if (ok) { fieldType = t; fieldKind = k; }
                                }
                                // Last-ditch: lift the TypeChecker's inferred
                                // type on the RHS expression. Catches cases
                                // the dynamic_cast chain can't see - most
                                // commonly `self.x = MODULE_CONST` where the
                                // const resolves to a str / int / list at
                                // type level but is just a NameExpr at AST.
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
                                // Class-instance fields: the static type is
                                // authoritative (D030). The dynamic_cast chain
                                // above only recognizes a direct ctor
                                // (`self.x = Foo(...)`, NameExpr callee); a
                                // static-factory init whose callee is an
                                // AttributeExpr (`self.sock = TcpStream.open(...)`)
                                // slips through, leaving the field i64/Other with
                                // classFieldClassName unset - so `self.sock.fd`
                                // would read the wrong offset. Recover the
                                // concrete class from the RHS InstanceType here.
                                if (assign->value && assign->value->type &&
                                    assign->value->type->kind() == Type::Kind::Instance &&
                                    !impl_->classFieldClassName[node.name].count(attrExpr->attribute)) {
                                    if (auto* inst = dynamic_cast<InstanceType*>(assign->value->type.get())) {
                                        if (inst->classType && impl_->classNames.count(inst->classType->name)) {
                                            fieldType = impl_->i8PtrType;
                                            fieldKind = Impl::VarKind::ClassInstance;
                                            impl_->classFieldClassName[node.name][attrExpr->attribute] = inst->classType->name;
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
                                // Niche-opt `Class | None`: the union lowers to a
                                // bare ptr (fieldType == i8*) that must read as a
                                // ClassInstance of `Class`, so `obj.field.x` and
                                // `obj.field != none` resolve to the right struct /
                                // a null check instead of falling through to
                                // const-0 / dragon_str_eq (the real miscompile of
                                // `ClassInstance | None` *fields* - local vars were
                                // already handled in Assign.cpp). Gated on the ptr
                                // lowering so a boxed multi-member union is untouched;
                                // str|None etc. return "" here and stay as-is.
                                if (fieldType == impl_->i8PtrType) {
                                    const std::string ucn =
                                        impl_->typeExprUnionClassName(annAssign->annotation.get());
                                    if (!ucn.empty()) {
                                        fieldKind = Impl::VarKind::ClassInstance;
                                        impl_->classFieldClassName[node.name][attrExpr->attribute] = ucn;
                                    }
                                }
                                // Bug A: same Callable-field tracking for the
                                // explicit `self.handler: Callable[...]` form.
                                if (auto* cte = dynamic_cast<CallableTypeExpr*>(annAssign->annotation.get())) {
                                    impl_->classFieldCallableType
                                        [node.name][attrExpr->attribute] =
                                        impl_->callableTypeExprToFnType(cte);
                                }
                                // Intrinsic Lock field, annotated form:
                                // `self._lock: Lock = Lock()`. Same tagging as
                                // the unannotated ctor-scan branch (see the
                                // cn == "Lock" case above) so field-receiver
                                // acquire/release/with dispatch works instead
                                // of silently dropping the calls.
                                if (auto* lockNamed = dynamic_cast<NamedTypeExpr*>(
                                        annAssign->annotation.get())) {
                                    if (lockNamed->name == "Lock" &&
                                        !impl_->classNames.count("Lock")) {
                                        fieldType = impl_->i8PtrType;
                                        fieldKind = Impl::VarKind::Other;
                                        impl_->classFieldClassName
                                            [node.name][attrExpr->attribute] = "__Lock";
                                    }
                                }
                                // `self.x: list[T] = ...` - record the element
                                // kind (and class name when T is a class) so
                                // for-in iteration / subscript on the field
                                // dispatches with the right LLVM type. Without
                                // this, an empty list initialized in the ctor
                                // and grown later via append carries no elem
                                // type, the loop var sizes as i64, and pointer
                                // payloads come back zero-shaped.
                                if (auto* generic = dynamic_cast<GenericTypeExpr*>(annAssign->annotation.get())) {
                                    if (auto* baseN = dynamic_cast<NamedTypeExpr*>(generic->base.get())) {
                                        if ((baseN->name == "list" || baseN->name == "List") &&
                                            !generic->typeArgs.empty()) {
                                            // D030 §5: direct Type::Kind from the annotation.
                                            Type::Kind ek = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
                                            impl_->classFieldListElemKinds[node.name][attrExpr->attribute] = ek;
                                            if (auto* elemNamed = dynamic_cast<NamedTypeExpr*>(generic->typeArgs[0].get())) {
                                                std::string cn = impl_->resolveAnnotationClassName(elemNamed->name);
                                                if (!cn.empty()) {
                                                    impl_->classFieldListElemClassName[node.name][attrExpr->attribute] = cn;
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

    // Per-instance field defaults: (field name, initializer Expr*) gathered from
    // value-bearing non-static class-body declarations. emitNewBody evaluates
    // each into every fresh instance. The Expr* is borrowed (owned by node.body),
    // so it stays valid for the lifetime of this visit().
    std::vector<std::pair<std::string, Expr*>> perInstanceDefaults;

    // A class-body field declaration (`name: T`, PEP 526 style) declares an
    // instance field even when the def() constructor doesn't assign it - the
    // field may be set in another method (e.g. `stream: SSLSocket` assigned in
    // connect()). Previously only constructor-assigned fields got a struct slot,
    // so such a field had NO slot: assignments to it elsewhere were silently
    // dropped and reads returned null -> segfault on use. Give every declared
    // instance field a slot (type/kind from its annotation - the declared type
    // is authoritative per D030). Skip `static` declarations (class-level,
    // handled separately) and any field a constructor already captured
    // (seenFields).
    //
    // PER-INSTANCE DEFAULTS: a VALUE-BEARING non-static declaration (`x: int = 5`,
    // `items: list = []`) also gets a slot here AND its initializer is recorded
    // for emitNewBody, which evaluates it fresh into every instance (NOT a shared
    // class global - `static` already owns "shared", and a fresh copy avoids
    // Python's mutable-default footgun). dataclass fields are exempt: their
    // ann->value was already moved out in synthesizeDataclassMethods (so the
    // `if (ann->value)` test below is false) and their synth ctor sets the
    // defaults itself. Collection of the default is independent of the seenFields
    // slot dedup: a field both declared-with-default AND *conditionally* assigned
    // in the ctor must still apply its default to the untaken branch.
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
        // Slot creation (deduped against constructor-captured fields). When the
        // ctor body already seeded this field (often with a less-precise kind
        // inferred from the assignment RHS - e.g. `self.f = mk()` infers
        // VarKind::Other), the CLASS-BODY annotation is authoritative: upgrade a
        // stale Other to the annotated heap kind so the field is laid out and
        // refcounted correctly (a `f: Callable` field seeded as Other from a
        // closure-returning-call ctor init was left out of the destructor ->
        // every instance leaked its closure).
        if (seenFields.count(tgt->name)) {
            if (Impl::isHeapKind(declKind)) {
                for (auto& fld : fields) {
                    if (fld.name == tgt->name && fld.kind == Impl::VarKind::Other) {
                        fld.kind = declKind;
                        fld.type = declType;
                    }
                }
            } else if (declKind != Impl::VarKind::Other) {
                // Scalar annotations fix the TYPE too: `price: float` seeded
                // by `self.price = d["price"]` (RHS type unknown to
                // extractFields) stayed i64, so the f64 store wrote raw
                // payload bits and reads returned the float's bit pattern as
                // an integer. Upgrade fld.type only; the KIND deliberately
                // stays Other for now. Upgrading it to Int/Float/Bool makes
                // the class eligible for the acyclic skip-tracking
                // optimization, and untracked instances surface the known
                // call-arg-temp RC leak (an instance-returning call used
                // directly as an argument orphans its +1) that the cycle
                // collector currently rescues tracked classes from. The C3
                // plan sequences that leak fix BEFORE widening acyclic
                // skip-tracking; when it lands, this branch should assign
                // fld.kind = declKind as well.
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

    // Persist this class's per-instance defaults so a later subclass's
    // emitNewBody can walk the parent chain and re-apply inherited defaults.
    // The pre-pass visits every class before any _new body is emitted and
    // classes are visited in source order (not topologically), so recording
    // here - not from emitNewBody - is what makes the parent-chain lookup
    // order-independent (a base declared after its subclass would otherwise be
    // absent when the subclass's _new is emitted).
    impl_->classPerInstanceDefaults[node.name] = perInstanceDefaults;

    // Inherit the parent's fields so the subclass struct is a prefix-
    // compatible extension of the parent (standard single-inheritance
    // layout: parent fields first, at the parent's indices, then the
    // subclass's own new fields). Without this, a subclass whose __init__
    // doesn't re-assign every inherited field (e.g. an empty `def() {}`
    // that relies on the parent's defaults) produced a struct MISSING those
    // fields. Accessing an inherited field through a parent-typed reference
    // then read/wrote past the (smaller) allocation - the OLD-value load in
    // an RC overwrite returned heap garbage and dragon_decref segfaulted.
    //
    // The subclass's own __init__ may also re-assign an inherited field;
    // extractFields already captured it, so we dedupe by name and keep the
    // parent's slot position (don't shift the inherited field to the tail).
    {
        auto parentIt = impl_->classParentNames.find(node.name);
        if (parentIt != impl_->classParentNames.end()) {
            const std::string& parentName = parentIt->second;
            auto pIdxIt = impl_->classFieldIndices.find(parentName);
            auto pTyIt  = impl_->classFieldTypes.find(parentName);
            auto pKindIt = impl_->classFieldKinds.find(parentName);
            if (pIdxIt != impl_->classFieldIndices.end() &&
                pTyIt  != impl_->classFieldTypes.end()) {
                // Recover the parent's field order from its name->index map.
                std::vector<std::pair<unsigned, std::string>> parentOrdered;
                for (auto& [fname, fidx] : pIdxIt->second)
                    parentOrdered.push_back({fidx, fname});
                std::sort(parentOrdered.begin(), parentOrdered.end());

                std::vector<FieldInfo> merged;
                std::set<std::string> mergedSeen;
                for (auto& [fidx, fname] : parentOrdered) {
                    (void)fidx;
                    // parallel maps (classFieldIndices / classFieldTypes) must
                    // agree on the parent's field set; if they drift, diagnose
                    // and skip rather than throwing from .at().
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
                    if (pKindIt != impl_->classFieldKinds.end()) {
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

                // Propagate the parent's list/dict element-kind and class-name
                // metadata for inherited fields so subscript/iteration on them
                // resolves correctly through the subclass too.
                auto copyMap = [&](auto& dst, auto& srcMap) {
                    auto sit = srcMap.find(parentName);
                    if (sit == srcMap.end()) return;
                    // snapshot parent's inner map: when dst == srcMap (called
                    // below with the same map for both args), dst[node.name]
                    // may rehash and invalidate sit before we iterate.
                    auto parentEntry = sit->second;
                    auto& dstEntry = dst[node.name];
                    for (auto& kv : parentEntry)
                        if (!dstEntry.count(kv.first)) dstEntry[kv.first] = kv.second;
                };
                copyMap(impl_->classFieldListElemKinds, impl_->classFieldListElemKinds);
                copyMap(impl_->classFieldClassName, impl_->classFieldClassName);
            }
        }
    }

    // D030: a class-body field annotation is authoritative for that field's
    // type/kind/class - the static type IS the truth. extractFields infers a
    // field only from how the constructor *initializes* it, and only the
    // direct-ctor (`self.a = A(..)`) and class-typed-param forms recorded the
    // field's class. A factory call (`self.a = make_a(..)`), a typed local
    // (`self.a = tmp`), or a cross-module return would leave the field as
    // kind=Other (no refcount management) and, cross-module, type=i64 (the
    // fallback default) - and crucially classFieldClassName unset, so method
    // dispatch on `self.a.method()` resolves to ConstantInt 0. Re-derive
    // declared class fields straight from their annotation here so the
    // initialization form does not matter.
    for (auto& bs : node.body) {
        auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get());
        if (!ann || ann->isStatic || !ann->annotation) continue;
        auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
        if (!tgt) continue;
        auto* named = dynamic_cast<NamedTypeExpr*>(ann->annotation.get());
        if (!named) continue;  // generics handled by the scan above
        std::string cn = impl_->resolveAnnotationClassName(named->name);
        if (cn.empty()) continue;
        impl_->classFieldClassName[node.name][tgt->name] = cn;
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

    //--- 2b: Create LLVM struct type ---
    // Dragon class struct layout (D026 Vtable + GC Phase 3):
    //  Without GC: %ClassName = type { field0_type, field1_type, ... }
    //  With GC: %ClassName = type { i64 refcount, i64 type_tag, ptr vtable, field0, ... }
    // The header consists of:
    //  index 0: refcount (i64) - initialized to 1 in _new()
    //  index 1: type_tag (i64, stores DRAGON_TAG_CLASS=7, padded to i64 for alignment)
    //  index 2: vtable pointer (ptr) - points to @ClassName__vtable global constant
    // All user fields are offset by 3 when GC is enabled.
    unsigned headerOffset = (impl_->options.gcMode == GCMode::RC) ? 3 : 0;
    std::vector<llvm::Type*> fieldTypes;
    if (impl_->options.gcMode == GCMode::RC) {
        fieldTypes.push_back(impl_->i64Type);   // index 0: refcount
        fieldTypes.push_back(impl_->i64Type);   // index 1: type_tag + gc_flags (packed into i64)
        fieldTypes.push_back(impl_->i8PtrType); // index 2: vtable pointer (D026)
    }
    for (auto& f : fields) fieldTypes.push_back(f.type);
    // Reuse the struct type if the layout pre-pass already created it - calling
    // StructType::create twice for the same class would mint a duplicate named
    // struct (`%Class.0`). The body-emission pass re-runs this function and must
    // reuse the layout pass's type.
    llvm::StructType* structType = nullptr;
    if (auto it = impl_->classStructTypes.find(node.name);
        it != impl_->classStructTypes.end()) {
        structType = it->second;
    } else {
        structType = llvm::StructType::create(*impl_->context, fieldTypes, clsSym);
        impl_->classStructTypes[node.name] = structType;
    }

    // Store field->index, field->type, and field->VarKind mappings (shifted by headerOffset for GC header)
    for (unsigned i = 0; i < fields.size(); ++i) {
        impl_->classFieldIndices[node.name][fields[i].name] = i + headerOffset;
        impl_->classFieldTypes[node.name][fields[i].name] = fields[i].type;
        impl_->classFieldKinds[node.name][fields[i].name] = fields[i].kind;
    }
    // Own (non-inherited) positional field order for match destructuring - the
    // same AST helper the TypeChecker fills ClassType::fieldOrder from, so the
    // position->field-name mapping is identical across stages.
    impl_->classFieldOrder[node.name] = instanceFieldOrder(node);

    // Track list element types for class fields (from constructor param type annotations)
    // This enables correct for-in iteration over self.field where field is list[str] etc.
    for (auto* initDecl : allInitDecls) {
        std::unordered_map<std::string, size_t> paramNameToIdx;
        for (size_t pi = 0; pi < initDecl->params.size(); ++pi) {
            paramNameToIdx[initDecl->params[pi].name] = pi;
        }
        // Handle AnnAssignStmt form: `self.x: list[Callable[[A], R]] = []`.
        // The parser uses AnnAssignStmt for any `target: T = value`; the
        // AssignStmt-with-typeAnnotation scan below misses this case.
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
                impl_->classFieldListElemKinds[node.name][attrExpr->attribute] = ek;
                // Callable element: stash FunctionType for ForLoop to register
                // callableTypes for the loop variable.
                if (auto* cte = dynamic_cast<CallableTypeExpr*>(generic->typeArgs[0].get())) {
                    impl_->classFieldListElemCallableType
                        [node.name][attrExpr->attribute] =
                        impl_->callableTypeExprToFnType(cte);
                }
            } else if (base->name == "dict" && generic->typeArgs.size() >= 2) {
                // Record value kind so SubscriptExpr (`obj.field["k"]`) routes
                // through the typed runtime op for str-keyed dicts. Without this,
                // class-field dict subscripts fall through to dragon_dict_get
                // returning i64, producing a PHI type mismatch when the subscript
                // appears in a ternary alongside a str/float/list value.
                // D030 §5: direct Type::Kind from the annotation.
                Type::Kind vk = impl_->typeExprToTypeKind(generic->typeArgs[1].get());
                impl_->classFieldDictValueKinds[node.name][attrExpr->attribute] = vk;
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
                auto fkIt = impl_->classFieldKinds[node.name].find(attrExpr->attribute);
                bool fieldIsList = (fkIt != impl_->classFieldKinds[node.name].end() && fkIt->second == Impl::VarKind::List);
                // Also check if the field is assigned from a function returning list[T]
                if (!fieldIsList && assign->value) {
                    if (auto* rhsCall = dynamic_cast<CallExpr*>(assign->value.get())) {
                        if (auto* cn = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                            // Search dep modules + entry module - classes commonly
                            // call helpers defined in the same file (e.g.
                            // _split_path in stdlib/http/server.dr).
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
                                    impl_->classFieldListElemKinds[node.name][attrExpr->attribute] = ek;
                                }
                            }
                        }
                    }
                }
                // Check if RHS is a function call with list[T] return type
                if (auto* rhsCall = dynamic_cast<CallExpr*>(assign->value.get())) {
                    if (auto* calleeName = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                        // Search dep modules + entry module - classes commonly
                        // call helpers defined in the same file (e.g.
                        // _split_path in stdlib/http/server.dr).
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
                                                impl_->classFieldListElemKinds[node.name][attrExpr->attribute] = ek;
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
                            impl_->classFieldListElemKinds[node.name][attrExpr->attribute] = ek;
                            // list[Callable[[...], R]] field - record the
                            // element FunctionType for for-loop call dispatch.
                            if (auto* cte = dynamic_cast<CallableTypeExpr*>(
                                    generic->typeArgs[0].get())) {
                                impl_->classFieldListElemCallableType
                                    [node.name][attrExpr->attribute] =
                                    impl_->callableTypeExprToFnType(cte);
                            }
                        }
                    }
                }
            }
        }
        // Second pass on AssignStmt - propagate dict[K, V] value kinds for fields
        // initialized from a typed __init__ parameter (`self.params = params`).
        // The AnnAssignStmt scan above only catches `self.x: dict[...] = ...`;
        // without this branch, `obj.field["k"]` falls through to the polymorphic
        // dragon_dict_get -> i64 path and PHIs against str/ptr in ternaries.
        for (auto& bodyStmt : initDecl->body) {
            auto* assign = dynamic_cast<AssignStmt*>(bodyStmt.get());
            if (!assign) continue;
            for (auto& target : assign->targets) {
                auto* attrExpr = dynamic_cast<AttributeExpr*>(target.get());
                if (!attrExpr) continue;
                auto* selfName = dynamic_cast<NameExpr*>(attrExpr->object.get());
                if (!selfName || selfName->name != "self") continue;
                if (impl_->classFieldDictValueKinds[node.name].count(attrExpr->attribute))
                    continue;
                auto fkIt = impl_->classFieldKinds[node.name].find(attrExpr->attribute);
                if (fkIt == impl_->classFieldKinds[node.name].end() ||
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
                // Dict-literal RHS: `self.x = {"a": "1", "b": "2"}`. Infer V from
                // the entries' value kinds; only commit if all entries agree
                // (heterogeneous dicts stay polymorphic - no false-positive typing).
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
                                impl_->classFieldDictValueKinds[node.name][attrExpr->attribute] = firstVK;
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
                impl_->classFieldDictValueKinds[node.name][attrExpr->attribute] = vk;
            }
        }
    }

    // Layout pre-pass stops here: all field-layout metadata (struct type, field
    // indices/types/kinds, list-elem/dict-value kinds, field class names) is now
    // registered. Bailing before any non-idempotent work (static-field globals,
    // _new/ctor/method bodies, vtables) lets a later pass emit those once. The
    // pre-pass runs for EVERY class before any body is emitted, so a method that
    // references a class defined later in the file sees its layout (fixes the
    // forward-reference field-access miscompile).
    if (impl_->classLayoutPass) return;

    //--- 2b2: Create LLVM globals for static fields ---
    // Scan class body for AnnAssignStmt with isStatic=true.
    // Each static field becomes a global variable @ClassName_fieldName.
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
        impl_->staticFieldGlobals[node.name][target->name] = gv;

        // If the initializer is a non-trivial expression, mark for runtime init.
        // We will emit the initialization code later in the main function preamble,
        // but for now, record what needs to be initialized at runtime.
        // The actual runtime init happens when we visit the AnnAssignStmt in the
        // class body below (if we're currently inside the main function or a class visitor).
    }

    //--- 2c: Emit ClassName___init__ (single-ctor) or ClassName___init___N (multi-ctor) ---
    // Helper lambda: emit the body of a single __init__ function given its
    // FunctionDecl and the LLVM Function to populate.
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

        // First arg is always self (ptr)
        auto initFuncType = initFunc->getFunctionType();
        auto argIt = initFunc->arg_begin();

        // Alloca for self
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
            // GC: mark params as borrowed unless `own` - an `own` ctor param is
            // moved in; when the body moves it into an own field the slot is
            // nulled (scope-exit free is a no-op), and when it is only consumed
            // the scope exit releases it instead of leaking (mirrors methods /
            // free functions; ASan A/B-proven on the consumed-not-stored case).
            if (Impl::isHeapKind(paramKind) && !decl->params[i].isOwn)
                impl_->scopes.back().borrowed.insert(pname);
            // Track class-typed params for field access (e.g. other.x in dunders)
            if (auto* namedType = dynamic_cast<NamedTypeExpr*>(decl->params[i].type.get())) {
                if (impl_->classNames.count(namedType->name))
                    impl_->varClassNames[pname] = namedType->name;
            }
            ++argIt;
        }

        // Generate init body
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
        impl_->classIdGlobals[node.name] = classIdGlobal;
    }

    // Decision 026: Forward-declare vtable global (initializer set after method emission).
    llvm::GlobalVariable* vtableGlobal = nullptr;
    if (impl_->options.gcMode == GCMode::RC) {
        auto vtOrdIt = impl_->classVtableMethodOrder.find(node.name);
        if (vtOrdIt != impl_->classVtableMethodOrder.end() && !vtOrdIt->second.empty()) {
            auto* vtableArrayType = llvm::ArrayType::get(impl_->i8PtrType, vtOrdIt->second.size());
            vtableGlobal = new llvm::GlobalVariable(
                *impl_->module, vtableArrayType, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantAggregateZero::get(vtableArrayType),
                clsSym + "__vtable");
            impl_->classVtables[node.name] = vtableGlobal;
        }
    }

    // Does `initDecl` assign `self.<fname>` UNCONDITIONALLY at its top level?
    // Used to skip a per-instance default that the constructor would immediately
    // overwrite (motto #1: no dead store / wasted heap alloc+free). Only a
    // top-level assignment counts - a conditional/nested one leaves a branch in
    // which the default must still hold, so it must NOT suppress the default.
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

    // Acyclic-class optimization: an instance whose every field is a value
    // scalar (int/float/bool) or a string (a heap LEAF - holds no object
    // references) can NEVER participate in a reference cycle, so the cyclic
    // collector need not track it. Skipping `dragon_gc_track` at allocation
    // also makes the matching decref-to-zero take the lock-free fast path
    // (runtime: untracked ⇒ no gc_lock), eliminating BOTH per-object mutex
    // acquisitions. The collector stays correct for objects that DO reach an
    // untracked instance: gc_visit_subtract/_reachable look it up in the
    // tracked-set hash table and ignore a miss (treated as an external ref),
    // and dragon_dealloc skips untrack when GC_FLAG_TRACKED is clear.
    //
    // Conservative whitelist (sound, not maximal): List/Dict/Set/Tuple are
    // containers (cyclic-capable); ClassInstance/Union/Closure/Generator can
    // transitively close a cycle; bytes shares VarKind::List post-D030 so it
    // can't be distinguished here. `fields` already includes inherited fields
    // (merged above), and an unknown parent kind defaults to Other ⇒ excluded.
    bool classIsAcyclic = (impl_->options.gcMode == GCMode::RC);
    for (auto& f : fields) {
        if (f.kind != Impl::VarKind::Int && f.kind != Impl::VarKind::Float &&
            f.kind != Impl::VarKind::Bool && f.kind != Impl::VarKind::Str &&
            f.kind != Impl::VarKind::StrLiteral) {
            classIsAcyclic = false;
            break;
        }
    }

    // B Phase 1 stack-construction eligibility. STRICTER than acyclic: every
    // field must be a value scalar (int/float/bool) - a str field is a heap
    // child that would need decref when the stack instance dies, which Phase 1
    // does not emit. Also require a single constructor that does not leak
    // `self`, and no class-body per-instance defaults - neither this class's own
    // NOR any inherited from an ancestor (so the stack path's memset + __init__
    // reproduces _new's field init exactly; the stack path never runs the
    // per-instance default stores, so an inherited scalar default would be
    // silently dropped there). The escape walk over the ctor body reuses the
    // same analysis as the use-site check.
    bool anyInheritedDefaults = false;
    {
        auto pit = impl_->classParentNames.find(node.name);
        std::string cur = (pit != impl_->classParentNames.end()) ? pit->second : "";
        while (!cur.empty()) {
            auto dit = impl_->classPerInstanceDefaults.find(cur);
            if (dit != impl_->classPerInstanceDefaults.end() && !dit->second.empty()) {
                anyInheritedDefaults = true;
                break;
            }
            auto nit = impl_->classParentNames.find(cur);
            cur = (nit != impl_->classParentNames.end()) ? nit->second : "";
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
            impl_->stackEligibleClasses.insert(node.name);
    }

    // Helper lambda: emit a _new function that mallocs the struct and calls the
    // corresponding __init__, then returns the instance pointer. `initDeclForSkip`
    // is the constructor whose body is scanned to suppress redundant default
    // stores (null for the synthesized no-ctor path - nothing to skip).
    auto emitNewBody = [&](llvm::Function* newFunc, const std::string& initFuncName,
                           FunctionDecl* initDeclForSkip) {
        if (!newFunc || !newFunc->empty()) return;

        // Record the ctor's owned-arg kinds under the _new symbol so the
        // ctor call site can release owned heap-temporary arguments (mirrors
        // funcParamKinds for plain functions; __init__ is skipped by the
        // method-decl loop, so derive directly from the decl). _new's params
        // are the ctor params after self - .dr has implicit self (not in
        // params), .py lists it explicitly.
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

        // Declare malloc if not already declared
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
            // Store type_tag + gc_flags + class_id + gc_track_idx packed into i64.
            // Layout (little-endian): byte 0=type_tag(7), byte 1=gc_flags(GC_FLAG_HEAP_OBJ=0x80),
            // bytes 2-3=class_id, bytes 4-7=gc_track_idx(-1=0xFFFFFFFF)
            //
            // The HEAP_OBJ bit MUST be set here - `dragon_mark_shared_deep`
            // uses it as the heap sentinel (skips raw integers cast to ptr),
            // and dragon_obj_init sets it for non-class allocators. Pre-fix
            // class instances had gc_flags=0, so the runtime couldn't tell a
            // heap class from a stack-allocated bag of bytes.
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
            // Phase 5e: track class instance for cycle collection. Skipped for
            // acyclic classes (see classIsAcyclic above) - they can't form a
            // cycle, so RC reclaims them and the per-object gc_lock is pure
            // overhead. The header already leaves GC_FLAG_TRACKED clear and
            // gc_track_idx=-1, so an untracked instance decrefs/deallocs
            // correctly without ever touching the collector's tracked set.
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

        // Per-instance field defaults (`x: int = 5`, `items: list = []`).
        // Evaluate each initializer fresh into this instance's own slot. Runs
        // for EVERY construction path (single ctor, each multi-ctor _new_N, and
        // the no-ctor synthesized path) - the one uniform site. __init__ runs
        // AFTER and overrides via the RC-correct field store (which decrefs the
        // default we lay down here), so refcounts stay balanced. Skip a field
        // the constructor assigns unconditionally at top level to avoid a dead
        // store. The struct was memset to 0 above, so a plain CreateStore of the
        // freshly-owned default value is correct (no old value to decref).
        // Build the ordered default list BASE-FIRST, DERIVED-LAST: walk the
        // parent chain from node.name to the root, collecting each ancestor's
        // recorded defaults (classPerInstanceDefaults, persisted at the layout
        // pre-pass so this is order-independent), then append this class's own
        // local `perInstanceDefaults`. A later (more-derived) entry for the same
        // field name overrides an earlier (base) one - the field's slot is stored
        // multiple times, last-write-wins, matching Python's override semantics.
        // The own defaults intentionally come last so a subclass default wins.
        std::vector<std::pair<std::string, Expr*>> orderedDefaults;
        {
            // Collect ancestors innermost->outermost, then reverse to base-first.
            std::vector<std::string> chain;
            auto pit = impl_->classParentNames.find(node.name);
            std::string cur = (pit != impl_->classParentNames.end()) ? pit->second : "";
            while (!cur.empty()) {
                chain.push_back(cur);
                auto nit = impl_->classParentNames.find(cur);
                cur = (nit != impl_->classParentNames.end()) ? nit->second : "";
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                auto dit = impl_->classPerInstanceDefaults.find(*it);
                if (dit != impl_->classPerInstanceDefaults.end())
                    for (auto& entry : dit->second) orderedDefaults.push_back(entry);
            }
            // This class's own defaults last (derived overrides base).
            for (auto& entry : perInstanceDefaults) orderedDefaults.push_back(entry);
            // Dedupe by field name keeping the LAST (most-derived) entry.
            // Plain last-write-wins stores are not RC-correct: evaluating a
            // shadowed base default (`items: list = []`) mints a fresh +1
            // heap value the derived default's plain CreateStore then
            // overwrites with no release - one leaked base default per
            // construction (pinned by test_rc_ctor_defaults.dr).
            // Skipping the shadowed initializer entirely also means only the
            // winning default's side effects run, matching override semantics.
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
                // Resolve the slot via the SUBCLASS's merged layout - it already
                // contains inherited slots at the parent's indices, so an
                // inherited default lands in the correct offset of this instance.
                auto idxIt = impl_->classFieldIndices[node.name].find(fname);
                if (idxIt == impl_->classFieldIndices[node.name].end()) continue;
                auto tyIt = impl_->classFieldTypes[node.name].find(fname);
                if (tyIt == impl_->classFieldTypes[node.name].end()) continue;
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

        // Return self
        impl_->builder->CreateRet(self);

        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
    };

    if (!isMultiCtor) {
        // --- Single-constructor path (unchanged naming) ---
        if (initDecl) {
            std::string initFuncName = clsSym + "___init__";
            emitInitBody(initDecl, impl_->module->getFunction(initFuncName));
        } else {
            // Synthesized default constructor (no explicit `def()` in source).
            // Emit a minimal __init__(self) body that delegates to the
            // parent's zero-arg constructor when one exists (so inherited
            // field setup still runs), then returns. Python parity for
            // `class Foo(Base): pass` / `class Foo: pass`.
            std::string initFuncName = clsSym + "___init__";
            if (auto* synthInit = impl_->module->getFunction(initFuncName)) {
                auto* prevFunc = impl_->currentFunction;
                auto* prevBlock = impl_->builder->GetInsertBlock();
                impl_->currentFunction = synthInit;
                auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", synthInit);
                impl_->builder->SetInsertPoint(entry);
                // Delegate to the parent's zero-arg __init__ if present. Resolve
                // it under the PARENT's owning module, not the current one - an
                // imported parent (e.g. a subclass of a stdlib class) lives in a
                // different module, so mangling its __init__ with currentModule
                // misses and the parent ctor silently never runs, leaving every
                // inherited field zero-initialized. classOwningModule records the
                // parent's real module; fall back to current only if unknown.
                auto parentIt = impl_->classParentNames.find(node.name);
                if (parentIt != impl_->classParentNames.end()) {
                    const std::string& parentName = parentIt->second;
                    auto pmIt = impl_->classOwningModule.find(parentName);
                    const std::string& parentMod =
                        pmIt != impl_->classOwningModule.end() ? pmIt->second
                                                               : impl_->currentModuleName;
                    llvm::Function* parentInit = impl_->resolveMethodFunction(
                        parentMod, parentName, "__init__");
                    // Only delegate when the parent ctor is zero-user-arg
                    // (just self) - otherwise we'd have no values to pass.
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
        // --- Multi-constructor path: emit ___init___N and _new_N for each overload ---
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

    //--- 2d2: Phase 5 - Generate per-class __dealloc__ and register class_id ---
    if (impl_->options.gcMode == GCMode::RC) {
        // Check if this class has any heap-typed fields
        bool hasHeapFields = false;
        for (auto& f : fields) {
            if (Impl::isHeapKind(f.kind)) { hasHeapFields = true; break; }
        }
        // Bug A: a Callable[[...], R] field can hold a heap DragonClosure
        // (capturing lambda) that this instance owns a reference to. The
        // dealloc must drop that ref via the tag-aware decref so the closure
        // isn't leaked when the last instance dies.
        bool hasCallableFields = false;
        {
            auto cfIt = impl_->classFieldCallableType.find(node.name);
            if (cfIt != impl_->classFieldCallableType.end() && !cfIt->second.empty()) {
                hasCallableFields = true;
                hasHeapFields = true; // ensures the dealloc body is emitted below
            }
        }

        // docs/001-memory.md own fields: a RAW-HANDLE own field (Lock, TLS
        // engine ctx, ...) is released when the instance dies, through the
        // v1 intrinsic alloc->releaser registry (Q1 sign-off: keyed on the
        // constructor's `self.f = <alloc>()` callee - zero new user surface).
        // Heap-typed own fields need nothing here: the heap-field loop below
        // already drops the instance's reference at dealloc. An own raw field
        // whose releaser cannot be derived is a compile error, per the ADR
        // release table - never a silent leak.
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
                // Derive each own field's alloc callee from the ctor stores
                // (E8 guarantees stores are fresh expressions, so a direct
                // `self.f = <call>()` is the only shape that assigns them).
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
                // A handle that arrives through an `own` PARAM was allocated
                // elsewhere, so the ctor-store scan cannot name its releaser.
                // The same Q1 intrinsic registry, keyed by class.field, covers
                // those stdlib cases (zero new user surface).
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

        // Generate __dragon_dealloc_<mod>__<ClassName>(void* self) for classes
        // with heap fields. Mangling matches the rest of the per-class symbols
        // so two same-named classes from different modules get distinct
        // helper functions.
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

            // own raw-handle fields release FIRST (depth-first chain of
            // custody: the handle dies with its sole owner; docs/002 3.2).
            // Null-gated: an owner whose close() already freed and nulled
            // the handle releases exactly once.
            for (auto& [fname, releaser] : ownRawReleasers) {
                auto idxIt = impl_->classFieldIndices[node.name].find(fname);
                auto tyIt = impl_->classFieldTypes[node.name].find(fname);
                if (idxIt == impl_->classFieldIndices[node.name].end() ||
                    tyIt == impl_->classFieldTypes[node.name].end())
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

            // For each heap-typed field, GEP and decref. Track which field
            // names we drop here so the Callable-specific pass below does NOT
            // decref them a second time (a Callable field is
            // VarKind::Closure - a heap kind - so it is handled in THIS loop via
            // dragon_decref_callable; without de-duping, the legacy callable pass
            // would double-decref it and free the closure early -> UAF).
            std::set<std::string> deallocHandled;
            for (auto& f : fields) {
                if (!Impl::isHeapKind(f.kind)) continue;
                unsigned idx = impl_->classFieldIndices[node.name][f.name];
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
                    // Any / Union field: the value is a {i64 tag, i64 payload}
                    // box. Inttoptr on the box trips the verifier; instead,
                    // extract tag + payload and route through emitUnionDecref
                    // which only decrefs when the tag is a heap kind.
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
            // Bug A: tag-aware decref on Callable fields (no-op for bare fn ptrs).
            // Only for Callable fields NOT already dropped by the heap-field loop
            // above (a field whose kind wasn't resolved to Closure - e.g. an
            // inference miss - still needs its single tag-gated decref here).
            if (hasCallableFields) {
                auto cfIt = impl_->classFieldCallableType.find(node.name);
                if (cfIt != impl_->classFieldCallableType.end()) {
                    for (auto& [fname, _ftype] : cfIt->second) {
                        if (deallocHandled.count(fname)) continue;
                        auto fIdxIt = impl_->classFieldIndices[node.name].find(fname);
                        if (fIdxIt == impl_->classFieldIndices[node.name].end())
                            continue;
                        unsigned idx = fIdxIt->second;
                        auto* gep2 = impl_->builder->CreateStructGEP(
                            structType, self, idx, fname + "_ptr");
                        auto* fldType =
                            impl_->classFieldTypes[node.name][fname];
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

        // --- Generate per-class __dragon_traverse_ClassName(self, visit_fn, arg) ---
        // Visit fields that can form cycles (containers + class instances, NOT strings)
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
                // a Closure field can capture `self` back
                // (self.cb = lambda { ...self... }) - instance -> closure ->
                // env -> self, a real cycle the collector must be able to see.
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

            // NULL-check each field before visit - the cycle collector can
            // fire DURING `ClassName_new` after `dragon_gc_track(self)` but
            // BEFORE `__init__` has populated heap fields (e.g. when an
            // intermediate allocation in __init__ crosses gc_threshold). At
            // that moment the field is still memset-zeroed (NULL). The
            // visitor (gc_visit_reachable) dereferences `(DragonObjectHeader*)
            // child` at offset 9 and segfaults on NULL.
            for (auto& f : fields) {
                if (f.kind != Impl::VarKind::List && f.kind != Impl::VarKind::Dict &&
                    f.kind != Impl::VarKind::Tuple && f.kind != Impl::VarKind::Set &&
                    f.kind != Impl::VarKind::ClassInstance &&
                    // visit Closure fields so the cycle collector
                    // subtracts the instance -> closure internal ref. The visit
                    // fns only deref a TRACKED child, so a bare-fn-ptr Callable
                    // field (no header, never tracked) is a safe hash-miss.
                    f.kind != Impl::VarKind::Closure) continue;
                unsigned idx = impl_->classFieldIndices[node.name][f.name];
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

        // --- Generate per-class __dragon_clear_ClassName(self) ---
        // Like dealloc but also zeros fields after decref (for cycle collector clear_refs).
        // This prevents double-decref when dragon_dealloc runs after clear_refs.
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
                unsigned idx = impl_->classFieldIndices[node.name][f.name];
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

        // --- Generate per-class __dragon_mark_shared_ClassName(self, worklist) ---
        // BFS walker for SHARED propagation (d018-shared-refcount.md). Visits
        // ALL heap-typed fields including strings - different from the cycle
        // collector's __dragon_traverse_X which only visits cyclic fields.
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
                unsigned idx = impl_->classFieldIndices[node.name][f.name];
                auto* gep = impl_->builder->CreateStructGEP(structType, selfArg4, idx, f.name + "_ptr");
                auto* val = impl_->builder->CreateLoad(f.type, gep, f.name + "_val");
                if (f.kind == Impl::VarKind::Str) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_str"], {val});
                } else if (f.kind == Impl::VarKind::Union) {
                    // Any / Union box field: only the heap-tagged payload
                    // needs shared marking. Strip the tag, branch on heap-vs-
                    // not, push payload onto the worklist when heap. (int /
                    // bool / float / None payloads are inline data, no RC.)
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
                    // A Callable field may hold a bare fn ptr (no header) or a
                    // DragonClosure. Route through the tag-gated mark: the generic
                    // push reads + atomic-ORs gc_flags at offset 9, which on a
                    // bare code pointer is a write into read-only .text (SIGSEGV /
                    // corruption). dragon_mark_shared_callable gates on type_tag
                    // first, exactly like dragon_incref_callable.
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

        // Decision 025: Create descriptor global variable (zero-initialized).
        // Created BEFORE deferredClassInits push so the dci can carry the
        // per-instance pointer - main preamble reads dci.descriptorGlobal
        // directly instead of looking up the bare-keyed
        // classDescriptorGlobals map (which is last-wins for duplicate names).
        // The LLVM symbol is mangled per owning module so two same-named
        // classes get separate descriptor globals.
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
            // Bare-keyed map is best-effort for source-level lookups (e.g.
            // CallExpr decorator path): last-wins is acceptable because the
            // decorator dispatcher is per-class-name and same-named decorated
            // classes would already need an alias to disambiguate.
            impl_->classDescriptorGlobals[node.name] = descGlobalForDci;
        }

        // Register in main() preamble: store dragon_class_register_dealloc(fn) -> classIdGlobal.
        // Capture clsSym + the per-instance descriptor pointer here so the
        // main preamble can resolve the right symbol/global without trusting
        // the last-write-wins bare-keyed classDescriptorGlobals map.
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
        // GC off: still create the descriptor global so source-level
        // descriptor accesses (e.g. `Cls.__name__` via descriptor_get_name)
        // resolve. No dci push because the main preamble's deferred init
        // loop is GC-only.
        if (!impl_->classDescriptorGlobals.count(node.name)) {
            auto* descGlobal = new llvm::GlobalVariable(
                *impl_->module, impl_->i64Type, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(impl_->i64Type, 0),
                clsSym + "__descriptor");
            impl_->classDescriptorGlobals[node.name] = descGlobal;
        }
    }

    //--- 2e: Emit regular methods (instance and static) ---
    for (auto& stmt : node.body) {
        auto* methodDecl = dynamic_cast<FunctionDecl*>(stmt.get());
        if (!methodDecl || methodDecl->name == "__init__") continue;
        // D044+ - a generic-method template is never lowered; only its stamped
        // instantiations (empty typeParams, appended to this body) are emitted.
        if (!methodDecl->typeParams.empty()) continue;

        std::string methodFuncName = clsSym + "_" + methodDecl->name;
        // ADR 010: emit each overload's body under its per-index symbol (matches
        // the forward-declaration in ImplInit). Without the suffix, same-name
        // overloads collided on one symbol and only the first body was emitted.
        if (methodDecl->methodOverloadCount > 1 &&
            methodDecl->methodOverloadIndex >= 0)
            methodFuncName += "__ov" + std::to_string(methodDecl->methodOverloadIndex);
        auto* methodFunc = impl_->module->getFunction(methodFuncName);
        if (!methodFunc || !methodFunc->empty()) continue;

        // Generator method (`yield` in the body): emit it as a generator - the
        // method function is the wrapper that returns the generator object, with
        // `self` threaded into the body. See emitGeneratorFn. Registered in
        // generatorFunctions at forwardDeclareClasses time (matching condition).
        // @classmethod generators are not yet supported (see that gate).
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

        // Isolate union-member-kind tracking to this method body (mirrors the
        // free-function path in visit(FunctionDecl)). Without populating this for
        // a `T | str` union param, isinstance ELSE-narrowing could not resolve
        // the complement arm (computeElseKind returned Union), so the else branch
        // stored the raw 16-byte box straight into a native (ptr) field -
        // corrupting adjacent memory and segfaulting. Save/clear here so a param
        // entry can't leak between methods; restore after the body.
        auto savedUnionMembers = std::move(impl_->unionMemberKinds);
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
                                impl_->classFieldKinds.count(nt->name))
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
                // GC: mark params as borrowed unless `own` (see the instance
                // path below) - an `own` param is moved in and the callee's
                // scope exit releases it if the body did not move it onward.
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

            // Remaining params. A `*args`/`**kwargs` param is bound as a
            // list/dict local exactly like the free-function prologue
            // (Functions.cpp) - the callee receives one already-packed pointer.
            // The bare `*` keyword-only separator has no LLVM param and is
            // skipped. argIdx tracks the LLVM parameter position (self is 0) and
            // advances only for real params, so a skipped bare `*` can't
            // desynchronize it from argIt.
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
                                impl_->classFieldKinds.count(nt->name))
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
                // GC: mark params as borrowed - the caller owns the reference. An
                // `own` param is the exception (docs/002 2.8): the caller MOVED
                // its +1 in, so this callee owns it and scope exit releases it
                // (unless the body moved it onward, which nulls the slot). This
                // mirrors the free-function prologue (Functions.cpp); without the
                // `!isOwn` guard an `own` method param consumed but not stored
                // leaked one value per call (ASan A/B-proven).
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

        // Generate method body
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
        impl_->unionMemberKinds = std::move(savedUnionMembers);
        impl_->currentClassName = prevClassName;
        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
    }

    // D033 Phase 3: emit per-method "bound thunks" for instance methods.
    // Each thunk has closure ABI (user_args..., env) and forwards to the
    // underlying method with self loaded out of env's body. Used by
    // dragon_getattr to build a bound callable without a runtime trampoline.
    //
    // Static methods get a NULL slot - dragon_getattr returns the raw fn
    // pointer for those; classmethods reuse the instance-method thunk shape
    // (env captures the descriptor instead of self at the runtime layer).
    {
        std::string thunkClsSym = Impl::mangleClass(impl_->currentModuleName, node.name);
        auto ownIt = impl_->classOwnMethods.find(node.name);
        if (ownIt != impl_->classOwnMethods.end()) {
            for (auto& methodName : ownIt->second) {
                uint8_t kind = 0;
                auto kIt = impl_->classMethodKinds[node.name].find(methodName);
                if (kIt != impl_->classMethodKinds[node.name].end()) kind = kIt->second;
                if (kind == 1 || kind == 2) {
                    // Static (1) or @classmethod (2): the LLVM-emitted method
                    // signature has no leading self/cls param (Classes.cpp
                    // collapses both to "static-style" emission). No bind
                    // needed - getattr returns the raw fn ptr. Slot stays
                    // NULL for alignment with method_fn_ptrs indices.
                    impl_->classMethodBoundThunks[node.name][methodName] = nullptr;
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
                impl_->classMethodBoundThunks[node.name][methodName] = thunkFn;
                impl_->currentFunction = prevFunc;
                if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
            }
        }
    }

    // Decision 026: Set real vtable initializer now that all method bodies exist.
    if (vtableGlobal) {
        auto& vtableOrder = impl_->classVtableMethodOrder[node.name];
        std::vector<llvm::Constant*> vtableEntries;
        for (auto& methodName : vtableOrder) {
            // MRO lookup: check this class, then walk parent chain. Method
            // symbols are mangled per owning module - own class uses clsSym;
            // parent classes resolve via classOwningModule (which records the
            // defining module of each class at forwardDeclareClasses time).
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

    //--- 2f: Emit runtime initialization for static fields with non-literal initializers ---
    // This must be called while we're in a function context (typically main) so the
    // initialization code runs at program startup. When processing dependency module
    // classes BEFORE main() exists, defer the init to the main() preamble.
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
        auto sfIt = impl_->staticFieldGlobals.find(node.name);
        if (sfIt == impl_->staticFieldGlobals.end()) continue;
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
