// D045 privacy via leading-underscore convention: single source of truth for
// classifyName and the reserved-dunder allowlists, shared by Sema/TypeChecker/CodeGen.
#ifndef DRAGON_PRIVACY_H
#define DRAGON_PRIVACY_H

#include <string>
#include <unordered_set>

namespace dragon {

/// Visibility tier from a name's leading/trailing underscores (D045); applies
/// identically to class members and module top-level names in both .dr and .py.
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

/// Classifies a member/module name by underscore shape (pure string function).
inline NameVisibility classifyName(const std::string& n) {
    // `__x__`: reserved protocol/metadata namespace, a different axis from privacy.
    if (n.size() >= 5 && hasLeadingDunder(n) && hasTrailingDunder(n))
        return NameVisibility::ReservedDunder;
    if (hasLeadingDunder(n) && !hasTrailingDunder(n))
        return NameVisibility::Private;
    if (n.size() >= 2 && n[0] == '_' && n[1] != '_')
        return NameVisibility::Protected;
    return NameVisibility::Public; // includes bare "_" wildcard
}

/// Exactly the `__x__` dunders Dragon dispatches/synthesizes (D045 decision 1);
/// any other `__x__` member is a compile error. Extend only with a real dispatch site.
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

/// Recognized module-level metadata dunders (name bindings, not dispatched
/// protocols); any other `__x__` module top-level name is a compile error.
inline bool isReservedModuleDunder(const std::string& name) {
    static const std::unordered_set<std::string> kReserved = {
        "__name__", "__file__", "__doc__",
        "__all__", "__version__", "__author__",
    };
    return kReserved.count(name) != 0;
}

} // namespace dragon

#endif // DRAGON_PRIVACY_H
