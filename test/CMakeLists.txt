include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

# Prevent overriding parent project compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Don't ship gtest/gmock headers and cmake configs in the dragon package.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Include project headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# Helper header available to all tests
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# --- Lexer Tests ---
add_executable(dragon_lexer_tests LexerTest.cpp)
target_link_libraries(dragon_lexer_tests PRIVATE dragon_lib gtest_main)
add_test(NAME LexerTests COMMAND dragon_lexer_tests)

# --- Parser Tests ---
add_executable(dragon_parser_tests ParserTest.cpp)
target_link_libraries(dragon_parser_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ParserTests COMMAND dragon_parser_tests)

# --- Sema Tests ---
add_executable(dragon_sema_tests SemaTest.cpp)
target_link_libraries(dragon_sema_tests PRIVATE dragon_lib gtest_main)
add_test(NAME SemaTests COMMAND dragon_sema_tests)

# --- Definite-Assignment Tests ---
add_executable(dragon_definite_assignment_tests DefiniteAssignmentTest.cpp)
target_link_libraries(dragon_definite_assignment_tests PRIVATE dragon_lib gtest_main)
add_test(NAME DefiniteAssignmentTests COMMAND dragon_definite_assignment_tests)

# --- Module Initialization-Order (definite-init) Tests ---
add_executable(dragon_module_init_order_tests ModuleInitOrderTest.cpp)
target_link_libraries(dragon_module_init_order_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ModuleInitOrderTests COMMAND dragon_module_init_order_tests)

# --- Ownership (del/own/dub) Tests ---
add_executable(dragon_ownership_tests OwnershipCheckTest.cpp)
target_link_libraries(dragon_ownership_tests PRIVATE dragon_lib gtest_main)
add_test(NAME OwnershipTests COMMAND dragon_ownership_tests)

# --- TypeChecker Tests ---
add_executable(dragon_typechecker_tests TypeCheckerTest.cpp)
target_link_libraries(dragon_typechecker_tests PRIVATE dragon_lib gtest_main)
add_test(NAME TypeCheckerTests COMMAND dragon_typechecker_tests)

# --- AST Printer Tests ---
add_executable(dragon_ast_tests ASTTest.cpp)
target_link_libraries(dragon_ast_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ASTTests COMMAND dragon_ast_tests)

# --- TypeHintEnforcer Tests ---
add_executable(dragon_enforcer_tests TypeHintEnforcerTest.cpp)
target_link_libraries(dragon_enforcer_tests PRIVATE dragon_lib gtest_main)
add_test(NAME EnforcerTests COMMAND dragon_enforcer_tests)

# --- ModuleResolver Tests ---
add_executable(dragon_resolver_tests ModuleResolverTest.cpp)
target_link_libraries(dragon_resolver_tests PRIVATE dragon_lib gtest_main)
add_test(NAME ResolverTests COMMAND dragon_resolver_tests)

# --- Diagnostic Formatter Tests ---
add_executable(dragon_diagnostic_tests DiagnosticTest.cpp)
target_link_libraries(dragon_diagnostic_tests PRIVATE dragon_lib gtest_main)
add_test(NAME DiagnosticTests COMMAND dragon_diagnostic_tests)

# --- Interop (End-to-End) Tests ---
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

# --- CodeGen (LLVM) Tests ---
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

# --- Dragon-language KAT suites (test/dr/test_*.dr via stdlib/unittest.dr) ---
# Per ADR 038 §11, crypto/TLS self-tests are written in .dr, not GoogleTest.
# Each file ends in unittest.main([...]), which exits non-zero on failure, so
# CTest gates on the process exit code. The dragon binary has DRAGON_STDLIB_DIR
# baked in, so stdlib imports (unittest, hashlib, hmac, secrets) resolve with no
# extra env. CONFIGURE_DEPENDS re-globs on build so new test_*.dr auto-register.
file(GLOB DR_TEST_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_*.dr)
foreach(_dr ${DR_TEST_FILES})
    get_filename_component(_name ${_dr} NAME_WE)
    # -I packages resolves first-party package imports (odb, raft, frames);
    # stdlib imports still resolve via the baked-in DRAGON_STDLIB_DIR.
    add_test(NAME dr_${_name} COMMAND $<TARGET_FILE:dragon> run ${_dr}
        -I ${CMAKE_SOURCE_DIR}/packages)
endforeach()

# D044 - generics `.py`-mode parity. Registered explicitly because the test_*.dr
# glob above is .dr-only; this file must keep its .py extension to parse in
# indentation mode (the whole point of the surface-parity check).
add_test(NAME dr_test_generics_py
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/generics_py/test_generics_py.py)

# ADR 041 - external native-extension FFI. This one is registered explicitly
# (not via the test_*.dr glob) because it needs the --cc-source flag to compile
# and link a C++ shim, which the bare `dragon run` glob above cannot pass.
add_test(NAME dr_ffi_cxx_shim
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/ffi_cxx/app.dr
        --cc-source ${CMAKE_CURRENT_SOURCE_DIR}/dr/ffi_cxx/shim.cpp)

# ODB M11 - the raft consensus package (packages/raft) and Raft HA over the
# commit log (odb.ha). Registered explicitly because they need -I to resolve
# the package, which the bare glob cannot pass.
add_test(NAME dr_test_raft
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/raft/test_raft.dr
        -I ${CMAKE_SOURCE_DIR}/packages)
add_test(NAME dr_test_odb_raft
    COMMAND $<TARGET_FILE:dragon> run
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/raft/test_odb_raft.dr
        -I ${CMAKE_SOURCE_DIR}/packages)

