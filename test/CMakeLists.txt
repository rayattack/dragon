include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include_directories(${CMAKE_SOURCE_DIR}/include)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_compile_definitions(
    DRAGON_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    DRAGON_TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases")

add_executable(dragon_zen_tests ZenTest.cpp)
target_link_libraries(dragon_zen_tests PRIVATE gtest_main)
add_test(NAME ZenTests COMMAND dragon_zen_tests)

add_executable(dragon_lexer_tests LexerTest.cpp)
target_link_libraries(dragon_lexer_tests PRIVATE dragon_lib gtest_main)
add_test(NAME LexerTests COMMAND dragon_lexer_tests)

add_executable(dragon_parser_tests ParserTest.cpp)
target_link_libraries(dragon_parser_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ParserTests COMMAND dragon_parser_tests)

add_executable(dragon_sema_tests SemaTest.cpp)
target_link_libraries(dragon_sema_tests PRIVATE dragon_lib gtest_main)
add_test(NAME SemaTests COMMAND dragon_sema_tests)

add_executable(dragon_ffisync_tests FfiSyncTest.cpp)
target_link_libraries(dragon_ffisync_tests PRIVATE dragon_lib gtest_main)
add_test(NAME FfiSyncTests COMMAND dragon_ffisync_tests)

add_executable(dragon_definite_assignment_tests DefiniteAssignmentTest.cpp)
target_link_libraries(dragon_definite_assignment_tests PRIVATE dragon_lib gtest_main)
add_test(NAME DefiniteAssignmentTests COMMAND dragon_definite_assignment_tests)

add_executable(dragon_module_init_order_tests ModuleInitOrderTest.cpp)
target_link_libraries(dragon_module_init_order_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ModuleInitOrderTests COMMAND dragon_module_init_order_tests)

add_executable(dragon_ownership_tests OwnershipCheckTest.cpp)
target_link_libraries(dragon_ownership_tests PRIVATE dragon_lib gtest_main)
add_test(NAME OwnershipTests COMMAND dragon_ownership_tests)

add_executable(dragon_typechecker_tests TypeCheckerTest.cpp)
target_link_libraries(dragon_typechecker_tests PRIVATE dragon_lib gtest_main)
add_test(NAME TypeCheckerTests COMMAND dragon_typechecker_tests)

add_executable(dragon_ast_tests ASTTest.cpp)
target_link_libraries(dragon_ast_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ASTTests COMMAND dragon_ast_tests)

add_executable(dragon_enforcer_tests TypeHintEnforcerTest.cpp)
target_link_libraries(dragon_enforcer_tests PRIVATE dragon_lib gtest_main)
add_test(NAME EnforcerTests COMMAND dragon_enforcer_tests)

add_executable(dragon_resolver_tests ModuleResolverTest.cpp)
target_link_libraries(dragon_resolver_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ResolverTests COMMAND dragon_resolver_tests)

add_executable(dragon_diagnostic_tests DiagnosticTest.cpp)
target_link_libraries(dragon_diagnostic_tests PRIVATE dragon_lib gtest_main)
add_test(NAME DiagnosticTests COMMAND dragon_diagnostic_tests)

add_executable(dragon_migrator_tests MigratorTest.cpp)
target_link_libraries(dragon_migrator_tests PRIVATE dragon_lib gtest_main)
add_test(NAME MigratorTests COMMAND dragon_migrator_tests)

add_executable(dragon_interop_tests InteropTest.cpp)
target_compile_definitions(dragon_interop_tests PRIVATE
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    DRAGON_RUNTIME_LIB="$<TARGET_FILE:dragon_runtime>"
    DRAGON_SQLITE3_LIB="$<TARGET_FILE:dragon_sqlite3>"
    DRAGON_PCRE2_LIB="$<TARGET_FILE:pcre2-8-static>"
    DRAGON_LLHTTP_LIB="$<TARGET_FILE:dragon_llhttp>"
    DRAGON_MBEDTLS_LIB="$<TARGET_FILE:dragon_mbedtls>"
    DRAGON_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib")
target_link_libraries(dragon_interop_tests PRIVATE dragon_lib gtest_main)
add_test(NAME InteropTests COMMAND dragon_interop_tests)

add_executable(dragon_codegen_tests
    CodeGenLiteralsTest.cpp
    CodeGenExpressionsTest.cpp
    CodeGenAssignTest.cpp
    CodeGenCallsTest.cpp
    CodeGenCollectionsTest.cpp
    CodeGenComprehensionsTest.cpp
    CodeGenFunctionsTest.cpp
    CodeGenClassesTest.cpp
    CodeGenDundersTest.cpp
    CodeGenDocstringTest.cpp
    CodeGenControlFlowTest.cpp
    CodeGenExceptionsTest.cpp
    CodeGenConcurrencyTest.cpp
    CodeGenTemplateTest.cpp
    CodeGenModuleTest.cpp
    CodeGenGenericsTest.cpp
)
target_compile_definitions(dragon_codegen_tests PRIVATE
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    DRAGON_RUNTIME_LIB="$<TARGET_FILE:dragon_runtime>"
    DRAGON_LLHTTP_LIB="$<TARGET_FILE:dragon_llhttp>"
    DRAGON_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib")
target_link_libraries(dragon_codegen_tests PRIVATE dragon_lib gtest_main)
add_test(NAME CodeGenTests COMMAND dragon_codegen_tests)

add_executable(dragon_repl_tests
    ReplJitTest.cpp
    ReplOwnershipTest.cpp
    ReplEchoTest.cpp
    ReplImportTest.cpp
)
target_compile_definitions(dragon_repl_tests PRIVATE
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    DRAGON_RUNTIME_LIB="$<TARGET_FILE:dragon_runtime>"
    DRAGON_SQLITE3_LIB="$<TARGET_FILE:dragon_sqlite3>"
    DRAGON_PCRE2_LIB="$<TARGET_FILE:pcre2-8-static>"
    DRAGON_LLHTTP_LIB="$<TARGET_FILE:dragon_llhttp>"
    DRAGON_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib")
if(DRAGON_ASAN)
    target_link_libraries(dragon_repl_tests PRIVATE
        dragon_lib dragon_runtime_asan dragon_sqlite3 gtest_main)
    target_link_options(dragon_repl_tests PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:dragon_runtime_asan>,--no-whole-archive"
        ${DRAGON_EXPORT_DYNAMIC} -fsanitize=address)
else()
    target_link_libraries(dragon_repl_tests PRIVATE
        dragon_lib dragon_runtime dragon_sqlite3 gtest_main)
    target_link_options(dragon_repl_tests PRIVATE
        ${DRAGON_RUNTIME_WHOLE} ${DRAGON_EXPORT_DYNAMIC})
endif()
add_test(NAME ReplTests COMMAND dragon_repl_tests)

file(GLOB DR_TEST_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_*.dr)
foreach(_dr ${DR_TEST_FILES})
    get_filename_component(_name ${_dr} NAME_WE)
    add_test(NAME dr_${_name} COMMAND $<TARGET_FILE:dragon> run ${_dr})
endforeach()
set_tests_properties(dr_test_green_parking PROPERTIES TIMEOUT 120)
add_test(NAME dr_test_simd_release
    COMMAND $<TARGET_FILE:dragon> run -O3 -mcpu=native ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_simd.dr)

# Linux-only: macOS accepts but does not enforce RLIMIT_AS, and it lazily grants
# huge reservations Linux overcommit refuses, so no oom probe can fail there.
if(NOT APPLE)
    add_test(NAME dr_oom_sustained
        COMMAND sh -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_sustained.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_sustained && ulimit -v 262144 && MALLOC_CONF=retain:false exec ${CMAKE_CURRENT_BINARY_DIR}/oom_sustained")
    set_tests_properties(dr_oom_sustained PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_bytes
        COMMAND sh -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_bytes.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_bytes && ulimit -v 262144 && MALLOC_CONF=retain:false exec ${CMAKE_CURRENT_BINARY_DIR}/oom_bytes")
    set_tests_properties(dr_oom_bytes PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_containers
        COMMAND sh -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_containers.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_containers && ulimit -v 262144 && MALLOC_CONF=retain:false exec ${CMAKE_CURRENT_BINARY_DIR}/oom_containers")
    set_tests_properties(dr_oom_containers PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_unservable
        COMMAND sh -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_unservable.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_unservable && ulimit -v 262144 && MALLOC_CONF=retain:false exec ${CMAKE_CURRENT_BINARY_DIR}/oom_unservable")
    set_tests_properties(dr_oom_unservable PROPERTIES TIMEOUT 90)

    foreach(_grow set deque)
        add_test(NAME dr_oom_${_grow}_grow
            COMMAND sh -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_${_grow}_grow.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_${_grow}_grow && ( ulimit -v 32768 && ${CMAKE_CURRENT_BINARY_DIR}/oom_${_grow}_grow 2>&1 ; echo \"rc=$?\" ) | grep -q 'rc=134'")
        set_tests_properties(dr_oom_${_grow}_grow PROPERTIES TIMEOUT 90)
    endforeach()
endif()

file(GLOB_RECURSE DRAGON_STDLIB_MODULES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/stdlib/*.dr)
set(_stdlib_imports "# Generated by test/CMakeLists.txt - do not edit.\n")
foreach(_mod ${DRAGON_STDLIB_MODULES})
    file(RELATIVE_PATH _rel ${CMAKE_SOURCE_DIR}/stdlib ${_mod})
    string(REGEX REPLACE "\\.dr$" "" _rel ${_rel})
    string(REPLACE "/" "." _rel ${_rel})
    string(APPEND _stdlib_imports "import ${_rel}\n")
endforeach()
string(APPEND _stdlib_imports "\nprint(\"all stdlib modules resolved\")\n")
set(DRAGON_STDLIB_IMPORTS_DR ${CMAKE_CURRENT_BINARY_DIR}/stdlib_imports.dr)
file(GENERATE OUTPUT ${DRAGON_STDLIB_IMPORTS_DR} CONTENT "${_stdlib_imports}")
add_test(NAME dr_stdlib_imports
    COMMAND $<TARGET_FILE:dragon> check ${DRAGON_STDLIB_IMPORTS_DR})

add_test(NAME dr_ffi_sync_check
    COMMAND $<TARGET_FILE:dragon> ffi sync
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_ffi_python.dr --check)

foreach(_guard decode_dict_class_guard decode_dict_intkey_guard)
    add_test(NAME dr_${_guard}
        COMMAND $<TARGET_FILE:dragon> check ${CMAKE_CURRENT_SOURCE_DIR}/dr/${_guard}.dr)
    set_tests_properties(dr_${_guard} PROPERTIES
        PASS_REGULAR_EXPRESSION "never falls back to a boxed type on its own")
endforeach()

foreach(_guard import_missing_guard from_import_missing_guard)
    add_test(NAME dr_${_guard}
        COMMAND $<TARGET_FILE:dragon> check ${CMAKE_CURRENT_SOURCE_DIR}/dr/${_guard}.dr)
    set_tests_properties(dr_${_guard} PROPERTIES
        PASS_REGULAR_EXPRESSION "cannot find module 'zzz_no_such_module'")
endforeach()

add_test(NAME dr_reactive_local_guard
    COMMAND $<TARGET_FILE:dragon> check ${CMAKE_CURRENT_SOURCE_DIR}/dr/reactive_local_guard.dr)
set_tests_properties(dr_reactive_local_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "may reference only module-global names")

add_test(NAME dr_reactive_global_guard
    COMMAND $<TARGET_FILE:dragon> run ${CMAKE_CURRENT_SOURCE_DIR}/dr/reactive_global_guard.dr)
set_tests_properties(dr_reactive_global_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "reactive global ok")

add_test(NAME dr_alias_import_guard
    COMMAND $<TARGET_FILE:dragon> run ${CMAKE_CURRENT_SOURCE_DIR}/dr/alias_import_guard.dr)
set_tests_properties(dr_alias_import_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "alias import ok")

add_test(NAME dr_recursive_alias_guard
    COMMAND $<TARGET_FILE:dragon> run ${CMAKE_CURRENT_SOURCE_DIR}/dr/recursive_alias_guard.dr)
set_tests_properties(dr_recursive_alias_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "recursive alias ok")

add_test(NAME dr_union_container_guard
    COMMAND $<TARGET_FILE:dragon> run ${CMAKE_CURRENT_SOURCE_DIR}/dr/union_container_guard.dr)
set_tests_properties(dr_union_container_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "union containers ok"
    FAIL_REGULAR_EXPRESSION "FAIL:")

add_test(NAME dr_ctor_arity_guard
    COMMAND $<TARGET_FILE:dragon> check ${CMAKE_CURRENT_SOURCE_DIR}/dr/ctor_arity_guard.dr)
set_tests_properties(dr_ctor_arity_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "no constructor overload of class 'Pair'")

add_test(NAME dr_test_generics_py
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/generics_py/test_generics_py.py)

add_test(NAME dr_ffi_cxx_shim
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/ffi_cxx/app.dr
        --cc-source ${CMAKE_CURRENT_SOURCE_DIR}/dr/ffi_cxx/shim.cpp)

# Not auto-globbed with dr/test_*.dr: `import ui` links a webview shell, and this
# suite substitutes a headless one that records the patch JS instead of running
# GTK, so it must be built with its own --cc-source.
add_test(NAME dr_ui_binders
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/ui_binders/app.dr
        --cc-source ${CMAKE_CURRENT_SOURCE_DIR}/dr/ui_binders/webview_probe.cpp)

set(DRAGON_INCLUDE_PROBE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include_probe)
file(GENERATE OUTPUT ${DRAGON_INCLUDE_PROBE_DIR}/inc/helper.dr
    CONTENT "def helped() -> int { return 41 }\n")
file(GENERATE OUTPUT ${DRAGON_INCLUDE_PROBE_DIR}/attached_include.dr
    CONTENT "from helper import helped\nprint(helped())\n")
add_test(NAME dr_attached_include_guard
    COMMAND $<TARGET_FILE:dragon> check
        ${DRAGON_INCLUDE_PROBE_DIR}/attached_include.dr
        -I${DRAGON_INCLUDE_PROBE_DIR}/inc)

file(GENERATE OUTPUT ${DRAGON_INCLUDE_PROBE_DIR}/shadow/sys/types.h
    CONTENT "struct dragon_probe { int x; };\n")
file(GENERATE OUTPUT ${DRAGON_INCLUDE_PROBE_DIR}/shadowed_stdlib.dr
    CONTENT "import sys\nprint(\"stdlib module still resolves\")\n")
add_test(NAME dr_shadowed_stdlib_package_guard
    COMMAND $<TARGET_FILE:dragon> check
        ${DRAGON_INCLUDE_PROBE_DIR}/shadowed_stdlib.dr
        -I ${DRAGON_INCLUDE_PROBE_DIR}/shadow)

add_test(NAME dr_unknown_option_guard
    COMMAND $<TARGET_FILE:dragon> check
        ${DRAGON_INCLUDE_PROBE_DIR}/shadowed_stdlib.dr --no-such-option)
set_tests_properties(dr_unknown_option_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "Unknown option: --no-such-option")

add_test(NAME dr_dump_tokens_on_run_guard
    COMMAND $<TARGET_FILE:dragon> run
        ${DRAGON_INCLUDE_PROBE_DIR}/shadowed_stdlib.dr --dump-tokens)
set_tests_properties(dr_dump_tokens_on_run_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "=== Tokens ===")

add_test(NAME dr_unimplemented_egg_verb_guard
    COMMAND $<TARGET_FILE:dragon> bump)
set_tests_properties(dr_unimplemented_egg_verb_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "Unknown command: bump")

add_test(NAME dr_unknown_gc_mode_guard
    COMMAND $<TARGET_FILE:dragon> check
        ${DRAGON_INCLUDE_PROBE_DIR}/shadowed_stdlib.dr --gc=bogus)
set_tests_properties(dr_unknown_gc_mode_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "Unknown --gc mode: 'bogus'")

add_test(NAME dr_help_lists_release_and_gc_none
    COMMAND $<TARGET_FILE:dragon> --help)
set_tests_properties(dr_help_lists_release_and_gc_none PROPERTIES
    PASS_REGULAR_EXPRESSION "--release")

file(GENERATE OUTPUT ${DRAGON_INCLUDE_PROBE_DIR}/untyped_param.dr
    CONTENT "def add(a, b) -> int {\n    return a + b\n}\nprint(add(1, 2))\n")
add_test(NAME dr_untyped_param_guard
    COMMAND $<TARGET_FILE:dragon> check
        ${DRAGON_INCLUDE_PROBE_DIR}/untyped_param.dr)
set_tests_properties(dr_untyped_param_guard PROPERTIES
    PASS_REGULAR_EXPRESSION "missing type annotation for parameter 'a' in function 'add'")

file(GLOB DRAGON_EXAMPLE_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/examples/*.dr
    ${CMAKE_SOURCE_DIR}/examples/docs_site/*.dr)
set(DRAGON_EXAMPLE_SKIP asyncs.dr syntax.dr server.dr)
foreach(_ex ${DRAGON_EXAMPLE_FILES})
    get_filename_component(_ex_file ${_ex} NAME)
    if(_ex_file IN_LIST DRAGON_EXAMPLE_SKIP)
        continue()
    endif()
    get_filename_component(_ex_name ${_ex} NAME_WE)
    add_test(NAME example_check_${_ex_name}
        COMMAND $<TARGET_FILE:dragon> check ${_ex})
    set_tests_properties(example_check_${_ex_name} PROPERTIES TIMEOUT 90)
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME docs_samples_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_doc_samples.py
        $<TARGET_FILE:dragon>
        ${CMAKE_SOURCE_DIR}/dragonlang-org/content/docs)
set_tests_properties(docs_samples_check PROPERTIES TIMEOUT 900)

get_property(_dr_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t ${_dr_all_tests})
    if(_t MATCHES "^dr_")
        get_test_property(${_t} TIMEOUT _dr_timeout)
        if(NOT _dr_timeout)
            set_tests_properties(${_t} PROPERTIES TIMEOUT 90)
        endif()
    endif()
endforeach()

foreach(_t
    dragon_lexer_tests dragon_parser_tests dragon_sema_tests dragon_ffisync_tests
    dragon_definite_assignment_tests dragon_module_init_order_tests
    dragon_ownership_tests dragon_typechecker_tests dragon_ast_tests
    dragon_enforcer_tests dragon_resolver_tests dragon_diagnostic_tests
    dragon_migrator_tests dragon_interop_tests dragon_codegen_tests)
    target_compile_options(${_t} PRIVATE ${DRAGON_WARNINGS})
endforeach()

add_test(NAME dragonsp
    COMMAND $<TARGET_FILE:dragon> run ${CMAKE_SOURCE_DIR}/tools/dragonsp/test_dragonsp.dr)
set_tests_properties(dragonsp PROPERTIES
    ENVIRONMENT "DRAGONSP_BIN=${DRAGONSP_OUT};DRAGONSP_COMPILER=${CMAKE_BINARY_DIR}/dragon${CMAKE_EXECUTABLE_SUFFIX};DRAGONSP_STDLIB=${CMAKE_SOURCE_DIR}/stdlib"
    TIMEOUT 300)
