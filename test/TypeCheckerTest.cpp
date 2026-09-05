#include <gtest/gtest.h>
#include "CodeBlock.h"
#include "TestHelpers.h"
#include "dragon/Privacy.h"

using namespace dragon;
using namespace dragon::test;

static std::string code(const std::string& block) {
    return extractCode("TypeCheckerTest.md", block);
}

static bool checkOk(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    return tc.check(*module);
}

static bool checkHasErrors(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    return tc.hasErrors();
}

static std::shared_ptr<Type> getExprType(const std::string& source) {
    auto module = parse(source);
    if (!module || module->body.empty()) return nullptr;
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    for (int i = static_cast<int>(module->body.size()) - 1; i >= 0; --i) {
        if (auto* es = dynamic_cast<ExprStmt*>(module->body[i].get())) {
            return es->expr ? es->expr->type : nullptr;
        }
    }
    return nullptr;
}

TEST(TypeCheckerTest, EmptyModule) {
    EXPECT_TRUE(checkOk(""));
}

TEST(TypeCheckerTest, PassStatement) {
    EXPECT_TRUE(checkOk("pass"));
}

TEST(TypeCheckerTest, IntegerLiteral) {
    EXPECT_TRUE(checkOk("42"));
}

TEST(TypeCheckerTest, FloatLiteral) {
    EXPECT_TRUE(checkOk("3.14"));
}

TEST(TypeCheckerTest, StringLiteral) {
    EXPECT_TRUE(checkOk("\"hello\""));
}

TEST(TypeCheckerTest, BooleanLiteral) {
    EXPECT_TRUE(checkOk("True"));
}

TEST(TypeCheckerTest, NoneLiteral) {
    EXPECT_TRUE(checkOk("None"));
}

TEST(TypeCheckerTest, IntegerLiteralType) {
    auto t = getExprType("42");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
    EXPECT_EQ(t->toString(), "int");
}

TEST(TypeCheckerTest, FloatLiteralType) {
    auto t = getExprType("3.14");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
    EXPECT_EQ(t->toString(), "float");
}

TEST(TypeCheckerTest, StringLiteralType) {
    auto t = getExprType("\"hello\"");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
    EXPECT_EQ(t->toString(), "str");
}

TEST(TypeCheckerTest, BoolLiteralType) {
    auto t = getExprType("True");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, NoneLiteralType) {
    auto t = getExprType("None");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::None_);
}

TEST(TypeCheckerTest, IntPlusInt) {
    auto t = getExprType("1 + 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, IntPlusFloat) {
    auto t = getExprType("1 + 2.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, FloatPlusFloat) {
    auto t = getExprType("1.0 + 2.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, IntMinusInt) {
    auto t = getExprType("5 - 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, IntTimesInt) {
    auto t = getExprType("3 * 4");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, IntTimesFloat) {
    auto t = getExprType("3 * 4.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, TrueDivisionAlwaysFloat) {
    auto t = getExprType("10 / 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, FloorDivIntInt) {
    auto t = getExprType("10 // 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, FloorDivIntFloat) {
    auto t = getExprType("10 // 3.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, ModuloIntInt) {
    auto t = getExprType("10 % 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, PowerIntInt) {
    auto t = getExprType("2 ** 10");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, StringConcatenation) {
    auto t = getExprType("\"hello\" + \" world\"");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, StringRepetition) {
    auto t = getExprType("\"ha\" * 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, IntPlusStringError) {
    EXPECT_TRUE(checkHasErrors("1 + \"hello\""));
}

TEST(TypeCheckerTest, FunctionValueSubscriptRejected) {
    EXPECT_TRUE(checkHasErrors(code("function_value_subscript_rejected")));
}

TEST(TypeCheckerTest, FunctionValueLenRejected) {
    EXPECT_TRUE(checkHasErrors(code("function_value_len_rejected")));
}

TEST(TypeCheckerTest, FunctionValueIterationRejected) {
    EXPECT_TRUE(checkHasErrors(code("function_value_iteration_rejected")));
}

TEST(TypeCheckerTest, CalledFunctionResultStaysUsable) {
    EXPECT_FALSE(checkHasErrors(code("called_function_result_stays_usable")));
}

TEST(TypeCheckerTest, FreshListLiteralCovariantToBase) {
    EXPECT_FALSE(checkHasErrors(code("fresh_list_literal_covariant_to_base")));
}

TEST(TypeCheckerTest, NamedListStaysInvariant) {
    EXPECT_TRUE(checkHasErrors(code("named_list_stays_invariant")));
}

TEST(TypeCheckerTest, FreshListNonSubclassRejected) {
    EXPECT_TRUE(checkHasErrors(code("fresh_list_non_subclass_rejected")));
}

TEST(TypeCheckerTest, HeterogeneousListLiteralRejected) {
    EXPECT_TRUE(checkHasErrors("xs: list[int] = [1, 2, \"three\"]\n"));
}

TEST(TypeCheckerTest, HomogeneousListLiteralOk) {
    EXPECT_FALSE(checkHasErrors("xs: list[int] = [1, 2, 3]\n"));
}

TEST(TypeCheckerTest, ListLiteralIntToFloatPromotionOk) {
    EXPECT_FALSE(checkHasErrors("xs: list[float] = [1, 2.0, 3]\n"));
}

TEST(TypeCheckerTest, ListLiteralUnionAcceptsExactlyItsArms) {
    // A closed domain takes the shapes it names ...
    EXPECT_FALSE(checkHasErrors("xs: list[int | str] = [1, \"two\"]\n"));
    // ... and refuses one it does not, instead of widening to a dynamic tier.
    EXPECT_TRUE(checkHasErrors("ys: list[int | str] = [1, \"two\", 3.0]\n"));
}

TEST(TypeCheckerTest, NamedConcreteListNotAssignableToListAny) {
    EXPECT_TRUE(checkHasErrors(code("named_concrete_list_not_assignable_to_list_any")));
}

TEST(TypeCheckerTest, ConcreteListArgNotAssignableToListAnyParam) {
    EXPECT_TRUE(checkHasErrors(code("concrete_list_arg_not_assignable_to_list_any_param")));
}

TEST(TypeCheckerTest, FreshLiteralStillAssignableToListAny) {
    EXPECT_FALSE(checkHasErrors("xs: list[int | str] = [\"a\", \"b\"]\n"));
}

TEST(TypeCheckerTest, FreshLiteralArgStillPassableToListAnyParam) {
    EXPECT_FALSE(checkHasErrors(code("fresh_literal_arg_still_passable_to_list_any_param")));
}

TEST(TypeCheckerTest, ListAnyNotAssignableToConcreteList) {
    EXPECT_TRUE(checkHasErrors(code("list_any_not_assignable_to_concrete_list")));
}

TEST(TypeCheckerTest, DictValueCovarianceToUnionRejected) {
    // A monomorphized dict and a boxed-value dict have different layouts, so
    // the conversion is refused rather than silently reinterpreted.
    EXPECT_TRUE(checkHasErrors(code("dict_value_covariance_to_union_rejected")));
}

TEST(TypeCheckerTest, StrArgToIntParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("str_arg_to_int_param_rejected")));
}

TEST(TypeCheckerTest, FloatArgToIntParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("float_arg_to_int_param_rejected")));
}

TEST(TypeCheckerTest, ScalarArgToContainerParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("scalar_arg_to_container_param_rejected")));
}

TEST(TypeCheckerTest, IntArgToFloatParamOk) {
    EXPECT_FALSE(checkHasErrors(code("int_arg_to_float_param_ok")));
}

TEST(TypeCheckerTest, SubclassArgToBaseParamOk) {
    EXPECT_FALSE(checkHasErrors(code("subclass_arg_to_base_param_ok")));
}

TEST(TypeCheckerTest, FreshListLiteralArgToBaseListParamOk) {
    EXPECT_FALSE(checkHasErrors(code("fresh_list_literal_arg_to_base_list_param_ok")));
}

TEST(TypeCheckerTest, GenericCallConcreteParamMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_call_concrete_param_mismatch_rejected")));
}

TEST(TypeCheckerTest, GenericCallConcreteParamOkStillInfers) {
    EXPECT_FALSE(checkHasErrors(code("generic_call_concrete_param_ok_still_infers")));
}

TEST(TypeCheckerTest, MatchTypeTestPatternOk) {
    EXPECT_FALSE(checkHasErrors(code("match_type_test_pattern_ok")));
}

TEST(TypeCheckerTest, MatchClassPatternArityMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(code("match_class_pattern_arity_mismatch_rejected")));
}

TEST(TypeCheckerTest, MatchClassPatternUnknownTypeRejected) {
    EXPECT_TRUE(checkHasErrors(code("match_class_pattern_unknown_type_rejected")));
}

TEST(TypeCheckerTest, MatchNonExhaustiveUnionRejected) {
    EXPECT_TRUE(checkHasErrors(code("match_non_exhaustive_union_rejected")));
}

TEST(TypeCheckerTest, MatchNonExhaustiveBoolRejected) {
    EXPECT_TRUE(checkHasErrors(code("match_non_exhaustive_bool_rejected")));
}

TEST(TypeCheckerTest, MatchUnreachableAfterWildcardRejected) {
    EXPECT_TRUE(checkHasErrors(code("match_unreachable_after_wildcard_rejected")));
}

TEST(TypeCheckerTest, MatchDuplicateLiteralRejected) {
    EXPECT_TRUE(checkHasErrors(code("match_duplicate_literal_rejected")));
}

TEST(TypeCheckerTest, StringMinusStringError) {
    EXPECT_TRUE(checkHasErrors("\"a\" - \"b\""));
}

TEST(TypeCheckerTest, ComparisonReturnsBool) {
    auto t = getExprType("1 < 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, EqualityReturnsBool) {
    auto t = getExprType("1 == 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, NotEqualReturnsBool) {
    auto t = getExprType("1 != 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, GreaterEqualReturnsBool) {
    auto t = getExprType("1 >= 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, UnaryMinusInt) {
    auto t = getExprType("-42");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, UnaryMinusFloat) {
    auto t = getExprType("-3.14");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, UnaryNotReturnsBool) {
    auto t = getExprType("not True");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, UnaryMinusStringError) {
    EXPECT_TRUE(checkHasErrors("-\"hello\""));
}

TEST(TypeCheckerTest, BitwiseAndIntInt) {
    auto t = getExprType("5 & 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, BitwiseOrIntInt) {
    auto t = getExprType("5 | 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, BitwiseXorIntInt) {
    auto t = getExprType("5 ^ 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, LeftShiftIntInt) {
    auto t = getExprType("1 << 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, BitwiseStringError) {
    EXPECT_TRUE(checkHasErrors("\"a\" & \"b\""));
}

TEST(TypeCheckerTest, AnnotatedAssignmentOk) {
    EXPECT_TRUE(checkOk("x: int = 5"));
}

TEST(TypeCheckerTest, AnnotatedAssignmentTypeMismatch) {
    EXPECT_TRUE(checkHasErrors("x: int = \"hello\""));
}

TEST(TypeCheckerTest, AnnotatedAssignmentStrOk) {
    EXPECT_TRUE(checkOk("x: str = \"hello\""));
}

TEST(TypeCheckerTest, AnnotatedAssignmentBoolToInt) {
    EXPECT_TRUE(checkOk("x: int = True"));
}

TEST(TypeCheckerTest, AnnotatedAssignmentIntToFloat) {
    EXPECT_TRUE(checkOk("x: float = 5"));
}

TEST(TypeCheckerTest, AnnotatedAssignmentFloatToInt) {
    EXPECT_TRUE(checkHasErrors("x: int = 3.14"));
}

TEST(TypeCheckerTest, AnnotationOnly) {
    EXPECT_TRUE(checkOk("x: int"));
}

TEST(TypeCheckerTest, AnnAssignTypeMismatch) {
    EXPECT_TRUE(checkHasErrors("x: int = \"hello\""));
}

TEST(TypeCheckerTest, AnnAssignNoneToInt) {
    EXPECT_TRUE(checkHasErrors("x: int = None"));
}

TEST(TypeCheckerTest, VariableTypeFromAssignment) {
    auto module = parse("x = 42\nx");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, VariableTypeFromAnnotation) {
    auto module = parse("x: str = \"hello\"\nx");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, FunctionDeclOk) {
    EXPECT_TRUE(checkOk(code("function_decl_ok")));
}

TEST(TypeCheckerTest, FunctionReturnTypeMismatch) {
    EXPECT_TRUE(checkHasErrors(code("function_return_type_mismatch")));
}

TEST(TypeCheckerTest, FunctionReturnNoneFromIntFunc) {
    EXPECT_TRUE(checkHasErrors(code("function_return_none_from_int_func")));
}

TEST(TypeCheckerTest, FunctionReturnNoneOk) {
    EXPECT_TRUE(checkOk(code("function_return_none_ok")));
}

TEST(TypeCheckerTest, FunctionCallReturnType) {
    auto module = parse(code("function_call_return_type"));
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, FunctionParamTypes) {
    EXPECT_TRUE(checkOk(code("function_param_types")));
}

TEST(TypeCheckerTest, FunctionParamBadReturn) {
    EXPECT_TRUE(checkHasErrors(code("function_param_bad_return")));
}

TEST(TypeCheckerTest, ListLiteral) {
    auto t = getExprType("[1, 2, 3]");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::List);
    auto& lt = static_cast<ListType&>(*t);
    EXPECT_EQ(lt.elementType->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, EmptyListLiteral) {
    auto t = getExprType("[]");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::List);
}

TEST(TypeCheckerTest, DictLiteral) {
    auto t = getExprType("{\"a\": 1, \"b\": 2}");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Dict);
    auto& dt = static_cast<DictType&>(*t);
    EXPECT_EQ(dt.keyType->kind(), Type::Kind::Str);
    EXPECT_EQ(dt.valueType->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, TupleLiteral) {
    auto t = getExprType("(1, \"a\", True)");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Tuple);
    auto& tt = static_cast<TupleType&>(*t);
    ASSERT_EQ(tt.elementTypes.size(), 3u);
    EXPECT_EQ(tt.elementTypes[0]->kind(), Type::Kind::Int);
    EXPECT_EQ(tt.elementTypes[1]->kind(), Type::Kind::Str);
    EXPECT_EQ(tt.elementTypes[2]->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, ListSubscriptType) {
    auto module = parse("x: list[int] = [1, 2, 3]\nx[0]");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, StringSubscriptType) {
    auto module = parse("s: str = \"hello\"\ns[0]");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, ResolveListType) {
    EXPECT_TRUE(checkOk("x: list[int] = [1, 2, 3]"));
}

TEST(TypeCheckerTest, ResolveDictType) {
    EXPECT_TRUE(checkOk("x: dict[str, int] = {\"a\": 1}"));
}

TEST(TypeCheckerTest, ResolveUnknownType) {
    EXPECT_TRUE(checkHasErrors("x: Foo = 5"));
}

TEST(TypeCheckerTest, BoolSubtypeOfInt) {
    auto boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    EXPECT_TRUE(boolType->isSubtypeOf(*intType));
}

TEST(TypeCheckerTest, IntSubtypeOfFloat) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    EXPECT_TRUE(intType->isSubtypeOf(*floatType));
}

TEST(TypeCheckerTest, BoolSubtypeOfFloat) {
    auto boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    auto floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    EXPECT_TRUE(boolType->isSubtypeOf(*floatType));
}

TEST(TypeCheckerTest, IntNotSubtypeOfStr) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    EXPECT_FALSE(intType->isSubtypeOf(*strType));
}

TEST(TypeCheckerTest, FloatNotSubtypeOfInt) {
    auto floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    EXPECT_FALSE(floatType->isSubtypeOf(*intType));
}

TEST(TypeCheckerTest, SubtypeOfAny) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto boxedType = std::make_shared<BoxedType>();
    EXPECT_TRUE(intType->isSubtypeOf(*boxedType));
}

TEST(TypeCheckerTest, SubtypeOfUnion) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    std::vector<std::shared_ptr<Type>> types = {intType, strType};
    auto unionType = std::make_shared<UnionType>(std::move(types));
    EXPECT_TRUE(intType->isSubtypeOf(*unionType));
    EXPECT_TRUE(strType->isSubtypeOf(*unionType));
}

TEST(TypeCheckerTest, PrimitiveTypeEquality) {
    auto a = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto b = std::make_shared<PrimitiveType>(Type::Kind::Int);
    EXPECT_TRUE(a->equals(*b));
}

TEST(TypeCheckerTest, PrimitiveTypeInequality) {
    auto a = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto b = std::make_shared<PrimitiveType>(Type::Kind::Str);
    EXPECT_FALSE(a->equals(*b));
}

TEST(TypeCheckerTest, ListTypeEquality) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto a = std::make_shared<ListType>(intType);
    auto b = std::make_shared<ListType>(intType);
    EXPECT_TRUE(a->equals(*b));
}

TEST(TypeCheckerTest, FunctionTypeEquality) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto a = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{intType, intType}, intType);
    auto b = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{intType, intType}, intType);
    EXPECT_TRUE(a->equals(*b));
}

TEST(TypeCheckerTest, TypeToString) {
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Int)->toString(), "int");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Float)->toString(), "float");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Bool)->toString(), "bool");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Str)->toString(), "str");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::None_)->toString(), "None");

    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    EXPECT_EQ(std::make_shared<ListType>(intType)->toString(), "list[int]");
    EXPECT_EQ(std::make_shared<DictType>(strType, intType)->toString(), "dict[str, int]");
    EXPECT_EQ(std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{intType, intType}, intType)->toString(),
        "(int, int) -> int");
}

TEST(TypeCheckerTest, IfStatement) {
    EXPECT_TRUE(checkOk(code("if_statement")));
}

TEST(TypeCheckerTest, WhileLoop) {
    EXPECT_TRUE(checkOk(code("while_loop")));
}

TEST(TypeCheckerTest, ForLoop) {
    EXPECT_TRUE(checkOk(code("for_loop")));
}

TEST(TypeCheckerTest, TryStatement) {
    EXPECT_TRUE(checkOk(code("try_statement")));
}

TEST(TypeCheckerTest, ClassDecl) {
    EXPECT_TRUE(checkOk(code("class_decl")));
}

TEST(TypeCheckerTest, ImplicitSelfMethodType) {
    auto module = parse(code("implicit_self_method_type"));
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    auto exports = tc.getExports();
    auto it = exports.find("Point");
    ASSERT_NE(it, exports.end());
    auto* classType = dynamic_cast<ClassType*>(it->second.get());
    ASSERT_NE(classType, nullptr);
    auto methodIt = classType->methods.find("distance");
    ASSERT_NE(methodIt, classType->methods.end());
    auto* funcType = dynamic_cast<FunctionType*>(methodIt->second.get());
    ASSERT_NE(funcType, nullptr);
    EXPECT_EQ(funcType->paramTypes.size(), 0u);
}

TEST(TypeCheckerTest, ImplicitSelfFieldAccess) {
    EXPECT_TRUE(checkOk(code("implicit_self_field_access")));
}

TEST(TypeCheckerTest, ExplicitSelfPyModeType) {
    auto module = parse(
        code("explicit_self_py_mode_type"),
        false
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    auto exports = tc.getExports();
    auto it = exports.find("Point");
    ASSERT_NE(it, exports.end());
    auto* classType = dynamic_cast<ClassType*>(it->second.get());
    ASSERT_NE(classType, nullptr);
    auto methodIt = classType->methods.find("distance");
    ASSERT_NE(methodIt, classType->methods.end());
    auto* funcType = dynamic_cast<FunctionType*>(methodIt->second.get());
    ASSERT_NE(funcType, nullptr);
    EXPECT_EQ(funcType->paramTypes.size(), 0u);
}

TEST(TypeCheckerTest, ClassInstantiation) {
    auto module = parse(code("class_instantiation"));
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Instance);
}

TEST(TypeCheckerTest, PrintReturnsNone) {
    auto t = getExprType("print(\"hello\")");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::None_);
}

TEST(TypeCheckerTest, LenReturnsInt) {
    auto t = getExprType("len([1, 2, 3])");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, InputReturnsStr) {
    auto t = getExprType("input(\"prompt\")");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, RangeReturnsList) {
    auto t = getExprType("range(10)");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::List);
}

TEST(TypeCheckerTest, FibonacciFunction) {
    EXPECT_TRUE(checkOk(code("fibonacci_function")));
}

TEST(TypeCheckerTest, AnnotatedVariableUsedInExpression) {
    EXPECT_TRUE(checkOk(code("annotated_variable_used_in_expression")));
}

TEST(TypeCheckerTest, MultipleStatements) {
    EXPECT_TRUE(checkOk(code("multiple_statements")));
}

TEST(TypeCheckerTest, ListSliceReturnsListType) {
    auto type = getExprType("x: list[int] = [1, 2, 3]\nx[1:3]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::List);
}

TEST(TypeCheckerTest, StringSliceReturnsStr) {
    auto type = getExprType("s: str = \"hello\"\ns[1:3]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, StringSubscriptReturnsStr) {
    auto type = getExprType("s: str = \"hello\"\ns[0]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, StringCasefoldReturnsStr) {
    auto type = getExprType("s: str = \"Hello\"\ns.casefold()");
    ASSERT_NE(type, nullptr);
    if (type->kind() == Type::Kind::Function) {
        auto& ft = static_cast<FunctionType&>(*type);
        EXPECT_EQ(ft.returnType->kind(), Type::Kind::Str);
    }
}

TEST(TypeCheckerTest, StringPartitionReturnsList) {
    auto type = getExprType("s: str = \"hello world\"\ns.partition(\" \")");
    ASSERT_NE(type, nullptr);
}

TEST(TypeCheckerTest, IsOperatorReturnsBool) {
    auto type = getExprType("x = 5\nx is 5");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, InOperatorReturnsBool) {
    auto type = getExprType("x = 5\nx in [1, 2, 3]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, RegisterExternalModuleResolvesImport) {
    auto module = parse(code("register_external_module_resolves_import"));
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);

    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    exports["add"] = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{
            std::make_shared<PrimitiveType>(Type::Kind::Int),
            std::make_shared<PrimitiveType>(Type::Kind::Int)
        },
        std::make_shared<PrimitiveType>(Type::Kind::Int)
    );

    TypeChecker tc;
    tc.registerExternalModule("utils", exports);
    tc.check(*module);

    for (auto& stmt : module->body) {
        if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            if (assign->value && assign->value->type) {
                EXPECT_EQ(assign->value->type->kind(), Type::Kind::Int);
            }
        }
    }
}

TEST(TypeCheckerTest, GetExportsReturnsDefinedFunctions) {
    auto module = parse(code("get_exports_returns_defined_functions"));
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);

    auto exports = tc.getExports();
    EXPECT_TRUE(exports.count("multiply") > 0);
    EXPECT_TRUE(exports.count("x") > 0);
    EXPECT_TRUE(exports.count("print") == 0);
}

TEST(TypeCheckerTest, UnknownImportProducesError) {
    auto module = parse(
        "from utils import nonexistent\n"
    );
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);

    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    exports["add"] = std::make_shared<PrimitiveType>(Type::Kind::Int);

    TypeChecker tc;
    tc.registerExternalModule("utils", exports);
    tc.check(*module);

    EXPECT_TRUE(tc.hasErrors());
}

TEST(TypeCheckerTest, CrossModuleTypeInfoFlowsToCallExpr) {
    auto module = parse(code("cross_module_type_info_flows_to_call_expr"));
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);

    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    exports["square"] = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{
            std::make_shared<PrimitiveType>(Type::Kind::Int)
        },
        std::make_shared<PrimitiveType>(Type::Kind::Int)
    );

    TypeChecker tc;
    tc.registerExternalModule("math_utils", exports);
    tc.check(*module);

    for (auto& stmt : module->body) {
        if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            if (assign->value) {
                if (auto* call = dynamic_cast<CallExpr*>(assign->value.get())) {
                    ASSERT_NE(call->type, nullptr);
                    EXPECT_EQ(call->type->kind(), Type::Kind::Int);
                }
            }
        }
    }
}

static const std::string TPL_BASE_TC =
    "class Template {\n"
    "    def(inner: str) { self._inner = inner }\n"
    "    @staticmethod\n"
    "    def escape(s: str) -> str { return s }\n"
    "}\n";

TEST(TypeCheckerTest, TypedTemplateRequiresTemplateBase) {
    EXPECT_TRUE(checkHasErrors(code("typed_template_requires_template_base")));
}

TEST(TypeCheckerTest, TypedTemplateExtendsTemplateOK) {
    EXPECT_TRUE(checkOk(
        TPL_BASE_TC +
        "class Foo(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"a\"\n"
        "y = template[Foo] {!{x}}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateGrandchildExtendsTemplateOK) {
    EXPECT_TRUE(checkOk(
        TPL_BASE_TC +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "class MyHTML(HTML) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"a\"\n"
        "y = template[MyHTML] {!{x}}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateUnknownContentTypeErrors) {
    EXPECT_TRUE(checkHasErrors(
        "y = template[NoSuchType] {hello}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateStructTemplateReservedForD037) {
    auto module = parse(code("typed_template_struct_template_reserved_for_d037"));
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_TRUE(tc.hasErrors());
    bool sawStructTemplate = false;
    for (auto& d : tc.diagnostics()) {
        if (d.message.find("StructTemplate") != std::string::npos) {
            sawStructTemplate = true;
            break;
        }
    }
    EXPECT_TRUE(sawStructTemplate)
        << "Expected error mentioning StructTemplate reservation";
}

TEST(TypeCheckerTest, UntypedTemplateUnchangedStillStr) {
    auto module = parse("y: str = template {hello}\n");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    EXPECT_FALSE(tc.hasErrors());
}

TEST(TypeCheckerTest, FireProducesTaskOfCalleeReturn) {
    EXPECT_TRUE(checkOk(code("fire_produces_task_of_callee_return")));
}

TEST(TypeCheckerTest, AwaitUnwrapsAsyncDefReturn) {
    EXPECT_TRUE(checkOk(code("await_unwraps_async_def_return")));
}

TEST(TypeCheckerTest, BareTaskAnnotationRefinesFromRHS) {
    EXPECT_TRUE(checkOk(code("bare_task_annotation_refines_from_rhs")));
    EXPECT_TRUE(checkHasErrors(code("bare_task_annotation_refines_from_rhs_2")));
}

TEST(TypeCheckerTest, AwaitIntResultNotAssignableToStr) {
    EXPECT_TRUE(checkHasErrors(code("await_int_result_not_assignable_to_str")));
}

TEST(TypeCheckerTest, AwaitOnSyncFunctionIsError) {
    EXPECT_TRUE(checkHasErrors(code("await_on_sync_function_is_error")));
}

TEST(TypeCheckerTest, WrongExplicitTaskParamIsError) {
    EXPECT_TRUE(checkHasErrors(code("wrong_explicit_task_param_is_error")));
}

TEST(TypeCheckerTest, DictIntKeyIntIndexOk) {
    EXPECT_TRUE(checkOk(code("dict_int_key_int_index_ok")));
}

TEST(TypeCheckerTest, DictIntKeyStrIndexIsError) {
    EXPECT_TRUE(checkHasErrors(code("dict_int_key_str_index_is_error")));
}

TEST(TypeCheckerTest, DictStrKeyStrIndexOk) {
    EXPECT_TRUE(checkOk(code("dict_str_key_str_index_ok")));
}

TEST(TypeCheckerTest, DictStrKeyIntIndexIsError) {
    EXPECT_TRUE(checkHasErrors(code("dict_str_key_int_index_is_error")));
}

TEST(TypeCheckerTest, DictWrongKeyTypeOnAssignIsError) {
    EXPECT_TRUE(checkHasErrors(code("dict_wrong_key_type_on_assign_is_error")));
}

TEST(TypeCheckerTest, DictHomogeneousLiteralOk) {
    EXPECT_TRUE(checkOk(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2, \"c\": 3}\n"));
}

TEST(TypeCheckerTest, DictMixedKeyLiteralIsError) {
    EXPECT_TRUE(checkHasErrors(
        "d: dict[int, str] = {1: \"a\", \"2\": \"b\"}\n"));
}

static std::vector<std::string> checkMessages(const std::string& source) {
    std::vector<std::string> out;
    auto module = parse(source);
    if (!module) { out.push_back("PARSE FAIL"); return out; }
    Sema sema; sema.analyze(*module);
    TypeChecker tc; tc.check(*module);
    for (auto& d : tc.diagnostics())
        if (d.level == TypeDiagnostic::Level::Error) out.push_back(d.message);
    return out;
}
static bool msgsContain(const std::vector<std::string>& m, const std::string& needle) {
    for (auto& s : m) if (s.find(needle) != std::string::npos) return true;
    return false;
}

TEST(TypeCheckerTest, D045_SameClassPrivateAccessOk) {
    EXPECT_TRUE(checkOk(code("d045__same_class_private_access_ok")));
}

TEST(TypeCheckerTest, D045_SamePackageProtectedAccessOk) {
    EXPECT_TRUE(checkOk(code("d045__same_package_protected_access_ok")));
}

TEST(TypeCheckerTest, D045_SubclassInheritedProtectedOk) {
    EXPECT_TRUE(checkOk(code("d045__subclass_inherited_protected_ok")));
}

TEST(TypeCheckerTest, D045_RecognizedDundersOk) {
    EXPECT_TRUE(checkOk(code("d045__recognized_dunders_ok")));
}

TEST(TypeCheckerTest, D045_RecognizedModuleMetadataOk) {
    EXPECT_TRUE(checkOk("__version__: str = \"1.0\"\n"));
}

TEST(TypeCheckerTest, D045_PrivateAcrossClassesRejected) {
    auto m = checkMessages(code("d045__private_across_classes_rejected"));
    EXPECT_TRUE(msgsContain(m, "cannot access"));
    EXPECT_TRUE(msgsContain(m, "private"));
}

TEST(TypeCheckerTest, D045_SubclassCannotTouchParentPrivate) {
    auto m = checkMessages(code("d045__subclass_cannot_touch_parent_private"));
    EXPECT_TRUE(msgsContain(m, "cannot access"));
}

TEST(TypeCheckerTest, D045_PrivateFromModuleTopLevelRejected) {
    auto m = checkMessages(code("d045__private_from_module_top_level_rejected"));
    EXPECT_TRUE(msgsContain(m, "private to"));
}

TEST(TypeCheckerTest, D045_UnrecognizedClassDunderRejected) {
    auto m = checkMessages(code("d045__unrecognized_class_dunder_rejected"));
    EXPECT_TRUE(msgsContain(m, "reserved"));
    EXPECT_TRUE(msgsContain(m, "not a recognized special method"));
}

TEST(TypeCheckerTest, D045_UnrecognizedModuleDunderRejected) {
    auto m = checkMessages("__weird__: int = 5\n");
    EXPECT_TRUE(msgsContain(m, "not a recognized module metadata name"));
}

TEST(TypeCheckerTest, D045_ReservedDunderCoversDispatchedSet) {
    const char* dispatched[] = {
        "__init__", "__str__", "__repr__",
        "__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__", "__hash__",
        "__add__", "__sub__", "__mul__", "__truediv__", "__floordiv__",
        "__mod__", "__pow__",
        "__iadd__", "__isub__", "__imul__", "__itruediv__", "__ifloordiv__",
        "__imod__", "__ipow__",
        "__neg__", "__pos__", "__abs__",
        "__len__", "__getitem__", "__setitem__", "__contains__",
        "__iter__", "__next__",
        "__call__", "__enter__", "__exit__",
        "__bool__", "__int__", "__float__",
        "__doc__", "__members__",
    };
    for (const char* d : dispatched)
        EXPECT_TRUE(isReservedDunder(d)) << "dispatched dunder not reserved: " << d;

    for (const char* d : {"__and__", "__or__", "__xor__", "__invert__",
                          "__radd__", "__delitem__", "__frob__"})
        EXPECT_FALSE(isReservedDunder(d)) << "unexpectedly reserved: " << d;
}

TEST(TypeCheckerTest, D045_NameClassification) {
    EXPECT_EQ(classifyName("name"), NameVisibility::Public);
    EXPECT_EQ(classifyName("_"), NameVisibility::Public);
    EXPECT_EQ(classifyName("_x"), NameVisibility::Protected);
    EXPECT_EQ(classifyName("_routes_by_sd"), NameVisibility::Protected);
    EXPECT_EQ(classifyName("__x"), NameVisibility::Private);
    EXPECT_EQ(classifyName("__secret"), NameVisibility::Private);
    EXPECT_EQ(classifyName("__init__"), NameVisibility::ReservedDunder);
    EXPECT_EQ(classifyName("__doc__"), NameVisibility::ReservedDunder);
}

TEST(TypeCheckerTest, GenericClassAndFunctionAccepted) {
    EXPECT_TRUE(checkOk(code("generic_class_and_function_accepted")));
}

TEST(TypeCheckerTest, GenericUnboundedMethodCallRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_unbounded_method_call_rejected")));
}

TEST(TypeCheckerTest, GenericUnboundedSubscriptRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_unbounded_subscript_rejected")));
}

TEST(TypeCheckerTest, GenericPolymorphicRecursionCapped) {
    EXPECT_TRUE(checkHasErrors(code("generic_polymorphic_recursion_capped")));
}

TEST(TypeCheckerTest, GenericClassArityMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_class_arity_mismatch_rejected")));
}

TEST(TypeCheckerTest, GenericEqualityAllowedOrderingRejected) {
    EXPECT_TRUE(checkOk(code("generic_equality_allowed_ordering_rejected")));
    EXPECT_TRUE(checkHasErrors(code("generic_equality_allowed_ordering_rejected_2")));
}

TEST(TypeCheckerTest, GenericMethodUnsolvedTypeParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_method_unsolved_type_param_rejected")));
}

TEST(TypeCheckerTest, GenericMethodDualDefinitionRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_method_dual_definition_rejected")));
}

TEST(TypeCheckerTest, GenericMethodArityMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_method_arity_mismatch_rejected")));
}

TEST(TypeCheckerTest, GenericMethodUnboundedMemberAccessRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_method_unbounded_member_access_rejected")));
}

TEST(TypeCheckerTest, GenericMethodOnGenericClassDoubleMonoOk) {
    EXPECT_TRUE(checkOk(code("generic_method_on_generic_class_double_mono_ok")));
}

TEST(TypeCheckerTest, GenericMethodShadowsClassTypeParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_method_shadows_class_type_param_rejected")));
}

TEST(TypeCheckerTest, GenericMethodPolymorphicRecursionCapped) {
    EXPECT_TRUE(checkHasErrors(code("generic_method_polymorphic_recursion_capped")));
}

TEST(TypeCheckerTest, GenericInstantiationSubclassRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_instantiation_subclass_rejected")));
}

TEST(TypeCheckerTest, GenericUnionTypeArgumentAccepted) {
    EXPECT_TRUE(checkOk(code("generic_union_type_argument_accepted")));
}

TEST(TypeCheckerTest, BoundedTypeParamMemberAccessOk) {
    EXPECT_TRUE(checkOk(code("bounded_type_param_member_access_ok")));
}

TEST(TypeCheckerTest, BoundedTypeParamSubclassArgAccepted) {
    EXPECT_TRUE(checkOk(code("bounded_type_param_subclass_arg_accepted")));
}

TEST(TypeCheckerTest, BoundedGenericClassMemberAccessOk) {
    EXPECT_TRUE(checkOk(code("bounded_generic_class_member_access_ok")));
}

TEST(TypeCheckerTest, BoundedTypeParamArgViolatesBoundRejected) {
    EXPECT_TRUE(checkHasErrors(code("bounded_type_param_arg_violates_bound_rejected")));
}

TEST(TypeCheckerTest, BoundedGenericClassArgViolatesBoundRejected) {
    EXPECT_TRUE(checkHasErrors(code("bounded_generic_class_arg_violates_bound_rejected")));
}

TEST(TypeCheckerTest, UnboundedTypeParamMemberAccessStillRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def bad[T](x: T) -> str { return x.speak() }\n"));
}

TEST(TypeCheckerTest, BoundedGenericMethodOk) {
    EXPECT_TRUE(checkOk(code("bounded_generic_method_ok")));
}

TEST(TypeCheckerTest, WalrusEmptyListInferRejected) {
    EXPECT_TRUE(checkHasErrors("xs := []\n"));
}

TEST(TypeCheckerTest, WalrusEmptyDictInferRejected) {
    EXPECT_TRUE(checkHasErrors("ds := {}\n"));
}

TEST(TypeCheckerTest, WalrusConcreteListInferOk) {
    EXPECT_TRUE(checkOk("xs := [1, 2, 3]\n"));
}

TEST(TypeCheckerTest, AnnotatedEmptyListOk) {
    EXPECT_TRUE(checkOk("xs: list[int] = []\n"));
}

TEST(TypeCheckerTest, WalrusExprConcreteOk) {
    EXPECT_TRUE(checkOk("if (n := 5) > 0 {\n  print(n)\n}\n"));
}

TEST(TypeCheckerTest, WalrusMixedListInferRejected) {
    EXPECT_TRUE(checkHasErrors("xs := [1, \"a\"]\n"));
}

TEST(TypeCheckerTest, WalrusIntFloatListInferRejected) {
    EXPECT_TRUE(checkHasErrors("xs := [1, 2.0]\n"));
}

TEST(TypeCheckerTest, WalrusHomogeneousListInferOk) {
    EXPECT_TRUE(checkOk("xs := [10, 20, 30]\n"));
}

TEST(TypeCheckerTest, AnnotatedUnionMixedListOk) {
    EXPECT_TRUE(checkOk("xs: list[int | str | float] = [1, \"a\", 3.0]\n"));
}

TEST(TypeCheckerTest, MixedListAgainstIntAnnotationRejected) {
    EXPECT_TRUE(checkHasErrors("xs: list[int] = [1, \"a\"]\n"));
}

TEST(TypeCheckerTest, LambdaBodyBadBindingRejected) {
    EXPECT_TRUE(checkHasErrors(code("lambda_body_bad_binding_rejected")));
}

TEST(TypeCheckerTest, LambdaBodyReturnTypeMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(code("lambda_body_return_type_mismatch_rejected")));
}

TEST(TypeCheckerTest, LambdaBodyWellTypedOk) {
    EXPECT_TRUE(checkOk(code("lambda_body_well_typed_ok")));
}

TEST(TypeCheckerTest, MethodOnAnyReceiverRejected) {
    EXPECT_TRUE(checkHasErrors(code("method_on_any_receiver_rejected")));
}

TEST(TypeCheckerTest, MethodOnTypedTaskListOk) {
    EXPECT_TRUE(checkOk(code("method_on_typed_task_list_ok")));
}

TEST(TypeCheckerTest, FieldReadOnAnyReceiverRejected) {
    EXPECT_TRUE(checkHasErrors(code("field_read_on_any_receiver_rejected")));
}

TEST(TypeCheckerTest, DeferOwnArgToBorrowingParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("defer_own_arg_to_borrowing_param_rejected")));
}

TEST(TypeCheckerTest, DeferMissingOwnAtOwnParamRejected) {
    EXPECT_TRUE(checkHasErrors(code("defer_missing_own_at_own_param_rejected")));
}

TEST(TypeCheckerTest, DeferArgTypeMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(code("defer_arg_type_mismatch_rejected")));
}

TEST(TypeCheckerTest, DeferWellTypedCallOk) {
    EXPECT_TRUE(checkOk(code("defer_well_typed_call_ok")));
}

// Identity resources: the typechecker owns the borrow-forge into an own-param ctor and the dub
// of a claim holder; the move/claim cases (use-after-move, double-move) live in OwnershipCheckTest.cpp.

TEST(TypeCheckerTest, IdentityResourceBorrowIntoOwnCtorRejected) {
    EXPECT_TRUE(checkHasErrors(code("identity_resource_borrow_into_own_ctor_rejected")));
}

TEST(TypeCheckerTest, IdentityResourceDubRejected) {
    EXPECT_TRUE(checkHasErrors(code("identity_resource_dub_rejected")));
}

TEST(TypeCheckerTest, IdentityResourceBlessedSpellingsOk) {
    EXPECT_TRUE(checkOk(code("identity_resource_blessed_spellings_ok")));
}

static const char* kIss25Class =
    "class Test {\n"
    "    def(name: str) { self.name: str = name }\n"
    "    def print_junk() { print(\"junk\") }\n"
    "    def double(n: int) -> int { return n * 2 }\n"
    "}\n"
    "test: Test = Test('t')\n";

TEST(TypeCheckerTest, MethodReassignRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "test.print_junk = \"nope\"\n"));
}

TEST(TypeCheckerTest, MethodAnnotatedReassignRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "test.print_junk: str = \"nope\"\n"));
}

TEST(TypeCheckerTest, MethodReassignOnClassRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "Test.print_junk = \"nope\"\n"));
}

TEST(TypeCheckerTest, MethodSelfReassignInCtorRejected) {
    EXPECT_TRUE(checkHasErrors(code("method_self_reassign_in_ctor_rejected")));
}

TEST(TypeCheckerTest, BareMethodReadRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "print(test.print_junk)\n"));
}

TEST(TypeCheckerTest, BareMethodReadOnClassRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "print(Test.print_junk)\n"));
}

TEST(TypeCheckerTest, MethodBoundToCallableRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "cb: Callable[[int], int] = test.double\n"));
}

TEST(TypeCheckerTest, MethodCallsStillOk) {
    EXPECT_TRUE(checkOk(std::string(kIss25Class) +
        "test.print_junk()\n"
        "d: int = test.double(5)\n"
        "test.name = \"renamed\"\n"));
}

TEST(TypeCheckerTest, InheritedMethodReassignRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kIss25Class) +
        "class Sub(Test) {\n"
        "    def(name: str) { self.name: str = name }\n"
        "}\n"
        "s: Sub = Sub('s')\n"
        "s.print_junk = \"nope\"\n"));
}

TEST(TypeCheckerTest, BareBuiltinMethodReadRejected) {
    EXPECT_TRUE(checkHasErrors(code("bare_builtin_method_read_rejected")));
}

TEST(TypeCheckerTest, BareTaskJoinReadRejected) {
    EXPECT_TRUE(checkHasErrors(code("bare_task_join_read_rejected")));
}

TEST(TypeCheckerTest, BuiltinMethodBoundToCallableRejected) {
    EXPECT_TRUE(checkHasErrors(code("builtin_method_bound_to_callable_rejected")));
}

TEST(TypeCheckerTest, BuiltinMethodCallsStillOk) {
    EXPECT_TRUE(checkOk(code("builtin_method_calls_still_ok")));
}

TEST(TypeCheckerTest, UnknownStaticMethodCallOnClassRejected) {
    EXPECT_TRUE(checkHasErrors(code("unknown_static_method_call_on_class_rejected")));
}

TEST(TypeCheckerTest, UnknownClassAttributeReadRejected) {
    EXPECT_TRUE(checkHasErrors(code("unknown_class_attribute_read_rejected")));
}

TEST(TypeCheckerTest, StaticMethodCallOnClassOk) {
    EXPECT_TRUE(checkOk(code("static_method_call_on_class_ok")));
}

TEST(TypeCheckerTest, InheritedStaticMethodThroughSubclassOk) {
    EXPECT_TRUE(checkOk(code("inherited_static_method_through_subclass_ok")));
}

TEST(TypeCheckerTest, UnknownMemberOnSubclassChainRejected) {
    EXPECT_TRUE(checkHasErrors(code("unknown_member_on_subclass_chain_rejected")));
}

static const char* kAmazing =
    "type Amazing {\n"
    "    def amazing_method() -> str\n"
    "}\n";

static std::string checkErrorText(const std::string& source) {
    auto module = parse(source);
    if (!module) return "<parse failed>";
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    std::string all;
    for (auto& d : tc.diagnostics()) all += d.message + "\n";
    return all;
}

TEST(TypeCheckerTest, ContractValuePositionNeedsDeclaredConformance) {
    std::string src = std::string(kAmazing) +
        "class Duck {\n"
        "    def amazing_method() -> str {\n"
        "        return \"quack\"\n"
        "    }\n"
        "}\n"
        "def show(x: Amazing) -> str {\n"
        "    return x.amazing_method()\n"
        "}\n"
        "d: Duck = Duck()\n"
        "show(d)\n";
    EXPECT_TRUE(checkHasErrors(src));
    std::string text = checkErrorText(src);
    EXPECT_NE(text.find("matching method set but no declared conformance"),
              std::string::npos);
    EXPECT_NE(text.find("d as Amazing"), std::string::npos);
    EXPECT_NE(text.find("class Duck -> Amazing"), std::string::npos);
}

TEST(TypeCheckerTest, ContractCastMissingMethodRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Rock {\n"
        "    def weight() -> int {\n"
        "        return 3\n"
        "    }\n"
        "}\n"
        "r: Rock = Rock()\n"
        "a: Amazing = r as Amazing\n"));
}

TEST(TypeCheckerTest, ContractPromiseMissingMethodRejectedAtClass) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Cat -> Amazing {\n"
        "    def other() -> str {\n"
        "        return \"meow\"\n"
        "    }\n"
        "}\n"));
}

TEST(TypeCheckerTest, ContractSignatureMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Off {\n"
        "    def amazing_method(n: int) -> str {\n"
        "        return \"x\"\n"
        "    }\n"
        "}\n"
        "o: Off = Off()\n"
        "a: Amazing = o as Amazing\n"));
}

TEST(TypeCheckerTest, ContractReturnTypeMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Wrong {\n"
        "    def amazing_method() -> int {\n"
        "        return 3\n"
        "    }\n"
        "}\n"
        "w: Wrong = Wrong()\n"
        "a: Amazing = w as Amazing\n"));
}

TEST(TypeCheckerTest, AsTargetMustBeAContract) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Dog -> Amazing {\n"
        "    def amazing_method() -> str {\n"
        "        return \"woof\"\n"
        "    }\n"
        "}\n"
        "class Other {\n"
        "    def hi() -> int {\n"
        "        return 1\n"
        "    }\n"
        "}\n"
        "d: Dog = Dog()\n"
        "x: Amazing = d as Other\n"));
}

TEST(TypeCheckerTest, ContractDownwardReviewRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "type Speaker {\n"
        "    def speak() -> str\n"
        "}\n"
        "class Dog -> Amazing {\n"
        "    def amazing_method() -> str {\n"
        "        return \"woof\"\n"
        "    }\n"
        "}\n"
        "d: Dog = Dog()\n"
        "a: Amazing = d as Amazing\n"
        "s: Speaker = a as Speaker\n"));
}

TEST(TypeCheckerTest, ContractCompositionConflictRejected) {
    EXPECT_TRUE(checkHasErrors(code("contract_composition_conflict_rejected")));
}

TEST(TypeCheckerTest, ContractCannotBeConstructed) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "a: Amazing = Amazing()\n"));
}

TEST(TypeCheckerTest, ContractBoundViolationListsMissingMethod) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Rock {\n"
        "    def weight() -> int {\n"
        "        return 3\n"
        "    }\n"
        "}\n"
        "def go[T: Amazing](x: T) -> str {\n"
        "    return x.amazing_method()\n"
        "}\n"
        "r: Rock = Rock()\n"
        "go(r)\n"));
}

TEST(TypeCheckerTest, SumRejectsNonNumericElements) {
    EXPECT_TRUE(checkHasErrors(code("sum_rejects_non_numeric_elements")));
    EXPECT_FALSE(checkHasErrors(code("sum_rejects_non_numeric_elements_2")));
}

TEST(TypeCheckerTest, MinMaxRejectContainerElements) {
    EXPECT_TRUE(checkHasErrors(code("min_max_reject_container_elements")));
    EXPECT_FALSE(checkHasErrors(code("min_max_reject_container_elements_2")));
}

TEST(TypeCheckerTest, ContractDeclaresNoSuchMethodRejected) {
    EXPECT_TRUE(checkHasErrors(std::string(kAmazing) +
        "class Dog -> Amazing {\n"
        "    def amazing_method() -> str {\n"
        "        return \"woof\"\n"
        "    }\n"
        "    def bark() -> str {\n"
        "        return \"!\"\n"
        "    }\n"
        "}\n"
        "d: Dog = Dog()\n"
        "a: Amazing = d\n"
        "a.bark()\n"));
}

TEST(ParserTest, ContractBodyRejectsFieldsBodiesDefaultsAndEmpty) {
    EXPECT_FALSE(parseErrors("type T {\n    def m() -> str\n}\n").empty()
                 ? false : true);
    EXPECT_TRUE(parseErrors("type Empty { }\n").size() > 0);
    EXPECT_TRUE(parseErrors("type Bad {\n    name: str\n}\n").size() > 0);
    EXPECT_TRUE(parseErrors(
        "type Bad {\n    def m() -> str { return \"x\" }\n}\n").size() > 0);
    EXPECT_TRUE(parseErrors(
        "type Bad {\n    def m(n: int = 3) -> str\n}\n").size() > 0);
}

TEST(TypeCheckerTest, GenericAndMonomorphicSameNameRejected) {
    EXPECT_TRUE(checkHasErrors(code("generic_and_monomorphic_same_name_rejected")));
    EXPECT_TRUE(checkHasErrors(code("generic_and_monomorphic_same_name_rejected_2")));
    EXPECT_TRUE(checkHasErrors(code("generic_and_monomorphic_same_name_rejected_3")));
}

TEST(TypeCheckerTest, IterReturningClassWithoutNextRejected) {
    EXPECT_TRUE(checkHasErrors(code("iter_returning_class_without_next_rejected")));
}

TEST(TypeCheckerTest, InOnScalarRejected) {
    EXPECT_TRUE(checkHasErrors(code("in_on_scalar_rejected")));
    EXPECT_TRUE(checkHasErrors(
        "b: bool = \"b\" in 5.5\n"));
    EXPECT_FALSE(checkHasErrors(code("in_on_scalar_rejected_2")));
    EXPECT_FALSE(checkHasErrors(code("in_on_scalar_rejected_3")));
    EXPECT_FALSE(checkHasErrors(
        "b: bool = 1 in {1, 2}\n"));
    EXPECT_FALSE(checkHasErrors(
        "b: bool = b\"a\" in b\"abc\"\n"));
}

TEST(TypeCheckerTest, InOnTupleRejected) {
    EXPECT_TRUE(checkHasErrors(code("in_on_tuple_rejected")));
}

TEST(TypeCheckerTest, StrInRequiresStrLeft) {
    EXPECT_TRUE(checkHasErrors(
        "b: bool = 5 in \"abc\"\n"));
    EXPECT_FALSE(checkHasErrors(
        "b: bool = \"a\" in \"abc\"\n"));
}

TEST(TypeCheckerTest, InOnInstanceNeedsContains) {
    EXPECT_TRUE(checkHasErrors(code("in_on_instance_needs_contains")));
    EXPECT_FALSE(checkHasErrors(code("in_on_instance_needs_contains_2")));
}

TEST(TypeCheckerTest, BareRangeValueRejected) {
    EXPECT_TRUE(checkHasErrors(
        "print(range(5))\n"));
    EXPECT_TRUE(checkHasErrors(
        "xs: list[int] = sorted(range(5))\n"));
    EXPECT_FALSE(checkHasErrors(
        "for i in range(3) { print(i) }\n"));
    EXPECT_FALSE(checkHasErrors(
        "xs: list[int] = list(range(3))\n"));
    EXPECT_FALSE(checkHasErrors(
        "xs: list[int] = [i * 2 for i in range(3)]\n"));
}

TEST(TypeCheckerTest, IsinstanceSecondArgMustBeType) {
    EXPECT_TRUE(checkHasErrors(code("isinstance_second_arg_must_be_type")));
    EXPECT_FALSE(checkHasErrors(code("isinstance_second_arg_must_be_type_2")));
    EXPECT_FALSE(checkHasErrors(code("isinstance_second_arg_must_be_type_3")));
}

TEST(TypeCheckerTest, NonCallableCalleeRejected) {
    EXPECT_TRUE(checkHasErrors(code("non_callable_callee_rejected")));
}

TEST(TypeCheckerTest, ConstantIntOverflowRejected) {
    EXPECT_TRUE(checkHasErrors("x: int = 2 ** 100\n"));
    EXPECT_TRUE(checkHasErrors("x: int = 2 ** 63\n"));
    EXPECT_TRUE(checkHasErrors("x: int = 9223372036854775807 + 1\n"));
    EXPECT_TRUE(checkHasErrors("x: int = -9223372036854775807 - 2\n"));
    EXPECT_TRUE(checkHasErrors("x: int = 4611686018427387904 * 2\n"));
    EXPECT_TRUE(checkHasErrors("x: int = (2 ** 50) * (2 ** 50)\n"));
    EXPECT_FALSE(checkHasErrors("x: int = 2 ** 62\n"));
    EXPECT_FALSE(checkHasErrors("x: int = 9223372036854775806 + 1\n"));
    EXPECT_FALSE(checkHasErrors("x: int = -9223372036854775807 - 1\n"));
}

TEST(TypeCheckerTest, ConstantDivModByZeroStaysRuntimeError) {
    EXPECT_FALSE(checkHasErrors("x: int = 1 // 0\n"));
    EXPECT_FALSE(checkHasErrors("x: int = 1 % 0\n"));
    EXPECT_FALSE(checkHasErrors("x: int = 7 // 2\n"));
    EXPECT_FALSE(checkHasErrors("x: int = -7 // 2\n"));
    EXPECT_FALSE(checkHasErrors("x: int = 7 % -2\n"));
}

TEST(TypeCheckerTest, SetTypeIsHonest) {
    auto t = getExprType(code("set_type_is_honest"));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Set);
    EXPECT_EQ(t->toString(), "set[int]");
}

TEST(TypeCheckerTest, SetOperatorsTyped) {
    EXPECT_FALSE(checkHasErrors(code("set_operators_typed")));
    EXPECT_TRUE(checkHasErrors(code("set_operators_typed_2")));
    EXPECT_TRUE(checkHasErrors(code("set_operators_typed_3")));
    EXPECT_TRUE(checkHasErrors(code("set_operators_typed_4")));
}

TEST(TypeCheckerTest, ConstantShiftCountRejected) {
    EXPECT_TRUE(checkHasErrors("x: int = 1 << 64\n"));
    EXPECT_TRUE(checkHasErrors("x: int = 1 << -1\n"));
    EXPECT_FALSE(checkHasErrors("x: int = 1 << 62\n"));
}

TEST(TypeCheckerTest, DiscardedTaskStatementRejected) {
    EXPECT_TRUE(checkHasErrors(code("discarded_task_statement_rejected")));
}

TEST(TypeCheckerTest, BareFireStatementAccepted) {
    EXPECT_FALSE(checkHasErrors(code("bare_fire_statement_accepted")));
}

TEST(TypeCheckerTest, BoundTaskDeclarationAccepted) {
    EXPECT_FALSE(checkHasErrors(code("bound_task_declaration_accepted")));
}

TEST(TypeCheckerTest, AsyncAnyReturnRejected) {
    EXPECT_TRUE(checkHasErrors(code("async_any_return_rejected")));
}

TEST(TypeCheckerTest, FireOnAnyReturningCalleeRejected) {
    EXPECT_TRUE(checkHasErrors(code("fire_on_any_returning_callee_rejected")));
}

TEST(TypeCheckerTest, DubOfTaskRejected) {
    EXPECT_TRUE(checkHasErrors(code("dub_of_task_rejected")));
}

TEST(TypeCheckerTest, RaiseFromCauseRejected) {
    EXPECT_TRUE(checkHasErrors(code("raise_from_cause_rejected")));
}

TEST(TypeCheckerTest, OwnParamByKeywordRequiresOwn) {
    EXPECT_TRUE(checkHasErrors(code("own_param_by_keyword_requires_own")));
}

TEST(TypeCheckerTest, OwnParamByKeywordAcceptsOwnMarkedArg) {
    EXPECT_TRUE(checkOk(code("own_param_by_keyword_accepts_own_marked_arg")));
}

TEST(TypeCheckerTest, OwnParamByKeywordAcceptsFreshValue) {
    EXPECT_TRUE(checkOk(code("own_param_by_keyword_accepts_fresh_value")));
}

TEST(TypeCheckerTest, BorrowParamByKeywordRejectsOwn) {
    EXPECT_TRUE(checkHasErrors(code("borrow_param_by_keyword_rejects_own")));
}

TEST(TypeCheckerTest, DubSatisfiesOwnParam) {
    EXPECT_TRUE(checkOk(code("dub_satisfies_own_param")));
}

TEST(TypeCheckerTest, DubSatisfiesOwnParamByKeyword) {
    EXPECT_TRUE(checkOk(code("dub_satisfies_own_param_by_keyword")));
}

TEST(TypeCheckerTest, UnmarkedBindingStillRejectedByOwnParam) {
    EXPECT_TRUE(checkHasErrors(code("unmarked_binding_still_rejected_by_own_param")));
}

TEST(TypeCheckerTest, NoConstructorClassRejectsArgs) {
    EXPECT_TRUE(checkHasErrors(code("no_ctor_class_rejects_args")));
}

TEST(TypeCheckerTest, NoConstructorSubclassRejectsParentArgs) {
    EXPECT_TRUE(checkHasErrors(code("no_ctor_subclass_rejects_parent_args")));
}

TEST(TypeCheckerTest, NoConstructorClassZeroArgsOk) {
    EXPECT_TRUE(checkOk(code("no_ctor_class_zero_args_ok")));
}

TEST(TypeCheckerTest, ExceptionSubclassMessageConstructionOk) {
    EXPECT_TRUE(checkOk(code("exception_subclass_message_construction_ok")));
}

TEST(TypeCheckerTest, ExceptionSubclassRejectsNonMessageArg) {
    EXPECT_TRUE(checkHasErrors(code("exception_subclass_rejects_non_message_arg")));
}

TEST(TypeCheckerTest, ExceptionSubclassRejectsExtraArgs) {
    EXPECT_TRUE(checkHasErrors(code("exception_subclass_rejects_extra_args")));
}

TEST(TypeCheckerTest, ExceptionMessageFieldMustBeStr) {
    EXPECT_TRUE(checkHasErrors(code("exception_message_field_must_be_str")));
    EXPECT_TRUE(checkHasErrors(
        code("exception_message_field_must_be_str_when_inferred")));
    EXPECT_TRUE(checkOk(code("exception_message_field_str_is_ok")));
}

TEST(TypeCheckerTest, NoConstructorSubclassZeroArgsOk) {
    EXPECT_TRUE(checkOk(code("no_ctor_subclass_zero_args_ok")));
}

TEST(TypeCheckerTest, CapitalCollectionAliasesRejected) {
    EXPECT_TRUE(checkHasErrors("d: Dict = {}"));
    EXPECT_TRUE(checkHasErrors("xs: List = []"));
    EXPECT_TRUE(checkHasErrors("t: Tuple = (1, 2)"));
    EXPECT_TRUE(checkHasErrors("s: Set = []"))
        << "capital Set resolved to a list type, so a list literal was accepted";
}

TEST(TypeCheckerTest, LowercaseCollectionNamesStillResolve) {
    EXPECT_TRUE(checkOk("d: dict = {}"));
    EXPECT_TRUE(checkOk("xs: list = []"));
    EXPECT_TRUE(checkOk("t: tuple = (1, 2)"));
    EXPECT_TRUE(checkOk("s: set = set()"));
}

TEST(TypeCheckerTest, BuiltinAttributeMembersAreChecked) {
    EXPECT_TRUE(checkHasErrors(code("builtin_attr_unknown_on_int")));
    EXPECT_TRUE(checkHasErrors(code("builtin_attr_unknown_on_str")));
    EXPECT_TRUE(checkHasErrors(code("builtin_attr_unknown_method_on_str")));
    EXPECT_TRUE(checkHasErrors(code("builtin_attr_unknown_on_list")));
    EXPECT_TRUE(checkOk(code("builtin_attr_known_methods_accepted")));
    EXPECT_TRUE(checkOk(code("dict_dot_access_is_not_an_attribute_error")));
}

TEST(TypeCheckerTest, StrContainsRetiredForInOperator) {
    EXPECT_TRUE(checkHasErrors(code("str_contains_method_rejected")));
    EXPECT_TRUE(checkOk(code("str_substring_in_accepted")));
}

TEST(TypeCheckerTest, ClassObjectIsNotAnInstanceInAContainerArgument) {
    EXPECT_TRUE(checkHasErrors(code("class_object_in_list_arg_rejected")));
    EXPECT_TRUE(checkOk(code("instance_in_list_arg_accepted")));
}

TEST(TypeCheckerTest, GenericConstructionStillNeedsInferableTypeArguments) {
    EXPECT_TRUE(checkHasErrors(code("generic_ctor_without_args_rejected")));
    EXPECT_TRUE(checkOk(code("generic_ctor_explicit_accepted")));
}

TEST(TypeCheckerTest, DuplicateConstructorArityIsRejected) {
    EXPECT_TRUE(checkHasErrors(code("duplicate_ctor_arity_rejected")));
    EXPECT_TRUE(checkOk(code("distinct_ctor_arities_ok")));
}

TEST(TypeCheckerTest, ConstructorOverloadArityIsValidatedAtCallSites) {
    EXPECT_TRUE(checkHasErrors(code("ctor_overload_arity_mismatch_rejected")));
    EXPECT_TRUE(checkHasErrors(code("ctor_overload_zero_args_rejected")));
    EXPECT_TRUE(checkOk(code("ctor_overload_default_range_ok")));
}

TEST(TypeCheckerTest, OverlappingConstructorDefaultsAreRejected) {
    EXPECT_TRUE(checkHasErrors(code("ctor_overlapping_defaults_rejected")));
    EXPECT_TRUE(checkOk(code("ctor_exact_shadow_of_defaults_ok")));
}

TEST(TypeCheckerTest, ConstructorOverloadKwargsBindByName) {
    EXPECT_TRUE(checkOk(code("ctor_overload_kwargs_ok")));
    EXPECT_TRUE(checkHasErrors(code("ctor_overload_kwargs_unknown_name_rejected")));
    EXPECT_TRUE(checkHasErrors(code("ctor_overload_kwargs_missing_required_rejected")));
}

TEST(TypeCheckerTest, UnnarrowedUnionMemberAccessIsRejected) {
    EXPECT_TRUE(checkHasErrors(code("attr_on_unnarrowed_optional_rejected")));
    EXPECT_TRUE(checkHasErrors(code("attr_on_plain_union_rejected")));
    EXPECT_TRUE(checkHasErrors(code("method_call_on_unnarrowed_optional_rejected")));
}

TEST(TypeCheckerTest, ComplementNarrowingUnlocksMemberAccess) {
    EXPECT_TRUE(checkOk(code("attr_after_complement_guard_accepted")));
    EXPECT_TRUE(checkOk(code("attr_after_else_return_accepted")));
}

TEST(TypeCheckerTest, MinVarargsTypeIsConcrete) {
    EXPECT_TRUE(checkHasErrors(code("min_varargs_concrete_mismatch_rejected")));
}

TEST(TypeCheckerTest, ClassWithoutStrCannotBeRendered) {
    EXPECT_TRUE(checkHasErrors(code("render_class_without_str_print_rejected")));
    EXPECT_TRUE(checkHasErrors(code("render_class_without_str_str_call_rejected")));
    EXPECT_TRUE(checkHasErrors(code("render_class_without_str_fstring_rejected")));
    EXPECT_TRUE(checkHasErrors(code("render_class_without_str_splice_rejected")));
    EXPECT_TRUE(checkOk(code("render_class_with_str_accepted")));
    EXPECT_TRUE(checkOk(code("render_container_accepted")));
    EXPECT_TRUE(checkHasErrors(code("render_contract_typed_value_rejected")));
}

TEST(TypeCheckerTest, TemplateSpreadOperandsMustBeRenderableLists) {
    EXPECT_TRUE(checkHasErrors(code("spread_of_nested_container_rejected")));
    EXPECT_TRUE(checkHasErrors(code("spread_of_non_list_rejected")));
    EXPECT_TRUE(checkHasErrors(code("spread_of_nested_list_rejected")));
    EXPECT_TRUE(checkHasErrors(code("spread_of_class_without_str_rejected")));
    EXPECT_TRUE(checkHasErrors(code("join_of_class_without_str_rejected")));
    EXPECT_TRUE(checkHasErrors(code("join_separator_without_str_rejected")));
    EXPECT_TRUE(checkOk(code("spread_of_str_list_accepted")));
}

TEST(TypeCheckerTest, WalrusBindingTypeIsFixedAtDeclaration) {
    EXPECT_TRUE(checkHasErrors(code("walrus_rebind_wrong_type_rejected")));
    EXPECT_TRUE(checkHasErrors(code("walrus_rebind_container_wrong_type_rejected")));
    EXPECT_TRUE(checkOk(code("walrus_rebind_same_type_accepted")));
}

TEST(TypeCheckerTest, RecursiveTypeAliasesMustBeProductive) {
    EXPECT_TRUE(checkOk(code("recursive_type_alias_accepted")));
    EXPECT_TRUE(checkHasErrors(code("recursive_type_alias_self_only_rejected")));
    EXPECT_TRUE(checkHasErrors(code("recursive_type_alias_bare_arm_rejected")));
    EXPECT_TRUE(checkOk(code("union_element_nested_literal_accepted")));
}

TEST(TypeCheckerTest, IntcLocalRejected) {
    EXPECT_TRUE(checkHasErrors(code("intc_local_rejected")));
}

TEST(TypeCheckerTest, IntcFieldRejected) {
    EXPECT_TRUE(checkHasErrors(code("intc_field_rejected")));
}

TEST(TypeCheckerTest, IntcParamInDragonFnRejected) {
    EXPECT_TRUE(checkHasErrors(code("intc_param_in_dragon_fn_rejected")));
}

TEST(TypeCheckerTest, IntcReturnInDragonFnRejected) {
    EXPECT_TRUE(checkHasErrors(code("intc_return_in_dragon_fn_rejected")));
}

TEST(TypeCheckerTest, IntcInExternSignatureOk) {
    EXPECT_TRUE(checkOk(code("intc_in_extern_signature_ok")));
}

TEST(TypeCheckerTest, DequeIsNotAList) {
    EXPECT_TRUE(checkHasErrors(code("deque_assigned_to_list_rejected")));
    EXPECT_TRUE(checkHasErrors(code("list_assigned_to_deque_rejected")));
    EXPECT_TRUE(checkHasErrors(code("deque_passed_to_list_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("deque_returned_as_list_rejected")));
    EXPECT_TRUE(checkHasErrors(code("deque_element_type_mismatch_rejected")));
    EXPECT_TRUE(checkOk(code("deque_flows_as_deque_accepted")));
}

TEST(TypeCheckerTest, DequeIsNotIterable) {
    EXPECT_TRUE(checkHasErrors(code("deque_for_loop_rejected")));
    EXPECT_TRUE(checkHasErrors(code("deque_comprehension_rejected")));
    EXPECT_TRUE(checkHasErrors(code("deque_sorted_rejected")));
    EXPECT_TRUE(checkHasErrors(code("deque_index_rejected")));
    EXPECT_TRUE(checkOk(code("user_sorted_shadowing_builtin_accepted")));
    EXPECT_TRUE(checkOk(code("user_min_shadowing_builtin_accepted")));
}

TEST(TypeCheckerTest, DefaultedParamsDoNotSkipArgumentChecking) {
    EXPECT_TRUE(checkHasErrors(code("list_arg_to_defaulted_str_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("int_arg_to_defaulted_str_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("list_arg_to_defaulted_method_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("list_arg_to_defaulted_self_method_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("list_arg_to_defaulted_ctor_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("arg_before_keyword_arg_type_checked")));
    EXPECT_TRUE(checkOk(code("defaulted_call_with_matching_args_accepted")));
}

TEST(TypeCheckerTest, KeywordArgumentValuesAreTypeChecked) {
    EXPECT_TRUE(checkHasErrors(code("keyword_arg_value_type_checked")));
    EXPECT_TRUE(checkHasErrors(code("keyword_arg_list_to_str_param_rejected")));
    EXPECT_TRUE(checkHasErrors(code("keyword_arg_on_method_type_checked")));
    EXPECT_TRUE(checkHasErrors(code("keyword_arg_on_ctor_type_checked")));
    EXPECT_TRUE(checkHasErrors(code("keyword_arg_on_ctor_overload_type_checked")));
    EXPECT_TRUE(checkHasErrors(code("keyword_arg_trailing_default_value_type_checked")));
    EXPECT_TRUE(checkOk(code("keyword_call_with_matching_args_accepted")));
}
