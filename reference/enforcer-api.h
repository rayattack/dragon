/**
 * Dragon TypeHintEnforcer API Reference
 * ======================================
 * Source: include/dragon/TypeHintEnforcer.h
 *
 * Enforces presence of PEP-484 type annotations on .py files.
 * Separate from the TypeChecker (which validates type correctness) -
 * this only checks that annotations EXIST on functions, parameters, and variables.
 *
 * Runs between Parser and Sema in the pipeline (for .py files only).
 */

#pragma once
#include <string>
#include <vector>

// Forward declarations
class Module;
class FunctionDecl;
class ClassDecl;
class AssignStmt;
struct SourceLocation;

// ============================================================================
// 1. ENFORCER DIAGNOSTIC
// ============================================================================

struct EnforcerDiagnostic {
    enum class Level { Error, Warning };

    Level level = Level::Error;
    SourceLocation location;
    std::string message;
};


// ============================================================================
// 2. ENFORCER OPTIONS
// ============================================================================

/**
 * Configuration for type hint enforcement.
 */
struct EnforcerOptions {
    bool requireFunctionParamTypes = true;  ///< Require type hints on function parameters
    bool requireReturnTypes = true;         ///< Require return type annotations on functions
    bool requireModuleVarTypes = true;      ///< Require type hints on module-level variables
    bool isImportedModule = false;          ///< When true, uses "Borders must be secured" message
    std::string importingFile;              ///< Name of file importing this module (for error messages)
};


// ============================================================================
// 3. TYPE HINT ENFORCER CLASS
// ============================================================================

/**
 * Walks the AST and checks that all declarations have type annotations.
 * Only used for .py mode files.
 */
class TypeHintEnforcer {
public:
    /**
     * Construct an enforcer.
     * @param options What to enforce (params, returns, module vars)
     */
    explicit TypeHintEnforcer(EnforcerOptions options = {}) {}

    /**
     * Walk the module AST and check all declarations for type annotations.
     * @param module Parsed Module AST
     * @return true if all required annotations are present (no errors)
     */
    bool enforce(Module& module) { return false; }

    /**
     * Get all diagnostics produced during enforcement.
     * @return Vector of EnforcerDiagnostic
     */
    const std::vector<EnforcerDiagnostic>& diagnostics() const { return {}; }

    /**
     * Check if any errors were found.
     * @return true if at least one Error-level diagnostic
     */
    bool hasErrors() const { return false; }

    // --- Private helpers (listed for reference) ---
    // checkFunction(FunctionDecl&, isMethod) - check function params and return type
    // checkClass(ClassDecl&) - check class methods
    // checkModuleLevelAssign(AssignStmt&) - check module-level assignments
    // addError(location, message) - add error diagnostic
};
