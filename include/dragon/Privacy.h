// Decision 045 - Enforced Member & Module Privacy via the leading-underscore
// convention. This header is the SINGLE SOURCE OF TRUTH for two things shared
// across compiler stages (Sema, TypeChecker, CodeGen):
//
//  1. `classifyName` - the name-shape classifier (public / _protected /
//  __private / reserved-dunder), applied identically to class members and
//  module-level names.
//  2. `isReservedDunder` / `isReservedModuleDunder` - the recognized-dunder
//  allowlists. They MUST stay in lockstep with the dunders codegen actually
//  dispatches/exposes: a dunder codegen handles but the predicate rejects
//  makes valid programs uncompilable; a name the predicate accepts but
//  codegen never dispatches is a "recognized-but-dead" wart D045 forbids.
//  A regression test asserts every dispatched dunder is reserved.
//
// The whole feature is 100% compile-time (commandment #1 untouched): nothing
// here influences codegen output or runtime cost - it only gates diagnostics.
#ifndef DRAGON_PRIVACY_H
#define DRAGON_PRIVACY_H

#include <string>
#include <unordered_set>

namespace dragon {

/// Visibility tier implied by a name's leading/trailing underscores (D045).
/// Applies to class members AND module top-level names - surface ≠ semantics,
/// so `.dr` and `.py` classify identically.
enum class NameVisibility {
    Public,         // `name` - exportable / importable / accessible anywhere
    Protected,      // `_name` - same package, plus subclasses anywhere
    Private,        // `__name` - declaring class only / declaring file only
    ReservedDunder, // `__name__` - reserved protocol/metadata namespace (public)
};

inline bool hasLeadingDunder(const std::string& n) {
    return n.size() >= 3 && n[0] == '_' && n[1] == '_';
}

inline bool hasTrailingDunder(const std::string& n) {
    return n.size() >= 3 && n[n.size() - 1] == '_' && n[n.size() - 2] == '_';
}

/// Classify a member/module name by underscore shape. Pure string function -
/// no AST/type context. `_` (the wildcard) and ordinary names are Public.
inline NameVisibility classifyName(const std::string& n) {
    // `__x__` (leading AND trailing double underscore, non-empty body) is the
    // reserved protocol+metadata namespace - a DIFFERENT axis from privacy.
    if (n.size() >= 5 && hasLeadingDunder(n) && hasTrailingDunder(n))
        return NameVisibility::ReservedDunder;
    // `__x` (leading double, NO trailing double) - private (strict tier).
    if (hasLeadingDunder(n) && !hasTrailingDunder(n))
        return NameVisibility::Private;
    // `_x` (single leading underscore, not double) - protected (soft tier).
    if (n.size() >= 2 && n[0] == '_' && n[1] != '_')
        return NameVisibility::Protected;
    return NameVisibility::Public; // includes bare "_" wildcard
}

/// The recognized class-member dunders - EXACTLY the set Dragon dispatches or
/// the compiler synthesizes/exposes today. Defining a `__x__` member NOT in
/// this set is a compile error (D045 decision 1). Extending it is a deliberate
/// edit that must also add a dispatch site. Derived from the codegen dispatch
/// sites enumerated in the D045 investigation (Expressions.cpp binary/unary,
/// Assign.cpp subscript/augassign, CallBuiltins.cpp str/repr/len/abs/int/float/
/// hash, Attributes.cpp getitem/__doc__, ForLoop.cpp iter/next, Exceptions.cpp
/// enter/exit, CallExpr.cpp call, CodeGenImpl.h toBool __bool__, ImplInit ctor;
/// __members__ is the compiler-synthesized enum field).
inline bool isReservedDunder(const std::string& name) {
    static const std::unordered_set<std::string> kReserved = {
        // Lifecycle / construction
        "__init__",
        // Representation
        "__str__", "__repr__",
        // Comparison
        "__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__", "__hash__",
        // Arithmetic (and in-place variants Dragon dispatches)
        "__add__", "__sub__", "__mul__", "__truediv__", "__floordiv__",
        "__mod__", "__pow__",
        "__iadd__", "__isub__", "__imul__", "__itruediv__", "__ifloordiv__",
        "__imod__", "__ipow__",
        "__neg__", "__pos__", "__abs__",
        // Container protocol
        "__len__", "__getitem__", "__setitem__", "__contains__",
        "__iter__", "__next__",
        // Callable / context
        "__call__", "__enter__", "__exit__",
        // Conversion / truthiness
        "__bool__", "__int__", "__float__",
        // Introspection / compiler-provided
        "__doc__", "__members__",
    };
    return kReserved.count(name) != 0;
}

/// The recognized module-level metadata dunders. A `__x__` module-top-level
/// name not in this set is a compile error. These are name bindings, not
/// dispatched protocols, so the set blesses the metadata surface D045 names
/// (`__name__`/`__file__` are Sema builtins; `__all__`/`__version__`/
/// `__author__`/`__doc__` are recognized metadata).
inline bool isReservedModuleDunder(const std::string& name) {
    static const std::unordered_set<std::string> kReserved = {
        "__name__", "__file__", "__doc__",
        "__all__", "__version__", "__author__",
    };
    return kReserved.count(name) != 0;
}

} // namespace dragon

#endif // DRAGON_PRIVACY_H
