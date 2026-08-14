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

/// Formats compiler diagnostics; replaces Driver.cpp's duplicated print loops.
/// Dragon theme: "DRAGON SCALE ERROR: ..."; plain: "file:line:col: level: message".
class DiagnosticFormatter {
public:
    explicit DiagnosticFormatter(DiagnosticStyle style = {});

    std::string format(const std::string& filename,
                       int line, int column,
                       const std::string& level,
                       const std::string& message,
                       const std::string& suggestion = "") const;

    /// Missing type hint error for .py files; context e.g. "parameter", "return type".
    std::string formatMissingType(const std::string& filename,
                                  int line, int column,
                                  const std::string& symbolName,
                                  const std::string& context = "parameter") const;

    /// "Borders must be secured" error for an untyped .py import.
    std::string formatUntypedImport(const std::string& importedFile) const;

    const DiagnosticStyle& style() const { return style_; }

private:
    DiagnosticStyle style_;
};

} // namespace dragon

#endif // DRAGON_DIAGNOSTIC_FORMATTER_H

