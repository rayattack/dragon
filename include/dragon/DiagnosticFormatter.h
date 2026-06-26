#ifndef DRAGON_DIAGNOSTIC_FORMATTER_H
#define DRAGON_DIAGNOSTIC_FORMATTER_H

#include "dragon/Token.h"
#include <string>
#include <vector>

namespace dragon {

/// Style options for diagnostic output
struct DiagnosticStyle {
    bool useDragonTheme = true;   // Dragon-branded errors vs plain
    bool showSuggestions = true;   // Show suggestion hints
    bool colorOutput = false;     // ANSI color codes (default off for test stability)
};

/// Formats compiler diagnostics in Dragon's branded style
///
/// Replaces the duplicated error-printing loops in Driver.cpp with a
/// single, consistent formatting facility. Supports two modes:
///  - Dragon theme: "DRAGON SCALE ERROR: ..." with suggestions
///  - Plain mode: "file:line:col: level: message" (classic compiler style)
class DiagnosticFormatter {
public:
    explicit DiagnosticFormatter(DiagnosticStyle style = {});

    /// Format a generic diagnostic message
    /// @param filename Source file path
    /// @param line Line number
    /// @param column Column number
    /// @param level "error" or "warning"
    /// @param message The diagnostic message
    /// @param suggestion Optional suggestion text
    /// @return Formatted diagnostic string
    std::string format(const std::string& filename,
                       int line, int column,
                       const std::string& level,
                       const std::string& message,
                       const std::string& suggestion = "") const;

    /// Format a missing type hint error (for .py files)
    /// @param filename Source file path
    /// @param line Line number
    /// @param column Column number
    /// @param symbolName The symbol missing a type annotation
    /// @param context E.g. "parameter", "return type", "variable"
    /// @return Formatted Dragon Scale Error string
    std::string formatMissingType(const std::string& filename,
                                  int line, int column,
                                  const std::string& symbolName,
                                  const std::string& context = "parameter") const;

    /// Format the "borders must be secured" error for untyped .py imports
    /// @param importedFile The .py file that lacks type hints
    /// @return Formatted error string
    std::string formatUntypedImport(const std::string& importedFile) const;

    /// Get the current style
    const DiagnosticStyle& style() const { return style_; }

private:
    DiagnosticStyle style_;
};

} // namespace dragon

#endif // DRAGON_DIAGNOSTIC_FORMATTER_H
