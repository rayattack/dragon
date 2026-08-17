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

file(GLOB DR_TEST_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_*.dr)
foreach(_dr ${DR_TEST_FILES})
    get_filename_component(_name ${_dr} NAME_WE)
    add_test(NAME dr_${_name} COMMAND $<TARGET_FILE:dragon> run ${_dr}
        -I ${CMAKE_SOURCE_DIR}/packages)
endforeach()

# Linux-only: macOS accepts but does not enforce RLIMIT_AS, and it lazily grants
# huge reservations Linux overcommit refuses, so no oom probe can fail there.
if(NOT APPLE)
    add_test(NAME dr_oom_sustained
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_sustained.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_sustained && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_sustained")
    set_tests_properties(dr_oom_sustained PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_bytes
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_bytes.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_bytes && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_bytes")
    set_tests_properties(dr_oom_bytes PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_containers
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_containers.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_containers && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_containers")
    set_tests_properties(dr_oom_containers PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_unservable
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_unservable.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_unservable && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_unservable")
    set_tests_properties(dr_oom_unservable PROPERTIES TIMEOUT 90)

    foreach(_grow set deque)
        add_test(NAME dr_oom_${_grow}_grow
            COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_${_grow}_grow.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_${_grow}_grow && ( ulimit -v 32768 && ${CMAKE_CURRENT_BINARY_DIR}/oom_${_grow}_grow 2>&1 ; echo \"rc=$?\" ) | grep -q 'rc=134'")
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
        PASS_REGULAR_EXPRESSION "never falls back to Any")
endforeach()

foreach(_guard import_missing_guard from_import_missing_guard)
    add_test(NAME dr_${_guard}
        COMMAND $<TARGET_FILE:dragon> check ${CMAKE_CURRENT_SOURCE_DIR}/dr/${_guard}.dr)
    set_tests_properties(dr_${_guard} PROPERTIES
        PASS_REGULAR_EXPRESSION "cannot find module 'zzz_no_such_module'")
endforeach()

add_test(NAME dr_test_generics_py
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/generics_py/test_generics_py.py)

add_test(NAME dr_ffi_cxx_shim
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/ffi_cxx/app.dr
        --cc-source ${CMAKE_CURRENT_SOURCE_DIR}/dr/ffi_cxx/shim.cpp)

add_test(NAME dr_test_raft
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/raft/test_raft.dr
        -I ${CMAKE_SOURCE_DIR}/packages)
add_test(NAME dr_test_odb_raft
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/raft/test_odb_raft.dr
        -I ${CMAKE_SOURCE_DIR}/packages)

add_test(NAME dr_test_frames
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_SOURCE_DIR}/packages/frames/test_frames.dr
        -I ${CMAKE_SOURCE_DIR}/packages)

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

