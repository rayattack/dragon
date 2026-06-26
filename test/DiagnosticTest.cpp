#include <gtest/gtest.h>
#include "dragon/DiagnosticFormatter.h"
#include <string>

using namespace dragon;

// ===== Dragon Theme (default) =====

TEST(DiagnosticFormatter, DragonThemeError) {
    DiagnosticFormatter fmt;
    auto result = fmt.format("test.dr", 10, 5, "error", "undeclared variable 'x'");
    EXPECT_NE(result.find("DRAGON SCALE ERROR"), std::string::npos);
    EXPECT_NE(result.find("undeclared variable 'x'"), std::string::npos);
    EXPECT_NE(result.find("test.dr:10:5"), std::string::npos);
}

TEST(DiagnosticFormatter, DragonThemeWarning) {
    DiagnosticFormatter fmt;
    auto result = fmt.format("test.dr", 3, 1, "warning", "unused variable");
    EXPECT_NE(result.find("DRAGON SCALE WARNING"), std::string::npos);
    EXPECT_NE(result.find("unused variable"), std::string::npos);
}

TEST(DiagnosticFormatter, DragonThemeCustomLevel) {
    DiagnosticFormatter fmt;
    auto result = fmt.format("test.dr", 1, 1, "note", "additional info");
    EXPECT_NE(result.find("DRAGON SCALE note"), std::string::npos);
}

TEST(DiagnosticFormatter, DragonThemeWithSuggestion) {
    DiagnosticFormatter fmt;
    auto result = fmt.format("test.dr", 10, 5, "error", "type mismatch",
                             "expected int, got str");
    EXPECT_NE(result.find("DRAGON SCALE ERROR"), std::string::npos);
    EXPECT_NE(result.find("Suggestion"), std::string::npos);
    EXPECT_NE(result.find("expected int, got str"), std::string::npos);
}

TEST(DiagnosticFormatter, DragonThemeNoSuggestionWhenEmpty) {
    DiagnosticFormatter fmt;
    auto result = fmt.format("test.dr", 1, 1, "error", "some error");
    EXPECT_EQ(result.find("Suggestion"), std::string::npos);
}

// ===== Plain Mode =====

TEST(DiagnosticFormatter, PlainModeError) {
    DiagnosticStyle style;
    style.useDragonTheme = false;
    DiagnosticFormatter fmt(style);
    auto result = fmt.format("test.py", 5, 3, "error", "syntax error");
    // Should be: test.py:5:3: error: syntax error
    EXPECT_NE(result.find("test.py:5:3: error: syntax error"), std::string::npos);
    EXPECT_EQ(result.find("DRAGON"), std::string::npos);
}

TEST(DiagnosticFormatter, PlainModeWarning) {
    DiagnosticStyle style;
    style.useDragonTheme = false;
    DiagnosticFormatter fmt(style);
    auto result = fmt.format("test.py", 12, 8, "warning", "unused import");
    EXPECT_NE(result.find("test.py:12:8: warning: unused import"), std::string::npos);
}

TEST(DiagnosticFormatter, PlainModeWithSuggestion) {
    DiagnosticStyle style;
    style.useDragonTheme = false;
    DiagnosticFormatter fmt(style);
    auto result = fmt.format("test.py", 1, 1, "error", "msg", "try this");
    EXPECT_NE(result.find("Suggestion"), std::string::npos);
    EXPECT_NE(result.find("try this"), std::string::npos);
}

// ===== Suggestion Toggle =====

TEST(DiagnosticFormatter, SuggestionsDisabled) {
    DiagnosticStyle style;
    style.showSuggestions = false;
    DiagnosticFormatter fmt(style);
    auto result = fmt.format("test.dr", 1, 1, "error", "msg", "should not appear");
    EXPECT_EQ(result.find("Suggestion"), std::string::npos);
    EXPECT_EQ(result.find("should not appear"), std::string::npos);
}

// ===== Color Output =====

TEST(DiagnosticFormatter, ColorOutputContainsAnsiCodes) {
    DiagnosticStyle style;
    style.colorOutput = true;
    DiagnosticFormatter fmt(style);
    auto result = fmt.format("test.dr", 1, 1, "error", "msg");
    // ANSI escape code \033[ should be present
    EXPECT_NE(result.find("\033["), std::string::npos);
}

TEST(DiagnosticFormatter, NoColorOutputNoAnsiCodes) {
    DiagnosticStyle style;
    style.colorOutput = false;
    DiagnosticFormatter fmt(style);
    auto result = fmt.format("test.dr", 1, 1, "error", "msg");
    EXPECT_EQ(result.find("\033["), std::string::npos);
}

// ===== Missing Type Formatter =====

TEST(DiagnosticFormatter, MissingTypeDragonTheme) {
    DiagnosticFormatter fmt;
    auto result = fmt.formatMissingType("app.py", 12, 5, "data", "parameter");
    EXPECT_NE(result.find("DRAGON SCALE ERROR"), std::string::npos);
    EXPECT_NE(result.find("Missing type hint"), std::string::npos);
    EXPECT_NE(result.find("app.py:12:5"), std::string::npos);
    EXPECT_NE(result.find("'data'"), std::string::npos);
    EXPECT_NE(result.find("parameter"), std::string::npos);
    EXPECT_NE(result.find("breathe fire"), std::string::npos);
}

TEST(DiagnosticFormatter, MissingTypePlainMode) {
    DiagnosticStyle style;
    style.useDragonTheme = false;
    DiagnosticFormatter fmt(style);
    auto result = fmt.formatMissingType("app.py", 12, 5, "data", "parameter");
    EXPECT_NE(result.find("app.py:12:5: error: missing type annotation"), std::string::npos);
    EXPECT_NE(result.find("'data'"), std::string::npos);
    EXPECT_EQ(result.find("DRAGON"), std::string::npos);
}

TEST(DiagnosticFormatter, MissingTypeReturnType) {
    DiagnosticFormatter fmt;
    auto result = fmt.formatMissingType("app.py", 5, 1, "calculate", "return type");
    EXPECT_NE(result.find("'calculate'"), std::string::npos);
    EXPECT_NE(result.find("return type"), std::string::npos);
}

// ===== Untyped Import Formatter =====

TEST(DiagnosticFormatter, UntypedImportMessage) {
    DiagnosticFormatter fmt;
    auto result = fmt.formatUntypedImport("utils.py");
    EXPECT_NE(result.find("Borders must be secured"), std::string::npos);
    EXPECT_NE(result.find("utils.py"), std::string::npos);
    EXPECT_NE(result.find("strictly typed"), std::string::npos);
}

// ===== Style Accessor =====

TEST(DiagnosticFormatter, StyleAccessor) {
    DiagnosticStyle style;
    style.useDragonTheme = false;
    style.colorOutput = true;
    style.showSuggestions = false;
    DiagnosticFormatter fmt(style);
    EXPECT_FALSE(fmt.style().useDragonTheme);
    EXPECT_TRUE(fmt.style().colorOutput);
    EXPECT_FALSE(fmt.style().showSuggestions);
}

// ===== Default Construction =====

TEST(DiagnosticFormatter, DefaultStyleIsDragonThemed) {
    DiagnosticFormatter fmt;
    EXPECT_TRUE(fmt.style().useDragonTheme);
    EXPECT_TRUE(fmt.style().showSuggestions);
    EXPECT_FALSE(fmt.style().colorOutput);
}
