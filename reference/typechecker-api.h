/**
 * Dragon TypeChecker API Reference
 * =================================
 * Source: include/dragon/TypeChecker.h
 *
 * Two responsibilities:
 *  1. Type system - class hierarchy representing Dragon types
 *  2. TypeChecker - validates type correctness across the AST
 *
 * Type hierarchy: Type (base) with 14 concrete subclasses.
 * Subtyping rules: bool <: int <: float; everything <: Any; Never <: everything.
 *
 * Uses the ASTVisitor pattern (56 visit methods).
 * Uses pimpl idiom (struct Impl with std::unique_ptr<Impl> impl_).
 * NOTE: pimpl is bae
 */

#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// Forward declarations
class Module;
struct SourceLocation;

// ============================================================================
// 1. TYPE BASE CLASS
// ============================================================================

/**
 * Abstract base for all Dragon types.
 */
class Type {
public:
    /** Classification of all possible types. */
    enum class Kind {
        Int, Float, Bool, Str, Bytes, None_,   // Primitives
        List, Dict, Set, Tuple,                 // Containers
        Function,                               // Callable
        Class, Instance,                        // OOP
        Any, Never, Union, Optional,            // Special
        TypeVar, Unknown, Ptr                   // Generic/FFI
    };

    virtual ~Type() = default;

    /** Get the kind of this type. */
    virtual Kind kind() const = 0;

    /** Human-readable string (e.g., "int", "list[str]", "dict[str, int]"). */
    virtual std::string toString() const = 0;

    /** Structural equality check. */
    virtual bool equals(const Type& other) const = 0;

    /**
     * Subtype relationship. Default checks equals() or target is Any.
     * Overridden by PrimitiveType (bool<:int<:float), UnionType, AnyType, NeverType.
     */
    virtual bool isSubtypeOf(const Type& other) const { return false; }

    /**
     * Check if this type can be assigned to another.
     * Accounts for subtyping, union compatibility, and Any.
     */
    bool isAssignableTo(const Type& other) const { return false; }
};


// ============================================================================
// 2. CONCRETE TYPE CLASSES (14 types)
// ============================================================================

/**
 * Primitive types: int, float, bool, str, bytes, None.
 * Subtyping: bool <: int <: float.
 */
class PrimitiveType : public Type {
public:
    explicit PrimitiveType(Kind k) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
    bool isSubtypeOf(const Type& other) const override { return false; }
};

/** List type: list[T]. */
class ListType : public Type {
public:
    std::shared_ptr<Type> elementType;  ///< Element type

    explicit ListType(std::shared_ptr<Type> elem) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Dictionary type: dict[K, V]. */
class DictType : public Type {
public:
    std::shared_ptr<Type> keyType;    ///< Key type (typically str)
    std::shared_ptr<Type> valueType;  ///< Value type

    DictType(std::shared_ptr<Type> k, std::shared_ptr<Type> v) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Tuple type: tuple[T1, T2, ...]. Fixed-length, heterogeneous. */
class TupleType : public Type {
public:
    std::vector<std::shared_ptr<Type>> elementTypes;

    explicit TupleType(std::vector<std::shared_ptr<Type>> elems) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Function type: (T1, T2, ...) -> R. */
class FunctionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> paramTypes;
    std::shared_ptr<Type> returnType;

    FunctionType(std::vector<std::shared_ptr<Type>> params, std::shared_ptr<Type> ret) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Class type - represents the class itself (not instances). */
class ClassType : public Type {
public:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<Type>> methods;   ///< Method signatures
    std::unordered_map<std::string, std::shared_ptr<Type>> fields;    ///< Field types
    std::shared_ptr<Type> parentClass;                                 ///< Base class (for inheritance)

    explicit ClassType(std::string n) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Instance type - represents an instance of a class. */
class InstanceType : public Type {
public:
    std::shared_ptr<ClassType> classType;  ///< The class this is an instance of

    explicit InstanceType(std::shared_ptr<ClassType> cls) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Union type: T1 | T2 | T3. */
class UnionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> types;  ///< Union members

    explicit UnionType(std::vector<std::shared_ptr<Type>> ts) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
    bool isSubtypeOf(const Type& other) const override { return false; }
};

/** Any type - compatible with everything (top type). */
class AnyType : public Type {
public:
    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

/** Never type - bottom type, subtype of everything. For functions that never return. */
class NeverType : public Type {
public:
    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

/** Unknown type - used when type cannot be determined. */
class UnknownType : public Type {
public:
    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

/** Ptr type - raw C pointer (void*/i8*) for extern "C" FFI. */
class PtrType : public Type {
public:
    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};

/** Type variable - for future generics support. */
class TypeVarType : public Type {
public:
    std::string name;

    explicit TypeVarType(std::string n) {}

    Kind kind() const override { return {}; }
    std::string toString() const override { return {}; }
    bool equals(const Type& other) const override { return false; }
};


// ============================================================================
// 3. TYPE DIAGNOSTIC
// ============================================================================

struct TypeDiagnostic {
    enum class Level { Warning, Error };

    Level level;
    SourceLocation location;
    std::string message;
};


// ============================================================================
// 4. TYPECHECKER CLASS
// ============================================================================

/**
 * Type checker enforcing Dragon's type system.
 * Walks the AST, infers types, checks assignments and calls, and reports errors.
 *
 * Implements ASTVisitor with 56 visit() methods for all node types.
 */
class TypeChecker {
public:
    TypeChecker() {}
    ~TypeChecker() {}

    /**
     * Perform type checking on a module.
     * @param module Parsed and semantically-analyzed Module AST
     * @return true if no type errors found
     */
    bool check(Module& module) { return false; }

    /**
     * Get all type checking diagnostics.
     * @return Vector of TypeDiagnostic
     */
    const std::vector<TypeDiagnostic>& diagnostics() const { return {}; }

    /**
     * Check if any type errors occurred.
     * @return true if at least one Error-level diagnostic
     */
    bool hasErrors() const { return false; }

    /**
     * Register type information from an external (imported) module.
     * Must be called BEFORE check() for importing modules.
     * @param moduleName Name of the external module
     * @param exports Map of exported symbol name -> Type
     */
    void registerExternalModule(
        const std::string& moduleName,
        const std::unordered_map<std::string, std::shared_ptr<Type>>& exports
    ) {}

    /**
     * Get all module-level exports after type checking.
     * Used to feed into registerExternalModule() for dependent modules.
     * @return Map of exported symbol name -> Type
     */
    std::unordered_map<std::string, std::shared_ptr<Type>> getExports() const { return {}; }

    // --- ASTVisitor overrides (56 methods) ---
    // All node types listed in ast-api.h are visited.
    // See ast-api.h section 16 for the full visitor interface.

    // --- Private helpers (listed for reference) ---
    // resolveType(TypeExpr*) -> shared_ptr<Type> - resolve type annotation to concrete Type
    // inferType(Expr*) -> shared_ptr<Type> - infer expression type
    // initBuiltinTypes() -> void - initialize int, str, float, bool, etc.
    // error(loc, message) -> void - record error diagnostic
    // warning(loc, message) -> void - record warning diagnostic
};
