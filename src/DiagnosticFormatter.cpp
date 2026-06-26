#include "dragon/DiagnosticFormatter.h"
#include <sstream>

namespace dragon {

// ---------------------------------------------------------------------------
// ANSI escape helpers (only emitted when colorOutput is true)
// ---------------------------------------------------------------------------
namespace {

const char* kRed     = "\033[1;31m";
const char* kCyan    = "\033[36m";
const char* kReset   = "\033[0m";

/// Wrap `text` in an ANSI color sequence if `useColor` is true.
std::string colorize(const std::string& text, const char* color, bool useColor) {
    if (!useColor) return text;
    return std::string(color) + text + kReset;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DiagnosticFormatter::DiagnosticFormatter(DiagnosticStyle style)
    : style_(style) {}

// ---------------------------------------------------------------------------
// format() -- generic diagnostic
// ---------------------------------------------------------------------------

/// Produce a formatted diagnostic for any compiler stage.
///
/// Dragon theme:
///  DRAGON SCALE ERROR: <message> at [<filename>:<line>:<column>]
///  Suggestion: <suggestion>
///
/// Plain mode:
///  <filename>:<line>:<column>: <level>: <message>
///  Suggestion: <suggestion>
std::string DiagnosticFormatter::format(const std::string& filename,
                                        int line, int column,
                                        const std::string& level,
                                        const std::string& message,
                                        const std::string& suggestion) const {
    std::ostringstream out;
    bool color = style_.colorOutput;

    if (style_.useDragonTheme) {
        // Build the level label, e.g. "DRAGON SCALE ERROR"
        std::string label;
        if (level == "error") {
            label = "DRAGON SCALE ERROR";
        } else if (level == "warning") {
            label = "DRAGON SCALE WARNING";
        } else {
            // Fallback: uppercase the level
            label = "DRAGON SCALE " + level;
        }

        out << colorize(label, kRed, color)
            << ": " << message
            << " at [" << filename << ":" << line << ":" << column << "]"
            << "\n";
    } else {
        // Classic plain format: file:line:col: level: message
        out << filename << ":" << line << ":" << column << ": "
            << colorize(level, kRed, color) << ": "
            << message << "\n";
    }

    // Optional suggestion line
    if (style_.showSuggestions && !suggestion.empty()) {
        out << "  " << colorize("Suggestion", kCyan, color)
            << ": " << suggestion << "\n";
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// formatMissingType() -- missing type annotation
// ---------------------------------------------------------------------------

/// Produce a formatted diagnostic for a missing type hint.
///
/// Dragon theme:
///  DRAGON SCALE ERROR: Missing type hint at [<filename>:<line>:<column>]
///  <context> '<symbolName>' requires a type annotation
///  Suggestion: "To breathe fire, the Dragon needs to know this type. ..."
///
/// Plain mode:
///  <filename>:<line>:<column>: error: missing type annotation for <context> '<symbolName>'
std::string DiagnosticFormatter::formatMissingType(const std::string& filename,
                                                   int line, int column,
                                                   const std::string& symbolName,
                                                   const std::string& context) const {
    std::ostringstream out;
    bool color = style_.colorOutput;

    if (style_.useDragonTheme) {
        out << colorize("DRAGON SCALE ERROR", kRed, color)
            << ": Missing type hint at [" << filename << ":" << line << ":" << column << "]"
            << "\n";
        out << "  " << context << " '" << symbolName << "' requires a type annotation"
            << "\n";
        if (style_.showSuggestions) {
            out << "  " << colorize("Suggestion", kCyan, color)
                << ": \"To breathe fire, the Dragon needs to know this type."
                << " Add ': int', ': str', etc.\"\n";
        }
    } else {
        out << filename << ":" << line << ":" << column << ": "
            << colorize("error", kRed, color)
            << ": missing type annotation for " << context
            << " '" << symbolName << "'" << "\n";
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// formatUntypedImport() -- untyped .py import
// ---------------------------------------------------------------------------

/// Produce the "borders must be secured" error for untyped Python imports.
/// This diagnostic is always Dragon-themed regardless of the style flag,
/// because it represents a core Dragon philosophy about type safety at
/// module boundaries.
std::string DiagnosticFormatter::formatUntypedImport(const std::string& importedFile) const {
    std::ostringstream out;

    out << "Borders must be secured: " << importedFile
        << " must be strictly typed to be imported into a Dragon context."
        << "\n";

    return out.str();
}

} // namespace dragon
