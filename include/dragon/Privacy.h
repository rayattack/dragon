#ifndef DRAGON_PRIVACY_H
#define DRAGON_PRIVACY_H

#include <string>
#include <unordered_set>

namespace dragon {

enum class NameVisibility {
    Public,
    Protected,
    Private,
    ReservedDunder,
};

inline bool hasLeadingDunder(const std::string& n) {
    return n.size() >= 3 && n[0] == '_' && n[1] == '_';
}

inline bool hasTrailingDunder(const std::string& n) {
    return n.size() >= 3 && n[n.size() - 1] == '_' && n[n.size() - 2] == '_';
}

inline NameVisibility classifyName(const std::string& n) {
    if (n.size() >= 5 && hasLeadingDunder(n) && hasTrailingDunder(n))
        return NameVisibility::ReservedDunder;
    if (hasLeadingDunder(n) && !hasTrailingDunder(n))
        return NameVisibility::Private;
    if (n.size() >= 2 && n[0] == '_' && n[1] != '_')
        return NameVisibility::Protected;
    return NameVisibility::Public;
}

inline bool isReservedDunder(const std::string& name) {
    static const std::unordered_set<std::string> kReserved = {
        "__init__",
        "__str__", "__repr__",
        "__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__", "__hash__",
        "__add__", "__sub__", "__mul__", "__truediv__", "__floordiv__",
        "__mod__", "__pow__",
        "__iadd__", "__isub__", "__imul__", "__itruediv__", "__ifloordiv__",
        "__imod__", "__ipow__",
        "__neg__", "__pos__", "__abs__",
        "__len__", "__getitem__", "__setitem__", "__contains__",
        "__iter__", "__next__",
        "__call__", "__enter__", "__exit__",
        "__bool__", "__int__", "__float__",
        "__doc__", "__members__",
    };
    return kReserved.count(name) != 0;
}

inline bool isReservedModuleDunder(const std::string& name) {
    static const std::unordered_set<std::string> kReserved = {
        "__name__", "__file__", "__doc__",
        "__all__", "__version__", "__author__",
    };
    return kReserved.count(name) != 0;
}

}

#endif
