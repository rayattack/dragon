#include "dragon/DiagnosticFormatter.h"
#include <sstream>

namespace dragon {

namespace {

const char* kRed     = "\033[1;31m";
const char* kYellow  = "\033[1;33m";
const char* kCyan    = "\033[36m";
const char* kReset   = "\033[0m";

/// Wrap `text` in an ANSI color sequence if `useColor` is true.
std::string colorize(const std::string& text, const char* color, bool useColor) {
    if (!useColor) return text;
    return std::string(color) + text + kReset;
}

} // anonymous namespace

DiagnosticFormatter::DiagnosticFormatter(DiagnosticStyle style)
    : style_(style) {}

/// Dragon theme: "DRAGON SCALE ERROR: <message> at [<file>:<line>:<col>]".
/// Plain mode: "<file>:<line>:<col>: <level>: <message>". Both take a Suggestion line.
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
            label = "DRAGON SCALE " + level;
        }

        out << colorize(label, level == "warning" ? kYellow : kRed, color)
            << ": " << message
            << " at [" << filename << ":" << line << ":" << column << "]"
            << "\n";
    } else {
        // Classic plain format: file:line:col: level: message
        out << filename << ":" << line << ":" << column << ": "
            << colorize(level, level == "warning" ? kYellow : kRed, color) << ": "
            << message << "\n";
    }

    // Optional suggestion line
    if (style_.showSuggestions && !suggestion.empty()) {
        out << "  " << colorize("Suggestion", kCyan, color)
            << ": " << suggestion << "\n";
    }

    return out.str();
}

/// Dragon theme: "DRAGON SCALE ERROR: Missing type hint ..." plus a Suggestion
/// line. Plain mode: "<file>:<line>:<col>: error: missing type annotation for <context> '<name>'".
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

/// "Borders must be secured" error for untyped Python imports; always
/// Dragon-themed regardless of the style flag.
std::string DiagnosticFormatter::formatUntypedImport(const std::string& importedFile) const {
    std::ostringstream out;

    out << "Borders must be secured: " << importedFile
        << " must be strictly typed to be imported into a Dragon context."
        << "\n";

    return out.str();
}

} // namespace dragon
