#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenFunctionsTest.md", block);
}

TEST(CodeGenTest, FunctionDecl) {
    auto ir = generateIR("def add(a: int, b: int) -> int {\n  return a + b\n}");
    EXPECT_NE(ir.find("define internal i64 @add(i64"), std::string::npos);
    EXPECT_NE(ir.find("ret i64"), std::string::npos);
}

TEST(CodeGenTest, FunctionCall) {
    auto ir = generateIR(code("function_call"));
    EXPECT_NE(ir.find("define internal i64 @double_"), std::string::npos);
    EXPECT_NE(ir.find("call i64 @double_"), std::string::npos);
}

TEST(CodeGenTest, VoidFunction) {
    auto ir = generateIR("def greet() -> None {\n  print(\"hi\")\n}");
    EXPECT_NE(ir.find("define internal void @greet()"), std::string::npos);
}

TEST(CodeGenTest, ForwardDeclaration) {
    auto ir = generateIR(code("forward_declaration"));
    EXPECT_NE(ir.find("define internal i64 @foo()"), std::string::npos);
    EXPECT_NE(ir.find("define internal i64 @bar()"), std::string::npos);
}

TEST(CodeGenTest, LambdaSimple) {
    auto ir = generateIR(code("lambda_simple"));
    EXPECT_NE(ir.find("__dragon_lambda_"), std::string::npos);
}

TEST(CodeGenIR, TypeAliasNoOp) {
    auto ir = generateIR(code("type_alias_no_op"));
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, PyTypeAliasIR) {
    auto ir = generateIRPy(code("py_type_alias_ir"));
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, IndirectCallIR) {
    auto ir = generateIR(code("indirect_call_ir"));
    EXPECT_NE(ir.find("f.load"), std::string::npos)
        << "Expected f.load for indirect call\nIR:\n" << ir;
}

TEST(CodeGenE2E, FunctionAndLoop) {
    auto output = compileAndRun(code("function_and_loop"));
    EXPECT_EQ(output, "55\n0\n1\n2\n");
}

TEST(CodeGenE2E, RecursivePower) {
    auto output = compileAndRun(code("recursive_power"));
    EXPECT_EQ(output, "1024\n");
}

TEST(CodeGenE2E, MultipleReturns) {
    auto output = compileAndRun(code("multiple_returns"));
    EXPECT_EQ(output, "negative\nzero\npositive\n");
}

TEST(CodeGenE2E, PositionalOnlyParamFunc) {
    auto output = compileAndRun(code("positional_only_param_func"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, KeywordOnlyParamFunc) {
    auto output = compileAndRun(code("keyword_only_param_func"));
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, MixedParamSeparators) {
    auto output = compileAndRun(code("mixed_param_separators"));
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, LambdaAssignedToVariableThenCalled) {
    auto output = compileAndRun(code("lambda_assigned_to_variable_then_called"));
    EXPECT_EQ(output, "16\n");
}

TEST(CodeGenE2E, NamedFunctionAssignedToVariableThenCalled) {
    auto output = compileAndRun(code("named_function_assigned_to_variable_then_called"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, FunctionPassedAsPtrParameter) {
    auto output = compileAndRun(code("function_passed_as_ptr_parameter"));
    EXPECT_EQ(output, "21\n");
}

TEST(CodeGenE2E, LambdaPassedDirectlyAsArgument) {
    auto output = compileAndRun(code("lambda_passed_directly_as_argument"));
    EXPECT_EQ(output, "15\n");
}

TEST(CodeGenE2E, FirstClassFuncMultipleArgs) {
    auto output = compileAndRun(code("first_class_func_multiple_args"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, HigherOrderFunctionReturningFunction) {
    auto output = compileAndRun(code("higher_order_function_returning_function"));
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, FirstClassFuncExistingCallsStillWork) {
    auto output = compileAndRun(code("first_class_func_existing_calls_still_work"));
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, LambdaAssignedWithAnnotation) {
    auto output = compileAndRun(code("lambda_assigned_with_annotation"));
    EXPECT_EQ(output, "36\n");
}

TEST(CodeGenTest, ClosureCaptureInt) {
    auto output = compileAndRun(code("closure_capture_int"));
    EXPECT_EQ(output, "15\n30\n");
}

TEST(CodeGenTest, ClosureCaptureStr) {
    auto output = compileAndRun(code("closure_capture_str"));
    EXPECT_EQ(output, "Hello, World\nHi, Dragon\n");
}

TEST(CodeGenTest, ClosureMultipleCaptures) {
    auto output = compileAndRun(code("closure_multiple_captures"));
    EXPECT_EQ(output, "Result: 21\nResult: 30\n");
}

TEST(CodeGenTest, ClosureByValueSemantics) {
    auto output = compileAndRun(code("closure_by_value_semantics"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenTest, NonCapturingLambdaUnchanged) {
    auto output = compileAndRun(code("non_capturing_lambda_unchanged"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenTest, ClosureCaptureBool) {
    auto output = compileAndRun(code("closure_capture_bool"));
    EXPECT_EQ(output, "yes: 42\nno: 42\n");
}

TEST(CodeGenTest, ClosureCaptureFloat) {
    auto output = compileAndRun(code("closure_capture_float"));
    EXPECT_EQ(output, "7.0\n");
}

TEST(CodeGenTest, ClosureReturnedFromFunction) {
    auto output = compileAndRun(code("closure_returned_from_function"));
    EXPECT_EQ(output, "105\n142\n");
}

TEST(CodeGenTest, NestedDefWithCapture) {
    auto output = compileAndRun(code("nested_def_with_capture"));
    EXPECT_EQ(output, "15\n");
}

TEST(CodeGenTest, NestedDefWithoutCapture) {
    auto output = compileAndRun(code("nested_def_without_capture"));
    EXPECT_EQ(output, "14\n");
}

TEST(CodeGenTest, NestedDefSiblings) {
    auto output = compileAndRun(code("nested_def_siblings"));
    EXPECT_EQ(output, "36\n");
}

TEST(CodeGenTest, NestedDefRecursive) {
    auto output = compileAndRun(code("nested_def_recursive"));
    EXPECT_EQ(output, "120\n");
}

TEST(CodeGenTest, NestedDefRecursiveWithCapture) {
    auto output = compileAndRun(code("nested_def_recursive_with_capture"));
    EXPECT_EQ(output, "35\n");
}

TEST(CodeGenTest, NestedDefStrCapture) {
    auto output = compileAndRun(code("nested_def_str_capture"));
    EXPECT_EQ(output, "hello!\n");
}

TEST(CodeGenTest, NestedDefDoesNotLeakIntoModule) {
    auto output = compileAndRun(code("nested_def_does_not_leak_into_module"));
    EXPECT_EQ(output, "49\n150\n");
}

TEST(CodeGenTest, GeneratorStoredInVarIR) {
    auto ir = generateIR(code("generator_stored_in_var_ir"));
    EXPECT_NE(ir.find("dragon_generator_advance"), std::string::npos) << "Missing generator_advance in IR:\n" << ir;
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos) << "Missing print_int in IR:\n" << ir;
}

TEST(CodeGenTest, GeneratorIR) {
    auto ir = generateIR(code("generator_ir"));
    EXPECT_NE(ir.find("dragon_generator_create"), std::string::npos);
    EXPECT_NE(ir.find("dragon_generator_advance"), std::string::npos);
    EXPECT_NE(ir.find("dragon_generator_yield_value"), std::string::npos);
    EXPECT_NE(ir.find("gen__gen_body"), std::string::npos);
    EXPECT_NE(ir.find("llvm.coro.suspend"), std::string::npos);
}

TEST(CodeGenE2E, GeneratorBasic) {
    auto output = compileAndRun(code("generator_basic"));
    EXPECT_EQ(output, "0\n1\n2\n3\n4\n");
}

TEST(CodeGenE2E, GeneratorFibonacci) {
    auto output = compileAndRun(code("generator_fibonacci"));
    EXPECT_EQ(output, "0\n1\n1\n2\n3\n5\n8\n");
}

TEST(CodeGenE2E, GeneratorNoArgs) {
    auto output = compileAndRun(code("generator_no_args"));
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, GeneratorEmpty) {
    auto output = compileAndRun(code("generator_empty"));
    EXPECT_EQ(output, "done\n");
}

TEST(CodeGenE2E, GeneratorStoredInVar) {
    auto output = compileAndRun(code("generator_stored_in_var"));
    EXPECT_EQ(output, "1\n2\n3\n");
}

TEST(CodeGenTest, BasicFunctionDecorator) {
    auto output = compileAndRun(code("basic_function_decorator"));
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenIR, VarArgsIR) {
    auto ir = generateIR(code("var_args_ir"));
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
}

TEST(CodeGenE2E, VarArgsEmpty) {
    auto out = compileAndRun(code("var_args_empty"));
    EXPECT_EQ(out, "test\n0\n");
}

TEST(CodeGenE2E, VarArgsLen) {
    auto out = compileAndRun(code("var_args_len"));
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, VarArgsMixed) {
    auto out = compileAndRun(code("var_args_mixed"));
    EXPECT_EQ(out, "hi\n2\n");
}

TEST(CodeGenE2E, KwargsEmpty) {
    auto out = compileAndRun(code("kwargs_empty"));
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenE2E, KwargsLen) {
    auto out = compileAndRun(code("kwargs_len"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, VarArgsAndKwargs) {
    auto out = compileAndRun(code("var_args_and_kwargs"));
    EXPECT_EQ(out, "1\n2\n2\n");
}

TEST(CodeGenE2E, VarArgsOnlyRegular) {
    auto out = compileAndRun(code("var_args_only_regular"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, VarArgsTypedFloat) {
    auto out = compileAndRun(code("var_args_typed_float"));
    EXPECT_EQ(out, "4.0\n");
}

TEST(CodeGenE2E, VarArgsTypedStr) {
    auto out = compileAndRun(code("var_args_typed_str"));
    EXPECT_EQ(out, "x\ny\nz\n");
}

TEST(CodeGenE2E, VarArgsTypedListElem) {
    auto out = compileAndRun(code("var_args_typed_list_elem"));
    EXPECT_EQ(out, "10\n30\n");
}

TEST(CodeGenE2E, VarArgsUnionElem) {
    auto out = compileAndRun(code("var_args_union_elem"));
    EXPECT_EQ(out, "[1, 2]\nhi\n");
}

TEST(CodeGenIR, GeneratorLoopBranchesOnExhaustion) {
    auto ir = generateIR(code("generator_reraise_emits_scope_cleanup"));
    EXPECT_NE(ir.find("dragon_generator_advance"), std::string::npos);
    EXPECT_EQ(ir.find("gen.reraise"), std::string::npos)
        << "The loop must not arm a frame per element to re-raise\nIR:\n" << ir;
    EXPECT_EQ(ir.find("StopIteration"), std::string::npos)
        << "Exhaustion is a returned value, never a synthesized raise\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Expected scope cleanup (decref_str) in caller\nIR:\n" << ir;
}

TEST(CodeGenE2E, GeneratorReraiseDoesntLeakOuterLocals) {
    auto out = compileAndRun(code("generator_reraise_doesnt_leak_outer_locals"));
    EXPECT_EQ(out, "2000\n");
}

TEST(CodeGenE2E, GeneratorReraiseExceptionPropagates) {
    auto out = compileAndRun(code("generator_reraise_exception_propagates"));
    EXPECT_EQ(out, "1\n1\n");
}

TEST(CodeGenIR, GeneratorWrapperBuildsCoroutineFrame) {
    auto ir = generateIR(code("generator_wrapper_uses_typed_create"));
    EXPECT_NE(ir.find("dragon_generator_create"), std::string::npos)
        << "Expected dragon_generator_create call\nIR:\n" << ir;
    EXPECT_NE(ir.find("echo__gen_body"), std::string::npos);
    EXPECT_NE(ir.find("llvm.coro.begin"), std::string::npos)
        << "Expected the body to open an LLVM coroutine frame\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_coro_resume"), std::string::npos)
        << "Expected the shared resume thunk\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_coro_destroy"), std::string::npos)
        << "Expected the shared destroy thunk\nIR:\n" << ir;
    EXPECT_EQ(ir.find("__dragon_gen_tramp_"), std::string::npos)
        << "A generator must not spawn a stack-switching trampoline\nIR:\n" << ir;
}

TEST(CodeGenIR, GeneratorCreateDeclaration) {
    auto ir = generateIR(code("generator_typed_create_declaration"));
    EXPECT_NE(ir.find("dragon_generator_create(ptr, ptr)"), std::string::npos)
        << "Expected the two-thunk signature for dragon_generator_create\n"
        << "IR:\n" << ir;
}

TEST(CodeGenE2E, GeneratorWithStringArgRunsCorrectly) {
    auto out = compileAndRun(code("generator_with_string_arg_runs_correctly"));
    EXPECT_EQ(out, "hello\nhello!\n");
}

TEST(CodeGenE2E, GeneratorYieldsStringRoundTrips) {
    auto out = compileAndRun(code("generator_yields_string_round_trips"));
    EXPECT_EQ(out, "first\nsecond\nthird\n");
}

TEST(CodeGenE2E, GeneratorAbandonedNoLeak) {
    auto out = compileAndRun(code("generator_abandoned_no_leak"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, GeneratorMultipleHeapArgsBalance) {
    auto out = compileAndRun(code("generator_multiple_heap_args_balance"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, GeneratorIntArgStillWorks) {
    auto out = compileAndRun(code("generator_int_arg_still_works"));
    EXPECT_EQ(out, "4950\n");
}

TEST(CodeGenE2E, FileReadFromPipe) {
    auto out = compileAndRun(code("file_read_from_pipe"));
    EXPECT_EQ(out, "abc\ndef\nghi\n\n");
}

TEST(CodeGenE2E, FileReadShellPipe) {
    auto out = compileAndRun(code("file_read_shell_pipe"));
    EXPECT_EQ(out, "line1\nline2\nline3\n\n");
}

TEST(CodeGenE2E, FileReadShellPipeLargeOutput) {
    auto out = compileAndRun(code("file_read_shell_pipe_large_output"));
    EXPECT_EQ(out, "20000\n");
}

TEST(CodeGenE2E, FileReadShellPipeEmpty) {
    auto out = compileAndRun(code("file_read_shell_pipe_empty"));
    EXPECT_EQ(out, "0\n");
}
