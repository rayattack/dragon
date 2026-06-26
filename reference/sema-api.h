/**
 * Dragon Sema (Semantic Analysis) API Reference
 * ==============================================
 * Source: include/dragon/Sema.h
 *
 * Performs name resolution, scope analysis, symbol table population,
 * import resolution, and semantic validation. Runs after parsing, before
 * type checking.
 *
 * Uses the ASTVisitor pattern (47 visit methods covering all node types).
 * Uses pimpl idiom (struct Impl with std::unique_ptr<Impl> impl_).
 * NOTE: pimpl is bae
 */

#pragma once
#include <string>
#include <vector>
#include <memory>

// Forward declarations
class Module;
class ASTVisitor;
class Type;
struct SourceLocation;

// ============================================================================
// 1. SYMBOL
// ============================================================================

/**
 * Represents a symbol (variable, function, class, etc.) in the symbol table.
 */
struct Symbol {
    /** Classification of the symbol. */
    enum class Kind {
        Variable,    ///< Local or global variable
        Function,    ///< Function definition
        Class,       ///< Class definition
        Parameter,   ///< Function parameter
        Module,      ///< Imported module
        TypeAlias    ///< Type alias (type X = ...)
    };

    std::string name;                  ///< Symbol name
    Kind kind;                         ///< Symbol classification
    std::shared_ptr<Type> type;        ///< Associated type (set by TypeChecker)
    SourceLocation declaration;        ///< Source location of declaration
    bool isGlobal = false;             ///< Module-level variable
    bool isNonlocal = false;           ///< Declared nonlocal
    bool isInitialized = false;        ///< Has been assigned a value
    bool isConst = false;              ///< Dragon const binding (.dr mode)
    bool isStatic = false;             ///< Dragon static member (.dr mode)
};


// ============================================================================
// 2. SCOPE
// ============================================================================

/**
 * Represents a lexical scope (module, class, function, or block).
 * Scopes form a tree via parent pointers.
 */
class Scope {
public:
    enum class Kind {
        Module,    ///< Top-level module scope
        Class,     ///< Class body scope
        Function,  ///< Function body scope
        Block      ///< Block scope (if, for, while, etc.)
    };

    /**
     * Create a new scope.
     * @param kind Scope type
     * @param parent Enclosing scope (nullptr for module scope)
     */
    Scope(Kind kind, Scope* parent = nullptr) {}
    ~Scope() {}

    /**
     * Define a new symbol in this scope.
     * @param symbol Symbol to define
     * @return true if successful, false if name already defined in this scope
     */
    bool define(const Symbol& symbol) { return false; }

    /**
     * Look up a symbol by name, searching parent scopes recursively.
     * @param name Symbol name to find
     * @return Pointer to Symbol, or nullptr if not found in any enclosing scope
     */
    Symbol* lookup(const std::string& name) { return nullptr; }

    /**
     * Look up a symbol in this scope only (no parent search).
     * @param name Symbol name to find
     * @return Pointer to Symbol, or nullptr if not in this scope
     */
    Symbol* lookupLocal(const std::string& name) { return nullptr; }

    /**
     * Find the nearest enclosing function scope.
     * @return Function scope pointer, or nullptr if at module level
     */
    Scope* enclosingFunction() { return nullptr; }

    /**
     * Find the nearest enclosing class scope.
     * @return Class scope pointer, or nullptr if not inside a class
     */
    Scope* enclosingClass() { return nullptr; }

    /** Get the kind of this scope. */
    Kind kind() const { return {}; }

    /** Get the parent scope. */
    Scope* parent() const { return nullptr; }
};


// ============================================================================
// 3. SEMA DIAGNOSTIC
// ============================================================================

/**
 * A single diagnostic message from semantic analysis.
 */
struct SemaDiagnostic {
    enum class Level { Warning, Error };

    Level level;
    SourceLocation location;
    std::string message;
};


// ============================================================================
// 4. SEMA CLASS
// ============================================================================

/**
 * Semantic analyzer. Visits the AST to perform:
 *  - Name resolution (resolve NameExpr to Symbol)
 *  - Scope analysis (build scope tree, detect undeclared/unused variables)
 *  - Import resolution (validate import targets exist)
 *  - Declaration checking (detect duplicates, const violations)
 *
 * Implements ASTVisitor with 47 visit() methods for all node types.
 */
class Sema {
public:
    Sema() {}
    ~Sema() {}

    /**
     * Analyze a parsed module. Populates symbol tables and validates semantics.
     * @param module Parsed Module AST
     * @return true if no errors found
     */
    bool analyze(Module& module) { return false; }

    /**
     * Get all diagnostics accumulated during analysis.
     * @return Vector of SemaDiagnostic
     */
    const std::vector<SemaDiagnostic>& diagnostics() const { return {}; }

    /**
     * Check if any errors occurred.
     * @return true if at least one Error-level diagnostic
     */
    bool hasErrors() const { return false; }

    // --- ASTVisitor overrides (47 methods) ---
    // All node types listed in ast-api.h are visited.
    // See ast-api.h section 16 for the full visitor interface.
};
