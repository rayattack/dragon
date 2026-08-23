#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenLiteralsTest.md", block);
}

TEST(CodeGenTest, IntegerLiteral) {
    auto ir = generateIR("print(42)");
    EXPECT_NE(ir.find("42"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenTest, FloatLiteral) {
    auto ir = generateIR("3.14");
    EXPECT_NE(ir.find("double"), std::string::npos);
}

TEST(CodeGenTest, StringLiteral) {
    auto ir = generateIR("\"hello\"");
    EXPECT_NE(ir.find("hello"), std::string::npos);
}

TEST(CodeGenTest, BoolLiteral) {
    auto ir = generateIR("True");
    EXPECT_NE(ir.find("define i32 @main("), std::string::npos);
    EXPECT_EQ(ir.find("<codegen failed"), std::string::npos);
}

TEST(CodeGenTest, StringIndex) {
    auto ir = generateIR("s: str = \"hello\"\nprint(s[0])");
    EXPECT_NE(ir.find("dragon_str_index"), std::string::npos);
}

TEST(CodeGenTest, StringSlice) {
    auto ir = generateIR("s: str = \"hello world\"\nt: str = s[0:5]");
    EXPECT_NE(ir.find("dragon_str_slice"), std::string::npos);
}

TEST(CodeGenTest, StringUpper) {
    auto ir = generateIR("s: str = \"hello\"\nt: str = s.upper()");
    EXPECT_NE(ir.find("dragon_str_upper"), std::string::npos);
}

TEST(CodeGenTest, StringFind) {
    auto ir = generateIR("s: str = \"hello world\"\nx: int = s.find(\"world\")");
    EXPECT_NE(ir.find("dragon_str_find"), std::string::npos);
}

TEST(CodeGenTest, StringReplace) {
    auto ir = generateIR("s: str = \"hello\"\nt: str = s.replace(\"l\", \"r\")");
    EXPECT_NE(ir.find("dragon_str_replace"), std::string::npos);
}

TEST(CodeGenTest, StringStartswith) {
    auto ir = generateIR("s: str = \"hello\"\nx: int = s.startswith(\"he\")");
    EXPECT_NE(ir.find("dragon_str_startswith"), std::string::npos);
}

TEST(CodeGenTest, StringIsDigit) {
    auto ir = generateIR("s: str = \"123\"\nx: int = s.isdigit()");
    EXPECT_NE(ir.find("dragon_str_isdigit"), std::string::npos);
}

TEST(CodeGenIR, StringCmpDeclared) {
    auto ir = generateIR(code("string_cmp_declared"));
    EXPECT_NE(ir.find("dragon_str_cmp"), std::string::npos)
        << "Expected dragon_str_cmp call for string < comparison\nIR:\n" << ir;
}

TEST(CodeGenTest, FStringSimple) {
    auto ir = generateIR("x: int = 42\nprint(f\"value is {x}\")");
    EXPECT_NE(ir.find("dragon_int_to_str"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
}

TEST(CodeGenTest, FStringFormatSpecIR) {
    auto ir = generateIR(code("f_string_format_spec_ir"));
    EXPECT_NE(ir.find("dragon_float_format"), std::string::npos);
}

TEST(CodeGenIR, FStringIntermediateDecref) {
    auto ir = generateIR(code("f_string_intermediate_decref"));
    auto count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("dragon_decref_str", pos)) != std::string::npos) {
        count++;
        pos += 17;
    }
    EXPECT_GE(count, 2)
        << "Expected decref_str for f-string intermediates\nIR:\n" << ir;
}

TEST(CodeGenIR, BinaryStringConcatChainDecref) {
    auto ir = generateIR(code("binary_string_concat_chain_decref"));
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Expected decref_str for concat chain intermediate\nIR:\n" << ir;
}

TEST(CodeGenE2E, FStringArithmetic) {
    auto output = compileAndRun(code("f_string_arithmetic"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, FStringFunctionCall) {
    auto output = compileAndRun(code("f_string_function_call"));
    EXPECT_EQ(output, "len=3\n");
}

TEST(CodeGenE2E, FStringMultipleExprs) {
    auto output = compileAndRun(code("f_string_multiple_exprs"));
    EXPECT_EQ(output, "10 + 20 = 30\n");
}

TEST(CodeGenE2E, FStringFloatFormat) {
    auto output = compileAndRun(code("f_string_float_format"));
    EXPECT_EQ(output, "3.14\n");
}

TEST(CodeGenE2E, FStringFloatFormat3) {
    auto output = compileAndRun(code("f_string_float_format3"));
    EXPECT_EQ(output, "3.1416\n");
}

TEST(CodeGenE2E, FStringIntHex) {
    auto output = compileAndRun(code("f_string_int_hex"));
    EXPECT_EQ(output, "ff\n");
}

TEST(CodeGenE2E, FStringIntHexUpper) {
    auto output = compileAndRun(code("f_string_int_hex_upper"));
    EXPECT_EQ(output, "FF\n");
}

TEST(CodeGenE2E, FStringIntOctal) {
    auto output = compileAndRun(code("f_string_int_octal"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, FStringIntBinary) {
    auto output = compileAndRun(code("f_string_int_binary"));
    EXPECT_EQ(output, "1010\n");
}

TEST(CodeGenE2E, FStringIntZeroPad) {
    auto output = compileAndRun(code("f_string_int_zero_pad"));
    EXPECT_EQ(output, "00042\n");
}

TEST(CodeGenE2E, FStringMixed) {
    auto output = compileAndRun(code("f_string_mixed"));
    EXPECT_EQ(output, "pi = 3.14\n");
}

TEST(CodeGenE2E, FStringMultiInterpolation) {
    auto output = compileAndRun(code("f_string_multi_interpolation"));
    EXPECT_EQ(output, "1 + 2 = 3\n");
}

TEST(CodeGenE2E, FStringRejectsPercentN) {
    auto output = compileAndRun(code("f_string_rejects_percent_n"));
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringRejectsPercentS) {
    auto output = compileAndRun(code("f_string_rejects_percent_s"));
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringLongAllZeroSpec) {
    auto output = compileAndRun(code("f_string_long_all_zero_spec"));
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, FStringRejectsHugeWidth) {
    auto output = compileAndRun(code("f_string_rejects_huge_width"));
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringRejectsFloatPercentN) {
    auto output = compileAndRun(code("f_string_rejects_float_percent_n"));
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringValidSpecsStillWork) {
    auto output = compileAndRun(code("f_string_valid_specs_still_work"));
    EXPECT_EQ(output, "3.14\n2a\n2A\n52\n101010\n00042\n");
}

TEST(CodeGenE2E, StringOperations) {
    auto output = compileAndRun(code("string_operations"));
    EXPECT_EQ(output, "5\nhello world\n");
}

TEST(CodeGenE2E, StringIndexing) {
    auto output = compileAndRun(code("string_indexing"));
    EXPECT_EQ(output, "D\nn\n");
}

TEST(CodeGenE2E, StringMethods) {
    auto output = compileAndRun(code("string_methods"));
    EXPECT_EQ(output, "HELLO\n2\n");
}

TEST(CodeGenE2E, StringSlice) {
    auto output = compileAndRun(code("string_slice"));
    EXPECT_EQ(output, "hello\nworld\n");
}

TEST(CodeGenE2E, ExprStmtStringMethodNoLeak) {
    auto output = compileAndRun(code("expr_stmt_string_method_no_leak"));
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, StringConcatChainE2E) {
    auto output = compileAndRun(code("string_concat_chain_e2_e"));
    EXPECT_EQ(output, "hello world\n");
}

TEST(CodeGenE2E, StringOrdering) {
    auto output = compileAndRun(code("string_ordering"));
    EXPECT_EQ(output, "True\nTrue\nTrue\nTrue\n");
}

TEST(CodeGenE2E, StringOrderingVariables) {
    auto output = compileAndRun(code("string_ordering_variables"));
    EXPECT_EQ(output, "in range\n");
}

TEST(CodeGenE2E, ListStrSubscript) {
    auto output = compileAndRun(code("list_str_subscript"));
    EXPECT_EQ(output, "alice\nbob\n");
}

TEST(CodeGenE2E, ListFloatSubscript) {
    auto output = compileAndRun(code("list_float_subscript"));
    EXPECT_EQ(output, "3.8\n");
}

TEST(CodeGenTest, BytesLiteralIR) {
    auto ir = generateIR("b: bytes = b\"hello\"\n");
    EXPECT_NE(ir.find("dragon_bytes_from_literal"), std::string::npos);
}

TEST(CodeGenTest, BytesConcatIR) {
    auto ir = generateIR(code("bytes_concat_ir"));
    EXPECT_NE(ir.find("dragon_bytes_concat"), std::string::npos);
}

TEST(CodeGenTest, BytesLenIR) {
    auto ir = generateIR(code("bytes_len_ir"));
    EXPECT_NE(ir.find("dragon_bytes_len"), std::string::npos);
}

TEST(CodeGenTest, BytesDecodeIR) {
    auto ir = generateIR(code("bytes_decode_ir"));
    EXPECT_NE(ir.find("dragon_bytes_decode"), std::string::npos);
}

TEST(CodeGenTest, StrEncodeIR) {
    auto ir = generateIR(code("str_encode_ir"));
    EXPECT_NE(ir.find("dragon_str_encode"), std::string::npos);
}

TEST(CodeGenE2E, BytesLiteralPrint) {
    auto output = compileAndRun("print(b\"hello\")\n");
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesLen) {
    auto output = compileAndRun(code("bytes_len"));
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, BytesIndex) {
    auto output = compileAndRun(code("bytes_index"));
    EXPECT_EQ(output, "104\n");
}

TEST(CodeGenE2E, BytesNegIndex) {
    auto output = compileAndRun(code("bytes_neg_index"));
    EXPECT_EQ(output, "111\n");
}

TEST(CodeGenE2E, BytesSlice) {
    auto output = compileAndRun(code("bytes_slice"));
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesConcat) {
    auto output = compileAndRun(code("bytes_concat"));
    EXPECT_EQ(output, "b'hello world'\n");
}

TEST(CodeGenE2E, BytesRepeat) {
    auto output = compileAndRun(code("bytes_repeat"));
    EXPECT_EQ(output, "b'ababab'\n");
}

TEST(CodeGenE2E, BytesEq) {
    auto output = compileAndRun(code("bytes_eq"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesNeq) {
    auto output = compileAndRun(code("bytes_neq"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesLt) {
    auto output = compileAndRun(code("bytes_lt"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesContainsInt) {
    auto output = compileAndRun(code("bytes_contains_int"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesDecode) {
    auto output = compileAndRun(code("bytes_decode"));
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, StrEncode) {
    auto output = compileAndRun(code("str_encode"));
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesHex) {
    auto output = compileAndRun(code("bytes_hex"));
    EXPECT_EQ(output, "68656c6c6f\n");
}

TEST(CodeGenE2E, BytesFromhex) {
    auto output = compileAndRun(code("bytes_fromhex"));
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesFind) {
    auto output = compileAndRun(code("bytes_find"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, BytesRfind) {
    auto output = compileAndRun(code("bytes_rfind"));
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, BytesCount) {
    auto output = compileAndRun(code("bytes_count"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, BytesReplace) {
    auto output = compileAndRun(code("bytes_replace"));
    EXPECT_EQ(output, "b'herro'\n");
}

TEST(CodeGenE2E, BytesStartswith) {
    auto output = compileAndRun(code("bytes_startswith"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesUpper) {
    auto output = compileAndRun(code("bytes_upper"));
    EXPECT_EQ(output, "b'HELLO'\n");
}

TEST(CodeGenE2E, BytesLower) {
    auto output = compileAndRun(code("bytes_lower"));
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesStrip) {
    auto output = compileAndRun(code("bytes_strip"));
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesSplit) {
    auto output = compileAndRun(code("bytes_split"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, BytesIsDigit) {
    auto output = compileAndRun(
        "print(b\"123\".isdigit())\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, ConcatIntermediateUpperLowerLoop) {
    auto output = compileAndRun(code("concat_intermediate_upper_lower_loop"));
    EXPECT_EQ(output, "ABCDEabcde\n");
}

TEST(CodeGenE2E, ConcatIntermediateStrCoercionLoop) {
    auto output = compileAndRun(code("concat_intermediate_str_coercion_loop"));
    EXPECT_EQ(output, "999910000\n");
}

TEST(CodeGenE2E, ConcatIntermediateSliceLoop) {
    auto output = compileAndRun(code("concat_intermediate_slice_loop"));
    EXPECT_EQ(output, "abcdef\n");
}

TEST(CodeGenE2E, ConcatIntermediateTriple) {
    auto output = compileAndRun(code("concat_intermediate_triple"));
    EXPECT_EQ(output, "AAAbbbyCy\n");
}

TEST(CodeGenE2E, ConcatIntermediateLiteralPlusStrPlusLiteral) {
    auto output = compileAndRun(code("concat_intermediate_literal_plus_str_plus_literal"));
    EXPECT_EQ(output, "prefix-9999-suffix\n");
}

TEST(CodeGenIR, ConcatBroadDecrefUpperLower) {
    auto ir = generateIR(code("concat_broad_decref_upper_lower"));
    EXPECT_NE(ir.find("dragon_str_upper"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_lower"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
    auto count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("dragon_decref_str", pos)) != std::string::npos) {
        count++;
        pos += 17;
    }
    EXPECT_GE(count, 2)
        << "Expected at least 2 decref_str calls for upper/lower intermediates\n"
        << "IR:\n" << ir;
}

TEST(CodeGenIR, ConcatBroadDecrefIntToStr) {
    auto ir = generateIR(code("concat_broad_decref_int_to_str"));
    EXPECT_NE(ir.find("dragon_int_to_str"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
    auto count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("dragon_decref_str", pos)) != std::string::npos) {
        count++;
        pos += 17;
    }
    EXPECT_GE(count, 2)
        << "Expected at least 2 decref_str calls for int_to_str intermediates\n"
        << "IR:\n" << ir;
}

TEST(CodeGenE2E, StrJoinEmptyList) {
    auto output = compileAndRun(code("str_join_empty_list"));
    EXPECT_EQ(output, "[]\n");
}

TEST(CodeGenE2E, StrJoinSingleElement) {
    auto output = compileAndRun(code("str_join_single_element"));
    EXPECT_EQ(output, "only\n");
}

TEST(CodeGenE2E, StrJoinAllEmptyStrings) {
    auto output = compileAndRun(code("str_join_all_empty_strings"));
    EXPECT_EQ(output, "[||]\n");
}

TEST(CodeGenE2E, StrJoinMixedEmpty) {
    auto output = compileAndRun(code("str_join_mixed_empty"));
    EXPECT_EQ(output, "a,,b,,c\n");
}

TEST(CodeGenE2E, StrJoinEmptySeparator) {
    auto output = compileAndRun(code("str_join_empty_separator"));
    EXPECT_EQ(output, "foobarbaz\n");
}

TEST(CodeGenE2E, StrJoinLoopBounded) {
    auto output = compileAndRun(code("str_join_loop_bounded"));
    EXPECT_EQ(output, "alpha--beta\n");
}

TEST(CodeGenE2E, StrReplaceShrink) {
    auto output = compileAndRun(code("str_replace_shrink"));
    EXPECT_EQ(output, "yyyy\n");
}

TEST(CodeGenE2E, StrReplaceExpand) {
    auto output = compileAndRun(code("str_replace_expand"));
    EXPECT_EQ(output, "AAAbcAAAbc\n");
}

TEST(CodeGenE2E, StrReplaceEqualLength) {
    auto output = compileAndRun(code("str_replace_equal_length"));
    EXPECT_EQ(output, "hell0 w0rld\n");
}

TEST(CodeGenE2E, StrReplaceNoMatch) {
    auto output = compileAndRun(code("str_replace_no_match"));
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, StrReplaceShrinkToEmpty) {
    auto output = compileAndRun(code("str_replace_shrink_to_empty"));
    EXPECT_EQ(output, "abcd\n");
}

TEST(CodeGenE2E, StrReplaceFullString) {
    auto output = compileAndRun(code("str_replace_full_string"));
    EXPECT_EQ(output, "[]\n");
}

TEST(CodeGenE2E, Utf8LenIsCodePointCount) {
    auto output = compileAndRun(code("utf8_len_is_code_point_count"));
    EXPECT_EQ(output, "5\n4\n11\n3\n");
}

TEST(CodeGenE2E, Utf8IndexingByCodePoint) {
    auto output = compileAndRun(code("utf8_indexing_by_code_point"));
    EXPECT_EQ(output, "c\na\nf\n\xc3\xa9\n\xc3\xa9\n");
}

TEST(CodeGenE2E, Utf8IndexingMultiByteOnlyString) {
    auto output = compileAndRun(code("utf8_indexing_multi_byte_only_string"));
    EXPECT_EQ(output, "\xe6\x97\xa5\n\xe6\x9c\xac\n\xe8\xaa\x9e\n");
}

TEST(CodeGenE2E, Utf8SlicePreservesValidEncoding) {
    auto output = compileAndRun(code("utf8_slice_preserves_valid_encoding"));
    EXPECT_EQ(output, "h\xc3\xa9llo\nw\xc3\xb6rld\n\xc3\xa9l\n");
}

TEST(CodeGenE2E, Utf8ConcatMixedKind) {
    auto output = compileAndRun(code("utf8_concat_mixed_kind"));
    EXPECT_EQ(output, "hello w\xc3\xb6rld\nprefix w\xc3\xb6rld suffix\n");
}

TEST(CodeGenE2E, Utf8ConcatCanonicalDowngrade) {
    auto output = compileAndRun(code("utf8_concat_canonical_downgrade"));
    EXPECT_EQ(output, "hello\n5\ne\n");
}

TEST(CodeGenE2E, Utf8FindAndContains) {
    auto output = compileAndRun(code("utf8_find_and_contains"));
    EXPECT_EQ(output, "6\n-1\nyes\nno\n");
}

TEST(CodeGenE2E, Utf8StartswithEndswith) {
    auto output = compileAndRun(code("utf8_startswith_endswith"));
    EXPECT_EQ(output, "sp\nep\nsn\n");
}

TEST(CodeGenE2E, Utf8Replace) {
    auto output = compileAndRun(code("utf8_replace"));
    EXPECT_EQ(output, "cafe au lait\ncaf\xc3\xa9 noir\nth\xc3\xa9 au lait\n");
}

TEST(CodeGenE2E, Utf8DictKeys) {
    auto output = compileAndRun(code("utf8_dict_keys"));
    EXPECT_EQ(output, "1\n2\n3\nTrue\nFalse\n");
}

TEST(CodeGenE2E, Utf8PrintRoundTrip) {
    auto output = compileAndRun(code("utf8_print_round_trip"));
    EXPECT_EQ(output, "caf\xc3\xa9\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\n\xce\xb1\xce\xb2\xce\xb3\n");
}

TEST(CodeGenE2E, Utf8AsciiUpperLowerStillWork) {
    auto output = compileAndRun(code("utf8_ascii_upper_lower_still_work"));
    EXPECT_EQ(output, "HELLO\nhello\n");
}

TEST(CodeGenE2E, Utf8CaseMapsLatin1) {
    auto output = compileAndRun(code("utf8_case_maps_latin1"));
    EXPECT_EQ(output, "H\xc3\x89LLO W\xc3\x96RLD\n");
}

TEST(CodeGenE2E, Utf8ByteLenPubReturnsWireBytes) {
    auto output = compileAndRun(code("utf8_byte_len_pub_returns_wire_bytes"));
    EXPECT_EQ(output, "5\n9\n5\n")
        << "byte_len_pub must return UTF-8 wire bytes, not 4×cp_count for "
           "kind=4 (would yield 16 / 12 / 5 instead of 5 / 9 / 5):\n"
        << output;
}

TEST(CodeGenE2E, PrintMultiArgStrings) {
    EXPECT_EQ(compileAndRun("print(\"a\", \"b\", \"c\")\n"), "a b c\n");
}

TEST(CodeGenE2E, PrintMultiArgInts) {
    EXPECT_EQ(compileAndRun("print(1, 2, 3)\n"), "1 2 3\n");
}

TEST(CodeGenE2E, PrintMultiArgMixedTypes) {
    EXPECT_EQ(compileAndRun(
        "print(\"count:\", 5, \"ratio:\", 2.5, \"ok:\", True)\n"),
        "count: 5 ratio: 2.5 ok: True\n");
}

TEST(CodeGenE2E, PrintMultiArgVariables) {
    EXPECT_EQ(compileAndRun(code("print_multi_arg_variables")),
        "10 hi\n");
}

TEST(CodeGenE2E, PrintMultiArgWithList) {
    EXPECT_EQ(compileAndRun(code("print_multi_arg_with_list")),
        "xs: [1, 2, 3]\n");
}

TEST(CodeGenE2E, PrintSingleArgUnchanged) {
    EXPECT_EQ(compileAndRun("print(42)\n"), "42\n");
    EXPECT_EQ(compileAndRun("print(\"solo\")\n"), "solo\n");
}

TEST(CodeGenE2E, PrintEmptyThenValue) {
    EXPECT_EQ(compileAndRun("print()\nprint(\"after\")\n"), "\nafter\n");
}
