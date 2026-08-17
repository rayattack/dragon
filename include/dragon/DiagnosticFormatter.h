#ifndef DRAGON_DIAGNOSTIC_FORMATTER_H
#define DRAGON_DIAGNOSTIC_FORMATTER_H

#include "dragon/Token.h"
#include <string>
#include <vector>

namespace dragon {

struct DiagnosticStyle {
    bool useDragonTheme = true;
    bool showSuggestions = true;
    bool colorOutput = false;
};

class DiagnosticFormatter {
public:
    explicit DiagnosticFormatter(DiagnosticStyle style = {});

    std::string format(const std::string& filename,
                       int line, int column,
                       const std::string& level,
                       const std::string& message,
                       const std::string& suggestion = "") const;

    std::string formatMissingType(const std::string& filename,
                                  int line, int column,
                                  const std::string& symbolName,
                                  const std::string& context = "parameter") const;

    std::string formatUntypedImport(const std::string& importedFile) const;

    const DiagnosticStyle& style() const { return style_; }

private:
    DiagnosticStyle style_;
};

}

#endif

