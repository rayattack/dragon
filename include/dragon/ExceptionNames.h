#ifndef DRAGON_EXCEPTION_NAMES_H
#define DRAGON_EXCEPTION_NAMES_H

#include <string>
#include <unordered_set>

namespace dragon {

inline bool isBuiltinExceptionName(const std::string& name) {
    static const std::unordered_set<std::string> names = {
        "BaseException", "SystemExit", "KeyboardInterrupt", "GeneratorExit",
        "Exception", "StopIteration",
        "ArithmeticError", "FloatingPointError", "OverflowError", "ZeroDivisionError",
        "AssertionError", "AttributeError", "BufferError", "EOFError",
        "ImportError", "ModuleNotFoundError",
        "LookupError", "IndexError", "KeyError",
        "MemoryError", "NameError", "UnboundLocalError",
        "OSError", "IOError", "FileNotFoundError", "FileExistsError", "IsADirectoryError",
        "NotADirectoryError", "PermissionError", "TimeoutError",
        "ConnectionError", "BrokenPipeError", "ConnectionAbortedError",
        "ConnectionRefusedError", "ConnectionResetError",
        "RuntimeError", "NotImplementedError", "RecursionError",
        "StopAsyncIteration", "SyntaxError", "TypeError",
        "ValueError", "UnicodeError", "UnicodeDecodeError",
        "UnicodeEncodeError", "UnicodeTranslateError",
        "Warning", "DeprecationWarning", "FutureWarning",
        "ResourceWarning", "RuntimeWarning", "UserWarning"
    };
    return names.count(name) > 0;
}

}

#endif
