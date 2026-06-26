#include <gtest/gtest.h>
#include "TestHelpers.h"

using namespace dragon;
using namespace dragon::test;

//===----------------------------------------------------------------------===//
// Basic Tests
//===----------------------------------------------------------------------===//

TEST(LexerTest, EmptyInput) {
    auto tokens = lex("");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), TokenType::END_OF_FILE);
}

TEST(LexerTest, WhitespaceOnly) {
    auto tokens = lex("   \t  ");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), TokenType::END_OF_FILE);
}

TEST(LexerTest, CommentOnly) {
    auto tokens = lex("# this is a comment");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), TokenType::END_OF_FILE);
}

TEST(LexerTest, CommentAfterToken) {
    auto tokens = lex("42 # this is a comment");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[0].lexeme(), "42");
    EXPECT_EQ(tokens[1].type(), TokenType::END_OF_FILE);
}

TEST(LexerTest, NoErrors) {
    auto diags = lexErrors("42");
    EXPECT_TRUE(diags.empty());
}

//===----------------------------------------------------------------------===//
// Integer Literals
//===----------------------------------------------------------------------===//

TEST(LexerTest, DecimalInteger) {
    auto tokens = lex("42");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[0].lexeme(), "42");
}

TEST(LexerTest, ZeroInteger) {
    auto tokens = lex("0");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
}

TEST(LexerTest, LargeInteger) {
    auto tokens = lex("1234567890");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
}

TEST(LexerTest, HexInteger) {
    auto tokens = lex("0x1F");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[0].lexeme(), "0x1F");
}

TEST(LexerTest, HexIntegerLower) {
    auto tokens = lex("0xff");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
}

TEST(LexerTest, BinaryInteger) {
    auto tokens = lex("0b1010");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[0].lexeme(), "0b1010");
}

TEST(LexerTest, OctalInteger) {
    auto tokens = lex("0o77");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[0].lexeme(), "0o77");
}

TEST(LexerTest, UnderscoreInInteger) {
    auto tokens = lex("1_000_000");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[0].lexeme(), "1_000_000");
}

//===----------------------------------------------------------------------===//
// Float Literals
//===----------------------------------------------------------------------===//

TEST(LexerTest, SimpleFloat) {
    auto tokens = lex("3.14");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::FLOAT);
    EXPECT_EQ(tokens[0].lexeme(), "3.14");
}

TEST(LexerTest, ScientificNotation) {
    auto tokens = lex("1e10");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::FLOAT);
    EXPECT_EQ(tokens[0].lexeme(), "1e10");
}

TEST(LexerTest, ScientificNotationNegative) {
    auto tokens = lex("1.5e-3");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::FLOAT);
    EXPECT_EQ(tokens[0].lexeme(), "1.5e-3");
}

TEST(LexerTest, ScientificNotationPositive) {
    auto tokens = lex("2.5E+10");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::FLOAT);
}

//===----------------------------------------------------------------------===//
// String Literals
//===----------------------------------------------------------------------===//

TEST(LexerTest, DoubleQuotedString) {
    auto tokens = lex("\"hello\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme(), "\"hello\"");
}

TEST(LexerTest, SingleQuotedString) {
    auto tokens = lex("'world'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme(), "'world'");
}

TEST(LexerTest, EmptyString) {
    auto tokens = lex("\"\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, StringWithEscapes) {
    auto tokens = lex("\"hello\\nworld\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, StringWithEscapedQuote) {
    auto tokens = lex("\"say \\\"hi\\\"\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, TripleQuotedString) {
    auto tokens = lex("\"\"\"hello\nworld\"\"\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, TripleSingleQuotedString) {
    auto tokens = lex("'''multi\nline'''");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, FString) {
    auto tokens = lex("f\"hello {name}\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, RawString) {
    auto tokens = lex("r\"raw\\nstring\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, ByteString) {
    auto tokens = lex("b\"bytes\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::STRING);
}

TEST(LexerTest, UnterminatedString) {
    auto diags = lexErrors("\"unterminated");
    EXPECT_FALSE(diags.empty());
}

TEST(LexerTest, UnterminatedStringNewline) {
    auto diags = lexErrors("\"hello\n");
    EXPECT_FALSE(diags.empty());
}

//===----------------------------------------------------------------------===//
// Boolean and None Literals
//===----------------------------------------------------------------------===//

TEST(LexerTest, TrueLiteral) {
    auto tokens = lex("True");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TRUE);
}

TEST(LexerTest, FalseLiteral) {
    auto tokens = lex("False");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::FALSE);
}

TEST(LexerTest, NoneLiteral) {
    auto tokens = lex("None");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::NONE);
}

TEST(LexerTest, LowercaseTrueAlias) {
    auto tokens = lex("true");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TRUE);
}

TEST(LexerTest, LowercaseFalseAlias) {
    auto tokens = lex("false");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::FALSE);
}

TEST(LexerTest, LowercaseNoneAlias) {
    auto tokens = lex("none");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::NONE);
}

//===----------------------------------------------------------------------===//
// Identifiers and Keywords
//===----------------------------------------------------------------------===//

TEST(LexerTest, SimpleIdentifier) {
    auto tokens = lex("hello");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "hello");
}

TEST(LexerTest, UnderscoreIdentifier) {
    auto tokens = lex("_private");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
}

TEST(LexerTest, IdentifierWithDigits) {
    auto tokens = lex("var123");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
}

TEST(LexerTest, AllKeywords) {
    struct KeywordTest { const char* text; TokenType expected; };
    KeywordTest tests[] = {
        {"and", TokenType::AND}, {"as", TokenType::AS}, {"assert", TokenType::ASSERT},
        {"async", TokenType::ASYNC}, {"await", TokenType::AWAIT}, {"break", TokenType::BREAK},
        {"class", TokenType::CLASS}, {"continue", TokenType::CONTINUE}, {"def", TokenType::DEF},
        {"del", TokenType::DEL}, {"elif", TokenType::ELIF}, {"else", TokenType::ELSE},
        {"except", TokenType::EXCEPT}, {"finally", TokenType::FINALLY}, {"for", TokenType::FOR},
        {"from", TokenType::FROM}, {"global", TokenType::GLOBAL}, {"if", TokenType::IF},
        {"import", TokenType::IMPORT}, {"in", TokenType::IN}, {"is", TokenType::IS},
        {"lambda", TokenType::LAMBDA}, {"nonlocal", TokenType::NONLOCAL}, {"not", TokenType::NOT},
        {"or", TokenType::OR}, {"pass", TokenType::PASS}, {"raise", TokenType::RAISE},
        {"return", TokenType::RETURN}, {"try", TokenType::TRY}, {"while", TokenType::WHILE},
        {"with", TokenType::WITH}, {"yield", TokenType::YIELD}, {"catch", TokenType::CATCH},
    };
    for (const auto& test : tests) {
        auto tokens = lex(test.text);
        ASSERT_GE(tokens.size(), 2u) << "Failed for keyword: " << test.text;
        EXPECT_EQ(tokens[0].type(), test.expected) << "Failed for keyword: " << test.text;
    }
}

//===----------------------------------------------------------------------===//
// Single-Character Delimiters
//===----------------------------------------------------------------------===//

TEST(LexerTest, AllSingleCharDelimiters) {
    auto tokens = lex("( ) [ ] { } , : ; .");
    ASSERT_GE(tokens.size(), 11u);
    EXPECT_EQ(tokens[0].type(), TokenType::LEFT_PAREN);
    EXPECT_EQ(tokens[1].type(), TokenType::RIGHT_PAREN);
    EXPECT_EQ(tokens[2].type(), TokenType::LEFT_BRACKET);
    EXPECT_EQ(tokens[3].type(), TokenType::RIGHT_BRACKET);
    EXPECT_EQ(tokens[4].type(), TokenType::LEFT_BRACE);
    EXPECT_EQ(tokens[5].type(), TokenType::RIGHT_BRACE);
    EXPECT_EQ(tokens[6].type(), TokenType::COMMA);
    EXPECT_EQ(tokens[7].type(), TokenType::COLON);
    EXPECT_EQ(tokens[8].type(), TokenType::SEMICOLON);
    EXPECT_EQ(tokens[9].type(), TokenType::DOT);
}

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//

TEST(LexerTest, ArithmeticOperators) {
    auto tokens = lex("+ - * / %");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type(), TokenType::PLUS);
    EXPECT_EQ(tokens[1].type(), TokenType::MINUS);
    EXPECT_EQ(tokens[2].type(), TokenType::STAR);
    EXPECT_EQ(tokens[3].type(), TokenType::SLASH);
    EXPECT_EQ(tokens[4].type(), TokenType::PERCENT);
}

TEST(LexerTest, BitwiseOperators) {
    auto tokens = lex("& | ^ ~");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type(), TokenType::AMPERSAND);
    EXPECT_EQ(tokens[1].type(), TokenType::PIPE);
    EXPECT_EQ(tokens[2].type(), TokenType::CARET);
    EXPECT_EQ(tokens[3].type(), TokenType::TILDE);
}

TEST(LexerTest, ComparisonOperators) {
    auto tokens = lex("== != < > <= >=");
    ASSERT_GE(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].type(), TokenType::EQUAL_EQUAL);
    EXPECT_EQ(tokens[1].type(), TokenType::NOT_EQUAL);
    EXPECT_EQ(tokens[2].type(), TokenType::LESS);
    EXPECT_EQ(tokens[3].type(), TokenType::GREATER);
    EXPECT_EQ(tokens[4].type(), TokenType::LESS_EQUAL);
    EXPECT_EQ(tokens[5].type(), TokenType::GREATER_EQUAL);
}

TEST(LexerTest, AssignmentOperators) {
    auto tokens = lex("= += -= *= /= %= **= //=");
    ASSERT_GE(tokens.size(), 9u);
    EXPECT_EQ(tokens[0].type(), TokenType::EQUAL);
    EXPECT_EQ(tokens[1].type(), TokenType::PLUS_EQUAL);
    EXPECT_EQ(tokens[2].type(), TokenType::MINUS_EQUAL);
    EXPECT_EQ(tokens[3].type(), TokenType::STAR_EQUAL);
    EXPECT_EQ(tokens[4].type(), TokenType::SLASH_EQUAL);
    EXPECT_EQ(tokens[5].type(), TokenType::PERCENT_EQUAL);
    EXPECT_EQ(tokens[6].type(), TokenType::POWER_EQUAL);
    EXPECT_EQ(tokens[7].type(), TokenType::DOUBLE_SLASH_EQUAL);
}

TEST(LexerTest, BitwiseAssignmentOperators) {
    auto tokens = lex("&= |= ^= <<= >>=");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type(), TokenType::AMPERSAND_EQUAL);
    EXPECT_EQ(tokens[1].type(), TokenType::PIPE_EQUAL);
    EXPECT_EQ(tokens[2].type(), TokenType::CARET_EQUAL);
    EXPECT_EQ(tokens[3].type(), TokenType::LEFT_SHIFT_EQUAL);
    EXPECT_EQ(tokens[4].type(), TokenType::RIGHT_SHIFT_EQUAL);
}

TEST(LexerTest, SpecialOperators) {
    auto tokens = lex("** // << >> -> ... := @=");
    ASSERT_GE(tokens.size(), 9u);
    EXPECT_EQ(tokens[0].type(), TokenType::POWER);
    EXPECT_EQ(tokens[1].type(), TokenType::DOUBLE_SLASH);
    EXPECT_EQ(tokens[2].type(), TokenType::LEFT_SHIFT);
    EXPECT_EQ(tokens[3].type(), TokenType::RIGHT_SHIFT);
    EXPECT_EQ(tokens[4].type(), TokenType::ARROW);
    EXPECT_EQ(tokens[5].type(), TokenType::ELLIPSIS);
    EXPECT_EQ(tokens[6].type(), TokenType::WALRUS);
    EXPECT_EQ(tokens[7].type(), TokenType::AT_EQUAL);
}

//===----------------------------------------------------------------------===//
// Multi-Token Sequences
//===----------------------------------------------------------------------===//

TEST(LexerTest, VariableDeclaration) {
    auto tokens = lex("x: int = 42");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "x");
    EXPECT_EQ(tokens[1].type(), TokenType::COLON);
    EXPECT_EQ(tokens[2].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].lexeme(), "int");
    EXPECT_EQ(tokens[3].type(), TokenType::EQUAL);
    EXPECT_EQ(tokens[4].type(), TokenType::INTEGER);
}

TEST(LexerTest, FunctionSignature) {
    auto tokens = lex("def greet(name: str) -> str {");
    ASSERT_GE(tokens.size(), 10u);
    EXPECT_EQ(tokens[0].type(), TokenType::DEF);
    EXPECT_EQ(tokens[1].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme(), "greet");
    EXPECT_EQ(tokens[2].type(), TokenType::LEFT_PAREN);
    EXPECT_EQ(tokens[3].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[4].type(), TokenType::COLON);
    EXPECT_EQ(tokens[5].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[6].type(), TokenType::RIGHT_PAREN);
    EXPECT_EQ(tokens[7].type(), TokenType::ARROW);
    EXPECT_EQ(tokens[8].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[9].type(), TokenType::LEFT_BRACE);
}

TEST(LexerTest, ArithmeticExpression) {
    auto tokens = lex("1 + 2 * 3");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[1].type(), TokenType::PLUS);
    EXPECT_EQ(tokens[2].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[3].type(), TokenType::STAR);
    EXPECT_EQ(tokens[4].type(), TokenType::INTEGER);
}

TEST(LexerTest, FunctionCall) {
    auto tokens = lex("print(\"hello\")");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "print");
    EXPECT_EQ(tokens[1].type(), TokenType::LEFT_PAREN);
    EXPECT_EQ(tokens[2].type(), TokenType::STRING);
    EXPECT_EQ(tokens[3].type(), TokenType::RIGHT_PAREN);
}

TEST(LexerTest, ListLiteral) {
    auto tokens = lex("[1, 2, 3]");
    ASSERT_GE(tokens.size(), 8u);
    EXPECT_EQ(tokens[0].type(), TokenType::LEFT_BRACKET);
    EXPECT_EQ(tokens[1].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[2].type(), TokenType::COMMA);
    EXPECT_EQ(tokens[3].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[4].type(), TokenType::COMMA);
    EXPECT_EQ(tokens[5].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[6].type(), TokenType::RIGHT_BRACKET);
}

TEST(LexerTest, GenericType) {
    auto tokens = lex("list[int]");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "list");
    EXPECT_EQ(tokens[1].type(), TokenType::LEFT_BRACKET);
    EXPECT_EQ(tokens[2].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[3].type(), TokenType::RIGHT_BRACKET);
}

TEST(LexerTest, UnionType) {
    auto tokens = lex("int | str");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type(), TokenType::PIPE);
    EXPECT_EQ(tokens[2].type(), TokenType::IDENTIFIER);
}

TEST(LexerTest, DecoratorSyntax) {
    auto tokens = lex("@authenticated");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type(), TokenType::AT);
    EXPECT_EQ(tokens[1].type(), TokenType::IDENTIFIER);
}

TEST(LexerTest, MultilineInBraceMode) {
    // In brace mode, newlines are emitted as NEWLINE tokens (statement separators)
    auto tokens = lex("x = 1\ny = 2");
    ASSERT_GE(tokens.size(), 8u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type(), TokenType::EQUAL);
    EXPECT_EQ(tokens[2].type(), TokenType::INTEGER);
    EXPECT_EQ(tokens[3].type(), TokenType::NEWLINE);
    EXPECT_EQ(tokens[4].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[5].type(), TokenType::EQUAL);
    EXPECT_EQ(tokens[6].type(), TokenType::INTEGER);
}

//===----------------------------------------------------------------------===//
// Token Utilities
//===----------------------------------------------------------------------===//

TEST(TokenTest, TokenTypeNames) {
    EXPECT_STREQ(Token::tokenTypeName(TokenType::INTEGER), "INTEGER");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::FLOAT), "FLOAT");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::STRING), "STRING");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::IDENTIFIER), "IDENTIFIER");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::DEF), "DEF");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::PLUS), "PLUS");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::EQUAL_EQUAL), "EQUAL_EQUAL");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::LEFT_PAREN), "LEFT_PAREN");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::END_OF_FILE), "EOF");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::ARROW), "ARROW");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::ELLIPSIS), "ELLIPSIS");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::WALRUS), "WALRUS");
    EXPECT_STREQ(Token::tokenTypeName(TokenType::POWER), "POWER");
}

TEST(TokenTest, TokenToString) {
    auto tokens = lex("42");
    ASSERT_GE(tokens.size(), 2u);
    std::string str = tokens[0].toString();
    EXPECT_FALSE(str.empty());
    EXPECT_NE(str.find("42"), std::string::npos);
}

TEST(TokenTest, IsKeyword) {
    EXPECT_TRUE(isKeyword("def"));
    EXPECT_TRUE(isKeyword("class"));
    EXPECT_TRUE(isKeyword("if"));
    EXPECT_TRUE(isKeyword("catch"));
    EXPECT_FALSE(isKeyword("foo"));
    EXPECT_FALSE(isKeyword(""));
}

//===----------------------------------------------------------------------===//
// Error Cases
//===----------------------------------------------------------------------===//

TEST(LexerTest, UnexpectedCharacter) {
    auto diags = lexErrors("`");
    EXPECT_FALSE(diags.empty());
}

TEST(LexerTest, BangAloneIsError) {
    auto diags = lexErrors("! ");
    EXPECT_FALSE(diags.empty());
}

TEST(LexerTest, BangEqualIsValid) {
    auto tokens = lex("!=");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::NOT_EQUAL);
    auto diags = lexErrors("!=");
    EXPECT_TRUE(diags.empty());
}

TEST(LexerTest, KeywordNotIdentifier) {
    auto tokens = lex("if");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::IF);
}

TEST(LexerTest, IdentifierStartingWithKeyword) {
    auto tokens = lex("iffy");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "iffy");
}

//===----------------------------------------------------------------------===//
// Template Block Tests
//===----------------------------------------------------------------------===//

TEST(LexerTest, TemplateSimple) {
    auto tokens = lex("template {hello world}");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TEMPLATE);
    EXPECT_EQ(tokens[0].lexeme(), "hello world");
}

TEST(LexerTest, TemplateWithInterpolation) {
    auto tokens = lex("template {hello !{name}}");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TEMPLATE);
    EXPECT_EQ(tokens[0].lexeme(), "hello !{name}");
}

TEST(LexerTest, TemplateBalancedBraces) {
    auto tokens = lex("template {{\"key\": \"val\"}}");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TEMPLATE);
    EXPECT_EQ(tokens[0].lexeme(), "{\"key\": \"val\"}");
}

TEST(LexerTest, TemplateMultiline) {
    auto tokens = lex("template {\nline1\nline2\n}");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TEMPLATE);
    EXPECT_EQ(tokens[0].lexeme(), "\nline1\nline2\n");
}

TEST(LexerTest, TemplateNotKeywordAlone) {
    // "template" without { is just an identifier
    auto tokens = lex("template = 5");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "template");
}

TEST(LexerTest, TemplateUnterminated) {
    auto diags = lexErrors("template {unterminated");
    EXPECT_FALSE(diags.empty());
}

TEST(LexerTest, TypedTemplateSimple) {
    auto tokens = lex("template[HTML] {<h1>hi</h1>}");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TEMPLATE);
    // Lexeme encodes "HTML\0<h1>hi</h1>"
    auto lex = tokens[0].lexeme();
    auto nul = lex.find('\0');
    ASSERT_NE(nul, std::string::npos);
    EXPECT_EQ(lex.substr(0, nul), "HTML");
    EXPECT_EQ(lex.substr(nul + 1), "<h1>hi</h1>");
}

TEST(LexerTest, TypedTemplateCustomType) {
    auto tokens = lex("template[SQL] {SELECT * FROM users}");
    ASSERT_GE(tokens.size(), 2u);
    auto lex = tokens[0].lexeme();
    auto nul = lex.find('\0');
    ASSERT_NE(nul, std::string::npos);
    EXPECT_EQ(lex.substr(0, nul), "SQL");
    EXPECT_EQ(lex.substr(nul + 1), "SELECT * FROM users");
}

TEST(LexerTest, TypedTemplateFileForm) {
    // template[HTML]("file.html") - Lexer emits TEMPLATE with "HTML\0"
    // followed by LEFT_PAREN (which the Parser handles)
    auto tokens = lex("template[HTML](\"file.html\")");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::TEMPLATE);
    auto lex = tokens[0].lexeme();
    auto nul = lex.find('\0');
    ASSERT_NE(nul, std::string::npos);
    EXPECT_EQ(lex.substr(0, nul), "HTML");
    EXPECT_EQ(lex.substr(nul + 1), "");
    // Next tokens should be ( "file.html" )
    EXPECT_EQ(tokens[1].type(), TokenType::LEFT_PAREN);
}

TEST(LexerTest, TypedTemplateUnterminatedBracket) {
    auto diags = lexErrors("template[HTML {body}");
    EXPECT_FALSE(diags.empty());
}
