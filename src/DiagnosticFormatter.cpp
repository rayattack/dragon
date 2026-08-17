#include "dragon/DiagnosticFormatter.h"
#include <sstream>

namespace dragon {

namespace {

const char* kRed     = "\033[1;31m";
const char* kYellow  = "\033[1;33m";
const char* kCyan    = "\033[36m";
const char* kReset   = "\033[0m";

std::string colorize(const std::string& text, const char* color, bool useColor) {
    if (!useColor) return text;
    return std::string(color) + text + kReset;
}

}

DiagnosticFormatter::DiagnosticFormatter(DiagnosticStyle style)
    : style_(style) {}

std::string DiagnosticFormatter::format(const std::string& filename,
                                        int line, int column,
                                        const std::string& level,
                                        const std::string& message,
                                        const std::string& suggestion) const {
    std::ostringstream out;
    bool color = style_.colorOutput;

    if (style_.useDragonTheme) {
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
        out << filename << ":" << line << ":" << column << ": "
            << colorize(level, level == "warning" ? kYellow : kRed, color) << ": "
            << message << "\n";
    }

    if (style_.showSuggestions && !suggestion.empty()) {
        out << "  " << colorize("Suggestion", kCyan, color)
            << ": " << suggestion << "\n";
    }

    return out.str();
}

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

std::string DiagnosticFormatter::formatUntypedImport(const std::string& importedFile) const {
    std::ostringstream out;

    out << "Borders must be secured: " << importedFile
        << " must be strictly typed to be imported into a Dragon context."
        << "\n";

    return out.str();
}

}
