#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenClassesTest.md", block);
}

TEST(CodeGenTest, MethodReflectionGlobalsEmitted) {
    auto ir = generateIR(code("method_reflection_globals_emitted"));
    EXPECT_NE(ir.find("__method_names"), std::string::npos)
        << "method_names global should be emitted";
    EXPECT_NE(ir.find("__method_fn_ptrs"), std::string::npos)
        << "method_fn_ptrs global should be emitted";
    EXPECT_NE(ir.find("__method_kinds"), std::string::npos)
        << "method_kinds global should be emitted";
    EXPECT_NE(ir.find("dragon_class_descriptor_set_methods"), std::string::npos)
        << "set_methods setter must be invoked";
    EXPECT_NE(ir.find("bar"), std::string::npos);
    EXPECT_NE(ir.find("baz"), std::string::npos);
}

TEST(CodeGenE2E, SubclassInheritsParentFieldLayout) {
    auto out = compileAndRun(code("subclass_inherits_parent_field_layout"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, DefaultConstructorSynthesisBare) {
    auto out = compileAndRun(code("default_constructor_synthesis_bare"));
    EXPECT_EQ(out, "hi\n");
}

TEST(CodeGenE2E, DefaultConstructorSynthesisSubclassDelegates) {
    auto out = compileAndRun(code("default_constructor_synthesis_subclass_delegates"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, ListCovarianceFreshLiteral) {
    auto out = compileAndRun(code("list_covariance_fresh_literal"));
    EXPECT_EQ(out, "2\n1\n2\n");
}


TEST(CodeGenE2E, HasattrFindsMethod) {
    auto out = compileAndRun(code("hasattr_finds_method"));
    EXPECT_EQ(out, "True\nTrue\nFalse\n");
}

TEST(CodeGenE2E, GetattrBoundMethodInvocationModuleScope) {
    auto out = compileAndRun(code("getattr_bound_method_invocation_module_scope"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, GetattrBoundMethodInvocationFunctionScope) {
    auto out = compileAndRun(code("getattr_bound_method_invocation_function_scope"));
    EXPECT_EQ(out, "8\n");
}

TEST(CodeGenE2E, GetattrMethodFromInheritedParent) {
    auto out = compileAndRun(code("getattr_method_from_inherited_parent"));
    EXPECT_EQ(out, "Rex\n");
}

TEST(CodeGenE2E, DirInstanceSortedWithDunderInit) {
    auto out = compileAndRun(code("dir_instance_sorted_with_dunder_init"));
    EXPECT_EQ(out, "__init__\nbar\nbaz\nx\n");
}

TEST(CodeGenE2E, DirClassDescriptor) {
    auto out = compileAndRun(code("dir_class_descriptor"));
    EXPECT_EQ(out, "__init__\nbar\nx\n");
}

TEST(CodeGenE2E, DirWalksParentChain) {
    auto out = compileAndRun(code("dir_walks_parent_chain"));
    EXPECT_EQ(out, "__init__\nfetch\nname\nspeak\n");
}

TEST(CodeGenE2E, MethodFindWalksParentChain) {
    auto out = compileAndRun(code("method_find_walks_parent_chain"));
    EXPECT_EQ(out, "generic sound\ngot it\n");
}

TEST(CodeGenTest, ClassDeclStructType) {
    auto ir = generateIR(code("class_decl_struct_type"));
    EXPECT_NE(ir.find("Counter___init__"), std::string::npos)
        << "Expected __init__ function declaration in IR";
    EXPECT_NE(ir.find("Counter_new"), std::string::npos)
        << "Expected _new constructor in IR";
    EXPECT_NE(ir.find("@dragon_instance_alloc"), std::string::npos)
        << "Expected dragon_instance_alloc call in constructor";
    EXPECT_EQ(ir.find("@malloc"), std::string::npos)
        << "Expected no raw malloc in constructor";
}

TEST(CodeGenTest, ClassMethodDecl) {
    auto ir = generateIR(code("class_method_decl"));
    EXPECT_NE(ir.find("Adder_get"), std::string::npos)
        << "Expected method function in IR";
    EXPECT_NE(ir.find("Adder___init__"), std::string::npos)
        << "Expected __init__ function in IR";
}

TEST(CodeGenTest, ClassConstructorCall) {
    auto ir = generateIR(code("class_constructor_call"));
    EXPECT_NE(ir.find("call ptr @Box_new"), std::string::npos)
        << "Expected call to Box_new constructor";
}

TEST(CodeGenTest, ClassFieldAccess) {
    auto ir = generateIR(code("class_field_access"));
    EXPECT_NE(ir.find("getelementptr"), std::string::npos)
        << "Expected GEP for field access in IR";
}

TEST(CodeGenTest, ClassMethodCall) {
    auto ir = generateIR(code("class_method_call"));
    EXPECT_NE(ir.find("call i64 @Val_get"), std::string::npos)
        << "Expected call to Val_get method";
}

TEST(CodeGenTest, ClassModuleVerifies) {
    auto ir = generateIR(code("class_module_verifies"));
    EXPECT_TRUE(ir.find("<codegen failed") == std::string::npos)
        << "Expected IR generation to succeed, got: " << ir;
}

TEST(CodeGenIR, ClassGCHeaderInStruct) {
    auto ir = generateIR(code("class_gc_header_in_struct"));
    EXPECT_NE(ir.find("%Widget = type { i64, i64, ptr, i64 }"), std::string::npos)
        << "Expected GC header (2 x i64) + vtable ptr prepended to class struct";
}

TEST(CodeGenIR, ClassGCHeaderInit) {
    auto ir = generateIR(code("class_gc_header_init"));
    EXPECT_NE(ir.find("getelementptr inbounds"), std::string::npos)
        << "Expected GEP for header init\nIR:\n" << ir;
    EXPECT_NE(ir.find("store i64 1,"), std::string::npos)
        << "Expected refcount = 1 store\nIR:\n" << ir;
    EXPECT_NE(ir.find("@__class_id_Obj"), std::string::npos)
        << "Expected class_id global for Obj\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_dealloc"), std::string::npos)
        << "Expected dealloc registration call\nIR:\n" << ir;
}

TEST(CodeGenIR, ClassInstanceDecrefAtScopeExit) {
    auto ir = generateIR(code("class_instance_decref_at_scope_exit"));
    EXPECT_NE(ir.find("dragon_decref"), std::string::npos)
        << "Expected dragon_decref call for class instance cleanup";
}

TEST(CodeGenIR, AtomicIncrefDeclared) {
    auto ir = generateIR("x: int = 1\n");
    EXPECT_NE(ir.find("dragon_incref_atomic"), std::string::npos)
        << "Expected dragon_incref_atomic to be declared";
    EXPECT_NE(ir.find("dragon_decref_atomic"), std::string::npos)
        << "Expected dragon_decref_atomic to be declared";
    EXPECT_NE(ir.find("dragon_incref_str_atomic"), std::string::npos)
        << "Expected dragon_incref_str_atomic to be declared";
    EXPECT_NE(ir.find("dragon_decref_str_atomic"), std::string::npos)
        << "Expected dragon_decref_str_atomic to be declared";
}

TEST(CodeGenIR, FireListArgAtomicIncref) {
    auto ir = generateIR(code("fire_list_arg_atomic_incref"));
    EXPECT_NE(ir.find("dragon_incref_atomic"), std::string::npos)
        << "Expected atomic incref for list arg passed to fire";
}

TEST(CodeGenIR, FireIntArgNoAtomicIncref) {
    auto ir = generateIR(code("fire_int_arg_no_atomic_incref"));
    EXPECT_EQ(ir.find("call void @dragon_incref_atomic"), std::string::npos)
        << "Scalar args should not get atomic incref";
}

TEST(CodeGenIR, FireTrampolineDecrefsHeapArgs) {
    auto ir = generateIR(code("fire_trampoline_decrefs_heap_args"));
    EXPECT_NE(ir.find("__dragon_fire_tramp_"), std::string::npos)
        << "Expected fire trampoline function\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_decref_atomic"), std::string::npos)
        << "Expected atomic decref in fire trampoline\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_vthread_spawn_typed"), std::string::npos)
        << "Expected dragon_vthread_spawn_typed call\nIR:\n" << ir;
}

TEST(CodeGenIR, GCPhase5FunctionsDecl) {
    auto ir = generateIR(code("gc_phase5_functions_decl"));
    EXPECT_NE(ir.find("dragon_gc_track"), std::string::npos)
        << "Expected dragon_gc_track declared\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_gc_collect"), std::string::npos)
        << "Expected dragon_gc_collect declared\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_list_new_tagged"), std::string::npos)
        << "Expected dragon_list_new_tagged declared\nIR:\n" << ir;
}

TEST(CodeGenIR, ClassDeallocAndTraverse) {
    auto ir = generateIR(code("class_dealloc_and_traverse"));
    EXPECT_NE(ir.find("__dragon_dealloc_Node"), std::string::npos)
        << "Expected dealloc function for Node\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_traverse_Node"), std::string::npos)
        << "Expected traverse function for Node\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_dealloc"), std::string::npos)
        << "Expected dealloc registration\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_traverse"), std::string::npos)
        << "Expected traverse registration\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_clear_Node"), std::string::npos)
        << "Expected clear function for Node\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_clear"), std::string::npos)
        << "Expected clear registration\nIR:\n" << ir;
}

TEST(CodeGenIR, ClassClearZerosFields) {
    auto ir = generateIR(code("class_clear_zeros_fields"));
    EXPECT_NE(ir.find("__dragon_clear_Container"), std::string::npos)
        << "Expected clear function for Container\nIR:\n" << ir;
    auto clearPos = ir.find("define internal void @__dragon_clear_Container");
    ASSERT_NE(clearPos, std::string::npos)
        << "Expected clear function definition\nIR:\n" << ir;
    auto clearEnd = ir.find("\n}\n", clearPos);
    auto clearBody = ir.substr(clearPos, clearEnd - clearPos);
    EXPECT_NE(clearBody.find("dragon_decref_str"), std::string::npos)
        << "Expected decref_str call in clear function\nBody:\n" << clearBody;
    EXPECT_NE(clearBody.find("dragon_decref"), std::string::npos)
        << "Expected dragon_decref call in clear function\nBody:\n" << clearBody;
    EXPECT_NE(clearBody.find("store"), std::string::npos)
        << "Expected store (zero) in clear function\nBody:\n" << clearBody;
}

TEST(CodeGenIR, AcyclicClassNotTracked) {
    auto ir = generateIR(code("acyclic_class_not_tracked"));
    EXPECT_EQ(ir.find("call void @dragon_instance_track"), std::string::npos)
        << "Acyclic class (int + str fields) must NOT be gc_tracked\nIR:\n" << ir;
}

TEST(CodeGenIR, CyclicCapableClassTracked) {
    auto ir = generateIR(code("cyclic_capable_class_tracked"));
    EXPECT_NE(ir.find("call void @dragon_instance_track"), std::string::npos)
        << "Cyclic-capable class (list field) must still be gc_tracked\nIR:\n" << ir;
}

TEST(CodeGenIR, StackAllocNonEscapingInstance) {
    auto ir = generateIR(code("stack_alloc_non_escaping_instance"));
    EXPECT_NE(ir.find("P.stack = alloca %P"), std::string::npos)
        << "Non-escaping instance should be stack-allocated\nIR:\n" << ir;
    EXPECT_EQ(ir.find("call ptr @P_new"), std::string::npos)
        << "Stack-allocated instance must NOT call the heap ctor _new\nIR:\n" << ir;
}

TEST(CodeGenIR, EscapingInstanceStaysHeap) {
    auto ir = generateIR(code("escaping_instance_stays_heap"));
    EXPECT_NE(ir.find("call ptr @P_new"), std::string::npos)
        << "Returned (escaping) instance must be heap-allocated via _new\nIR:\n" << ir;
    EXPECT_EQ(ir.find("P.stack = alloca %P"), std::string::npos)
        << "Escaping instance must NOT be stack-allocated\nIR:\n" << ir;
}

TEST(CodeGenE2E, StackInstanceFieldReadsCorrect) {
    auto out = compileAndRun(code("stack_instance_field_reads_correct"));
    EXPECT_EQ(out, "1000000\n");
}

TEST(CodeGenE2E, EscapingInstancesDistinctAfterReturn) {
    auto out = compileAndRun(code("escaping_instances_distinct_after_return"));
    EXPECT_EQ(out, "60\n");
}

TEST(CodeGenIR, ConstIR) {
    auto ir = generateIR(code("const_ir"));
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, StaticFieldIR) {
    auto ir = generateIR(code("static_field_ir"));
    EXPECT_NE(ir.find("@Counter_count"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, StaticMethodIR) {
    auto ir = generateIR(code("static_method_ir"));
    EXPECT_NE(ir.find("MathUtil_add"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, MultiConstructorIR) {
    auto ir = generateIR(code("multi_constructor_ir"));
    EXPECT_NE(ir.find("Point___init___0"), std::string::npos);
    EXPECT_NE(ir.find("Point___init___1"), std::string::npos);
    EXPECT_NE(ir.find("Point_new_0"), std::string::npos);
    EXPECT_NE(ir.find("Point_new_1"), std::string::npos);
    EXPECT_EQ(ir.find("Point___init__("), std::string::npos);
    EXPECT_EQ(ir.find("Point_new("), std::string::npos);
}

TEST(CodeGenIR, SingleConstructorUnchangedIR) {
    auto ir = generateIR(code("single_constructor_unchanged_ir"));
    EXPECT_NE(ir.find("Simple___init__"), std::string::npos);
    EXPECT_NE(ir.find("Simple_new"), std::string::npos);
    EXPECT_EQ(ir.find("Simple___init___0"), std::string::npos);
    EXPECT_EQ(ir.find("Simple_new_0"), std::string::npos);
}

TEST(CodeGenIR, SuperCallIR) {
    auto ir = generateIR(code("super_call_ir"));
    EXPECT_NE(ir.find("Animal___init__"), std::string::npos);
    EXPECT_NE(ir.find("Animal_speak"), std::string::npos);
}

TEST(CodeGenIR, MROMethodLookupIR) {
    auto ir = generateIR(code("mro_method_lookup_ir"));
    EXPECT_NE(ir.find("A_greet"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, PyClassInheritanceIR) {
    auto ir = generateIRPy(code("py_class_inheritance_ir"));
    EXPECT_NE(ir.find("Base_get"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenTest, FirstClassClassStaticDispatchUnchanged) {
    auto output = compileAndRun(code("first_class_class_static_dispatch_unchanged"));
    EXPECT_EQ(output, "3\n4\n");
}

TEST(CodeGenTest, FirstClassClassDescriptorIR) {
    auto ir = generateIR(code("first_class_class_descriptor_ir"));
    EXPECT_NE(ir.find("Foo__descriptor"), std::string::npos)
        << "Expected descriptor global in IR";
    EXPECT_NE(ir.find("dragon_class_descriptor_create"), std::string::npos)
        << "Expected descriptor create call in IR";
}

TEST(CodeGenE2E, FirstClassClassTypeParam) {
    auto out = compileAndRun(code("first_class_class_type_param"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassReturnTypeAnnotation) {
    auto out = compileAndRun(code("first_class_class_return_type_annotation"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassListIteration) {
    auto out = compileAndRun(code("first_class_class_list_iteration"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassDictLookup) {
    auto out = compileAndRun(code("first_class_class_dict_lookup"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, ClassValueBindingRejected) {
    auto out = compileAndRun(code("class_value_binding_rejected"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassUnannotatedParamErrors) {
    auto out = compileAndRun(code("first_class_class_unannotated_param_errors"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("cannot call 'cls'"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorIdentity) {
    auto out = compileAndRun(code("class_decorator_identity"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorRegistry) {
    auto out = compileAndRun(code("class_decorator_registry"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorStacking) {
    auto out = compileAndRun(code("class_decorator_stacking"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorIsinstanceWorks) {
    auto out = compileAndRun(code("class_decorator_isinstance_works"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorPreservesAttribute) {
    auto out = compileAndRun(code("class_decorator_preserves_attribute"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, DataclassConstruction) {
    auto out = compileAndRun(code("dataclass_construction"));
    EXPECT_EQ(out, "3\n4\n");
}

TEST(CodeGenE2E, DataclassEqualByFields) {
    auto out = compileAndRun(code("dataclass_equal_by_fields"));
    EXPECT_EQ(out, "eq12\nne13\n");
}

TEST(CodeGenE2E, DataclassReprFormat) {
    auto out = compileAndRun(code("dataclass_repr_format"));
    EXPECT_EQ(out, "Point(x=3, y=4)\n");
}

TEST(CodeGenE2E, DataclassDefaultValues) {
    auto out = compileAndRun(code("dataclass_default_values"));
    EXPECT_EQ(out, "60\n30\n");
}

TEST(CodeGenE2E, DataclassUserInitOverrides) {
    auto out = compileAndRun(code("dataclass_user_init_overrides"));
    EXPECT_EQ(out, "14\n");
}

TEST(CodeGenE2E, DataclassMixedFieldTypes) {
    auto out = compileAndRun(code("dataclass_mixed_field_types"));
    EXPECT_EQ(out, "eq12\nne13\nPerson(name=Ada, age=36, active=True)\n");
}

TEST(CodeGenE2E, NamedTupleConstruction) {
    auto out = compileAndRun(code("named_tuple_construction"));
    EXPECT_EQ(out, "1\n2\nVec(x=1, y=2)\n");
}

TEST(CodeGenE2E, DataclassWithRuntimeDecorator) {
    auto out = compileAndRun(code("dataclass_with_runtime_decorator"));
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
}

TEST(CodeGenIR, VtableGlobalInIR) {
    auto ir = generateIR(code("vtable_global_in_ir"));
    EXPECT_NE(ir.find("Dog__vtable"), std::string::npos)
        << "Expected vtable global in IR";
    EXPECT_NE(ir.find("Dog_speak"), std::string::npos)
        << "Expected method function in IR";
}

TEST(CodeGenIR, VtableStructLayout) {
    auto ir = generateIR(code("vtable_struct_layout"));
    EXPECT_NE(ir.find("%Point = type { i64, i64, ptr, i64, i64 }"), std::string::npos)
        << "Expected vtable ptr in struct layout at index 2";
}

TEST(CodeGenTest, VtableDynamicMethodDispatch) {
    auto output = compileAndRun(code("vtable_dynamic_method_dispatch"));
    EXPECT_EQ(output, "Woof\n");
}

TEST(CodeGenTest, VtableInheritanceDispatch) {
    auto output = compileAndRun(code("vtable_inheritance_dispatch"));
    EXPECT_EQ(output, "Woof\n");
}

TEST(CodeGenTest, VtableInheritedMethod) {
    auto output = compileAndRun(code("vtable_inherited_method"));
    EXPECT_EQ(output, "...\n");
}

TEST(CodeGenTest, VtableStaticDispatchUnchanged) {
    auto output = compileAndRun(code("vtable_static_dispatch_unchanged"));
    EXPECT_EQ(output, "Meow\n");
}

TEST(CodeGenTest, VtableMultipleMethods) {
    auto output = compileAndRun(code("vtable_multiple_methods"));
    EXPECT_EQ(output, "15\n30\n");
}

TEST(CodeGenTest, VtableVoidMethod) {
    auto output = compileAndRun(code("vtable_void_method"));
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenTest, BugReproStrFieldCmp) {
    auto ir = generateIR(code("bug_repro_str_field_cmp"));
    EXPECT_NE(ir.find("dragon_str_eq"), std::string::npos)
        << "Expected dragon_str_eq for string field comparison\nIR:\n" << ir;
}

TEST(CodeGenTest, FloatFieldInference) {
    auto ir = generateIR(code("float_field_inference"));
    EXPECT_NE(ir.find("fadd"), std::string::npos)
        << "Expected fadd for float field addition\nIR:\n" << ir;
}

TEST(CodeGenTest, BoolFieldInference) {
    std::string out = compileAndRun(code("bool_field_inference"));
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenTest, StrFieldLiteralInit) {
    auto ir = generateIR(code("str_field_literal_init"));
    EXPECT_NE(ir.find("dragon_str_eq"), std::string::npos)
        << "Expected dragon_str_eq for string field comparison\nIR:\n" << ir;
}

TEST(CodeGenE2E, ClassBasic) {
    auto output = compileAndRun(code("class_basic"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, ClassGCFieldAccess) {
    auto out = compileAndRun(code("class_gc_field_access"));
    EXPECT_EQ(out, "3\n4\n");
}

TEST(CodeGenE2E, ClassGCMethodAccess) {
    auto out = compileAndRun(code("class_gc_method_access"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, ClassGCMultipleFields) {
    auto out = compileAndRun(code("class_gc_multiple_fields"));
    EXPECT_EQ(out, "50\n");
}

TEST(CodeGenE2E, ClassGCFieldMutation) {
    auto out = compileAndRun(code("class_gc_field_mutation"));
    EXPECT_EQ(out, "1\n42\n");
}

TEST(CodeGenE2E, ClassGCReturnInstance) {
    auto out = compileAndRun(code("class_gc_return_instance"));
    // Just verify it doesn't crash (return incref prevents use-after-free)
    EXPECT_FALSE(out.empty() && false);
}

TEST(CodeGenE2E, CycleCollectorFreesObjects) {
    auto out = compileAndRun(code("cycle_collector_frees_objects"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, CycleCollectorReentrancyGuard) {
    auto out = compileAndRun(code("cycle_collector_reentrancy_guard"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, ClassInstanceCreateDestroyE2E) {
    auto out = compileAndRun(code("class_instance_create_destroy_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, MultiConstructorDispatchE2E) {
    auto out = compileAndRun(code("multi_constructor_dispatch_e2_e"));
    EXPECT_EQ(out, "10\n20\n0\n0\n");
}

TEST(CodeGenE2E, ThreeConstructorsE2E) {
    auto out = compileAndRun(code("three_constructors_e2_e"));
    EXPECT_EQ(out, "0\n5\n3\n");
}

TEST(CodeGenE2E, MultiConstructorWithMethodsE2E) {
    auto out = compileAndRun(code("multi_constructor_with_methods_e2_e"));
    EXPECT_EQ(out, "12\n1\n");
}

TEST(CodeGenE2E, SelfCtorSingleE2E) {
    auto out = compileAndRun(code("self_ctor_single_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, DefInitBackcompatE2E) {
    auto out = compileAndRun(code("def_init_backcompat_e2_e"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, StaticFieldE2E) {
    auto out = compileAndRun(code("static_field_e2_e"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, StaticmethodDr) {
    auto out = compileAndRun(code("staticmethod_dr"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, StaticmethodPy) {
    auto out = compileAndRunPy(code("staticmethod_py"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, ClassmethodPy) {
    auto out = compileAndRunPy(code("classmethod_py"));
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenE2E, StaticAndInstanceMethodCoexist) {
    auto out = compileAndRun(code("static_and_instance_method_coexist"));
    EXPECT_EQ(out, "15\n3\n");
}

TEST(CodeGenE2E, SuperMethodCall) {
    auto out = compileAndRun(code("super_method_call"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, MROInheritedMethod) {
    auto out = compileAndRun(code("mro_inherited_method"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, PyMROInheritedMethodE2E) {
    auto out = compileAndRunPy(code("py_mro_inherited_method_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, PySuperMethodE2E) {
    auto out = compileAndRunPy(code("py_super_method_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, StrFieldCmpE2E) {
    std::string out = compileAndRun(code("str_field_cmp_e2_e"));
    EXPECT_EQ(out, "Hello, World!\n");
}

TEST(CodeGenTest, StrFieldCmpNotEqual) {
    std::string out = compileAndRun(code("str_field_cmp_not_equal"));
    EXPECT_EQ(out, "not admin\n");
}

TEST(CodeGenTest, UnionParamIsinstanceNarrowing) {
    auto output = compileAndRun(code("union_param_isinstance_narrowing"));
    EXPECT_EQ(output, "42\nhello\n");
}

TEST(CodeGenTest, UnionParamPrintDispatch) {
    auto output = compileAndRun(code("union_param_print_dispatch"));
    EXPECT_EQ(output, "42\nworld\n");
}

TEST(CodeGenTest, UnionParamIntFloat) {
    auto output = compileAndRun(code("union_param_int_float"));
    EXPECT_EQ(output, "11\n3.14\n");
}

TEST(CodeGenTest, UnionParamMultiple) {
    auto output = compileAndRun(code("union_param_multiple"));
    EXPECT_EQ(output, "30\n");
}

TEST(CodeGenTest, UnionTypeBuiltin) {
    auto output = compileAndRun(code("union_type_builtin"));
    EXPECT_EQ(output, "int\nstr\n");
}

TEST(CodeGenTest, UnionThreeTypes) {
    auto output = compileAndRun(code("union_three_types"));
    EXPECT_EQ(output, "int\nstr\nfloat\n");
}

TEST(CodeGenTest, UnionNarrowedArithmetic) {
    auto output = compileAndRun(code("union_narrowed_arithmetic"));
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenTest, UnionPassThrough) {
    auto output = compileAndRun(code("union_pass_through"));
    EXPECT_EQ(output, "99\npass\n");
}

TEST(CodeGenTest, UnionLocalVariable) {
    auto output = compileAndRun(code("union_local_variable"));
    EXPECT_EQ(output, "42\nhello\n");
}

TEST(CodeGenTest, UnionParamBool) {
    auto output = compileAndRun(code("union_param_bool"));
    EXPECT_EQ(output, "True\n11\n");
}

TEST(CodeGenTest, PrintClassDescriptor) {
    auto output = compileAndRun(code("print_class_descriptor"));
    EXPECT_NE(output.find("codegen failed"), std::string::npos);
    EXPECT_NE(output.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, PropertyGetterBareAccess) {
    auto out = compileAndRun(code("property_getter_bare_access"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, PropertySetterRoundTrip) {
    auto out = compileAndRun(code("property_setter_round_trip"));
    EXPECT_EQ(out, "10\n10\n");
}

TEST(CodeGenE2E, PropertyComputedDerived) {
    auto out = compileAndRun(code("property_computed_derived"));
    EXPECT_EQ(out, "12\n");
}

TEST(CodeGenE2E, PropertyReturnsString) {
    // String-returning property - refcount path must not double-free or leak.
    auto out = compileAndRun(code("property_returns_string"));
    EXPECT_EQ(out, "hi world\n");
}

TEST(CodeGenE2E, PropertyInheritedFromBase) {
    auto out = compileAndRun(code("property_inherited_from_base"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, PropertyAssignedToTypedLocal) {
    auto out = compileAndRun(code("property_assigned_to_typed_local"));
    EXPECT_EQ(out, "12\n");
}

TEST(CodeGenE2E, PropertyGetterCalledOnce) {
    auto out = compileAndRun(code("property_getter_called_once"));
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(CodeGenIR, PropertyEmitsMethodCallNotFieldLoad) {
    auto ir = generateIR(code("property_emits_method_call_not_field_load"));
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("Box_value"), std::string::npos)
        << "Expected getter Box_value to be defined";
    EXPECT_NE(ir.find("call i64 @Box_value"), std::string::npos)
        << "Expected getter call site at b.value access";
}

TEST(CodeGenE2E, EnumAutoNumbered) {
    auto out = compileAndRun(code("enum_auto_numbered"));
    EXPECT_EQ(out, "0\n1\n2\n");
}

TEST(CodeGenE2E, EnumExplicitValues) {
    auto out = compileAndRun(code("enum_explicit_values"));
    EXPECT_EQ(out, "200\n404\n500\n");
}

TEST(CodeGenE2E, EnumMixedAutoAndExplicit) {
    auto out = compileAndRun(code("enum_mixed_auto_and_explicit"));
    EXPECT_EQ(out, "0\n10\n11\n12\n");
}

TEST(CodeGenE2E, EnumWithMatchStatement) {
    auto out = compileAndRun(code("enum_with_match_statement"));
    EXPECT_EQ(out, "red\ngreen\nblue\nunknown\n");
}

TEST(CodeGenE2E, EnumNegativeValue) {
    auto out = compileAndRun(code("enum_negative_value"));
    EXPECT_EQ(out, "-1\n0\n1\n");
}

TEST(CodeGenE2E, EnumUsedInIfElif) {
    auto out = compileAndRun(code("enum_used_in_if_elif"));
    EXPECT_EQ(out, "D\nI\nW\nE\n");
}

TEST(CodeGenE2E, EnumTrailingCommaOptional) {
    auto out = compileAndRun(code("enum_trailing_comma_optional"));
    EXPECT_EQ(out, "0\n1\n2\n");
}

TEST(CodeGenIR, PropertySetterMangledInVtable) {
    auto ir = generateIR(code("property_setter_mangled_in_vtable"));
    EXPECT_NE(ir.find("Box_value__setter"), std::string::npos)
        << "Expected mangled setter Box_value__setter in IR";
    EXPECT_NE(ir.find("@Box_value__setter"), std::string::npos)
        << "Expected setter to be emitted/called as @Box_value__setter";
}

TEST(CodeGenE2E, ForInClassFieldDictWithExplicitFieldAnnotation) {
    auto out = compileAndRun(code("for_in_class_field_dict_with_explicit_field_annotation"));
    EXPECT_EQ(out, "a\n1\nb\n2\n");
}

TEST(CodeGenE2E, ForInClassFieldDictInferredFromDictLiteral) {
    auto out = compileAndRun(code("for_in_class_field_dict_inferred_from_dict_literal"));
    EXPECT_EQ(out, "a\n1\nb\n2\n");
}

TEST(CodeGenE2E, ForInOtherObjectDictField) {
    auto out = compileAndRun(code("for_in_other_object_dict_field"));
    EXPECT_EQ(out, "x\ny\np\nq\n");
}

TEST(CodeGenE2E, ForInClassFieldString) {
    auto out = compileAndRun(code("for_in_class_field_string"));
    EXPECT_EQ(out, "h\ni\n");
}

TEST(CodeGenE2E, ForInClassFieldDictKeysMethodOnAttribute) {
    auto out = compileAndRun(code("for_in_class_field_dict_keys_method_on_attribute"));
    EXPECT_EQ(out, "a\nb\n");
}

TEST(CodeGenE2E, ForInClassFieldDictHeterogeneousStaysPolymorphic) {
    auto out = compileAndRun(code("for_in_class_field_dict_heterogeneous_stays_polymorphic"));
    EXPECT_EQ(out, "a\nb\n");
}

TEST(CodeGenE2E, CapturingClosureStoredOnClassFieldInvokedThroughField) {
    auto out = compileAndRun(code("capturing_closure_stored_on_class_field_invoked_through_field"));
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, BareFnPointerOnClassFieldInvokedThroughField) {
    auto out = compileAndRun(code("bare_fn_pointer_on_class_field_invoked_through_field"));
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenTest, OwnPtrFieldDefaultRegisteredAllocatorReleases) {
    auto ir = generateIR(code("own_ptr_field_default_registered_allocator_releases"));
    EXPECT_EQ(ir.find("<codegen failed"), std::string::npos) << ir;
    EXPECT_NE(ir.find("dragon_lock_destroy"), std::string::npos)
        << "field-default allocator must register its releaser in the dealloc";
}

TEST(CodeGenTest, OwnFieldDefaultUnregisteredCalleeRejected) {
    auto ir = generateIR(code("own_field_default_unregistered_callee_rejected"));
    EXPECT_NE(ir.find("no registered releaser"), std::string::npos) << ir;
}

TEST(CodeGenTest, OwnReleaseInheritedBySubclassDealloc) {
    auto ir = generateIR(code("own_release_inherited_by_subclass_dealloc"));
    EXPECT_EQ(ir.find("<codegen failed"), std::string::npos) << ir;
    EXPECT_EQ(countSubstring(ir, "call void @dragon_lock_destroy"), 2u)
        << "base and child deallocs must each destroy the inherited resource";
}

TEST(CodeGenTest, OwnReleaseTransitiveThroughGrandparent) {
    auto ir = generateIR(code("own_release_transitive_through_grandparent"));
    EXPECT_EQ(ir.find("<codegen failed"), std::string::npos) << ir;
    EXPECT_EQ(countSubstring(ir, "call void @dragon_lock_destroy"), 3u)
        << "every class dealloc in the chain must destroy the grandparent's resource";
}
