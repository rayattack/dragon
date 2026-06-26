#include "dragon/TypeHintEnforcer.h"

#include <set>

namespace dragon {

namespace {

/// Collect every name bound by an assignment *target* that, per the Dragon
/// declare-vs-reassign rule (`:` declares, `=` reassigns - Sema.cpp visit
/// AssignStmt / AnnAssignStmt), introduces a binding for the name. A bare
/// single `NameExpr` target only declares when the statement carries an
/// annotation (the vestigial AssignStmt::typeAnnotation form); without one it
/// is a reassignment and is handled by the caller. Tuple-unpack targets have
/// no annotation slot and implicitly declare each name. Subscript/attribute
/// targets are mutations and never declare.
void collectImplicitlyDeclaredNames(Expr* target, std::set<std::string>& out) {
    if (auto* name = dynamic_cast<NameExpr*>(target)) {
        out.insert(name->name);
    } else if (auto* tup = dynamic_cast<TupleExpr*>(target)) {
        for (auto& e : tup->elements) collectImplicitlyDeclaredNames(e.get(), out);
    } else if (auto* lst = dynamic_cast<ListExpr*>(target)) {
        // `[a, b] = pair` unpacking - also an annotation-less binding form.
        for (auto& e : lst->elements) collectImplicitlyDeclaredNames(e.get(), out);
    }
    // SubscriptExpr / AttributeExpr targets mutate an existing object and
    // never introduce a module-level binding - intentionally ignored.
}

} // namespace

TypeHintEnforcer::TypeHintEnforcer(EnforcerOptions options)
    : options_(std::move(options)) {}

bool TypeHintEnforcer::enforce(Module& module) {
    diagnostics_.clear();

    // The enforcer runs before name resolution, so it cannot ask Sema whether a
    // bare `name = ...` reassigns an existing binding or introduces a new one.
    // We track module-level declared names ourselves, mirroring the Sema rule
    // (`:` declares, `=` reassigns): an annotation introduces a binding, a bare
    // `=` only reassigns. Only a *first* bare-NameExpr declaration must carry an
    // annotation; reassignments and subscript/attribute/tuple mutations do not.
    std::set<std::string> declaredModuleNames;

    for (auto& stmt : module.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            checkFunction(*func);
        } else if (auto* cls = dynamic_cast<ClassDecl*>(stmt.get())) {
            checkClass(*cls);
        } else if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            // `x: int = 0` is the declaration form; record the name so a later
            // bare `x = x + 1` is recognized as a reassignment, not a new var.
            collectImplicitlyDeclaredNames(ann->target.get(), declaredModuleNames);
        } else if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            // Decide declare-vs-reassign before tracking, since the binding a
            // statement introduces is visible only to *subsequent* statements.
            const bool isBareSingleName =
                assign->targets.size() == 1 &&
                dynamic_cast<NameExpr*>(assign->targets[0].get()) != nullptr;
            const bool isReassign =
                isBareSingleName && !assign->typeAnnotation &&
                declaredModuleNames.count(
                    static_cast<NameExpr*>(assign->targets[0].get())->name) != 0;

            // A subscript/attribute target (`cache["k"] = v`, `obj.f = v`) and a
            // bare reassignment (`counter = counter + 1`) are mutations - never
            // require an annotation. Only a genuine first declaration is checked.
            if (!isReassign) checkModuleLevelAssign(*assign);

            // Record any names this statement binds for later reassignments:
            // the vestigial annotated form, and annotation-less tuple/list
            // unpacking. A bare single `=` introduces no new binding here -
            // it either errored above (undeclared) or was already declared.
            if (assign->typeAnnotation || !isBareSingleName) {
                for (auto& target : assign->targets) {
                    collectImplicitlyDeclaredNames(target.get(), declaredModuleNames);
                }
            }
        }
    }

    return !hasErrors();
}

bool TypeHintEnforcer::hasErrors() const {
    for (const auto& diag : diagnostics_) {
        if (diag.level == EnforcerDiagnostic::Level::Error) return true;
    }
    return false;
}

void TypeHintEnforcer::checkFunction(FunctionDecl& func, bool isMethod) {
    if (!options_.requireFunctionParamTypes && !options_.requireReturnTypes)
        return;

    // Check parameter types
    if (options_.requireFunctionParamTypes) {
        for (size_t i = 0; i < func.params.size(); ++i) {
            auto& param = func.params[i];

            // Skip 'self' and 'cls' as first parameter of methods
            if (isMethod && i == 0 &&
                (param.name == "self" || param.name == "cls")) {
                continue;
            }

            // *args and **kwargs don't strictly require type annotations
            if (param.isVarArg || param.isKwArg) continue;

            if (!param.type) {
                addError(func.location(),
                    "missing type annotation for parameter '" + param.name +
                    "' in function '" + func.name + "'");
            }
        }
    }

    // Check return type
    if (options_.requireReturnTypes) {
        // __init__ implicitly returns None
        if (func.name != "__init__" && !func.returnType) {
            addError(func.location(),
                "missing return type annotation for function '" + func.name + "'");
        }
    }
}

void TypeHintEnforcer::checkClass(ClassDecl& cls) {
    for (auto& stmt : cls.body) {
        if (auto* method = dynamic_cast<FunctionDecl*>(stmt.get())) {
            checkFunction(*method, /*isMethod=*/true);
        }
    }
}

void TypeHintEnforcer::checkModuleLevelAssign(AssignStmt& assign) {
    if (!options_.requireModuleVarTypes) return;

    // A statement that already carries an annotation is a complete declaration.
    if (assign.typeAnnotation) return;

    // `enforce()` only routes genuine first declarations here (it filters out
    // bare reassignments of already-declared names). The remaining cases that
    // must still NOT require an annotation are the implicit-binding forms that
    // have no annotation slot, mirroring Sema's AssignStmt rule:
    //  - subscript/attribute targets (`cache["k"] = v`, `obj.f = v`) mutate an
    //  existing object - they declare nothing.
    //  - tuple/list unpacking and chained `a = b = c` implicitly declare with
    //  an inferred type.
    // Only a bare single `NameExpr` target is a declaration that demands an
    // explicit annotation.
    if (assign.targets.size() != 1) return;
    auto* name = dynamic_cast<NameExpr*>(assign.targets[0].get());
    if (!name) return;

    const std::string& varName = name->name;

    // Skip dunder variables like __all__, __version__, etc.
    if (varName.size() >= 4 && varName.substr(0, 2) == "__" &&
        varName.substr(varName.size() - 2) == "__") {
        return;
    }

    addError(assign.location(),
        "missing type annotation for module-level variable '" + varName + "'");
}

void TypeHintEnforcer::addError(SourceLocation loc, const std::string& message) {
    EnforcerDiagnostic diag;
    diag.level = EnforcerDiagnostic::Level::Error;
    diag.location = loc;
    diag.message = message;
    diagnostics_.push_back(std::move(diag));
}

} // namespace dragon
