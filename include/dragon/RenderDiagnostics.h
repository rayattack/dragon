#ifndef DRAGON_RENDER_DIAGNOSTICS_H
#define DRAGON_RENDER_DIAGNOSTICS_H

#include <string>

namespace dragon {

inline std::string unrenderableClassMessage(const std::string& site,
                                            const std::string& className) {
    return site + ": '" + className + "' has no __str__ or __repr__, so it "
           "cannot be rendered as text. Give " + className +
           " a `def __str__() -> str`, or render one of its fields instead.";
}

inline std::string unrenderableContractMessage(const std::string& site,
                                               const std::string& contractName) {
    return site + ": '" + contractName + "' is a contract, and rendering "
           "dispatches on the concrete class, which is not known here. Call a "
           "method that returns str, or take the class itself.";
}

inline std::string unrenderableTypeMessage(const std::string& site,
                                           const std::string& typeName) {
    return site + ": a value of type '" + typeName + "' cannot be rendered as "
           "text. Convert it explicitly, or render a value the compiler can "
           "turn into a string.";
}

inline std::string unknownRenderTypeMessage(const std::string& site) {
    return site + ": the compiler cannot tell what this value is, so it cannot "
           "render it. Annotate the value with its type.";
}

}

#endif
