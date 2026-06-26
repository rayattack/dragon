/**
 * Dragon DiagnosticFormatter API Reference
 * =========================================
 * Source: include/dragon/DiagnosticFormatter.h
 *
 * Formats compiler diagnostics in Dragon's branded style with colored output,
 * source location info, and suggestion hints.
 *
 * Used by the Driver to format errors/warnings from all compiler stages
 * (Lexer, Parser, Sema, TypeChecker, TypeHintEnforcer, CodeGen).
 */

#pragma once
#include <string>

// ============================================================================
// 1. DIAGNOSTIC STYLE
// ============================================================================

/**
 * Style options controlling diagnostic output appearance.
 */
struct DiagnosticStyle {
    bool useDragonTheme = true;   ///< Use Dragon-branded error format (vs plain)
    bool showSuggestions = true;   ///< Show suggestion/hint text below errors
    bool colorOutput = false;      ///< Use ANSI color codes (false for test stability)
};


// ============================================================================
// 2. DIAGNOSTIC FORMATTER CLASS
// ============================================================================

/**
 * Formats compiler diagnostics into human-readable error/warning messages.
 */
class DiagnosticFormatter {
public:
    /**
     * Construct a formatter with style options.
     * @param style Appearance configuration
     */
    explicit DiagnosticFormatter(DiagnosticStyle style = {}) {}

    /**
     * Format a generic diagnostic message.
     * @param filename Source file path
     * @param line 1-based line number
     * @param column 1-based column number
     * @param level Severity string ("error", "warning")
     * @param message Diagnostic message text
     * @param suggestion Optional suggestion/hint text (empty for none)
     * @return Formatted multi-line string ready for output
     */
    std::string format(
        const std::string& filename,
        int line,
        int column,
        const std::string& level,
        const std::string& message,
        const std::string& suggestion = ""
    ) const { return {}; }

    /**
     * Format a missing type hint error for .py files.
     * Used by the TypeHintEnforcer for Python-mode type annotation enforcement.
     * @param filename Source file path
     * @param line Line number
     * @param column Column number
     * @param symbolName Name of the untyped symbol (param, function, variable)
     * @param context What kind of symbol ("parameter", "return type", "variable")
     * @return Formatted error string
     */
    std::string formatMissingType(
        const std::string& filename,
        int line,
        int column,
        const std::string& symbolName,
        const std::string& context = "parameter"
    ) const { return {}; }

    /**
     * Format the "borders must be secured" error for importing untyped .py files.
     * Shown when a .dr file imports a .py file that lacks type annotations.
     * @param importedFile Path to the untyped .py file
     * @return Formatted error string
     */
    std::string formatUntypedImport(const std::string& importedFile) const { return {}; }

    /**
     * Get the current style configuration.
     * @return Reference to DiagnosticStyle
     */
    const DiagnosticStyle& style() const { return {}; }
};
