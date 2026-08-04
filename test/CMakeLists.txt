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

# --- ffi sync tool tests (D052 stub generation + provenance guard) ---
add_executable(dragon_ffisync_tests FfiSyncTest.cpp)
target_link_libraries(dragon_ffisync_tests PRIVATE dragon_lib gtest_main)
add_test(NAME FfiSyncTests COMMAND dragon_ffisync_tests)

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

# Sustained-OOM recoverability. Outside the test_*.dr glob on purpose: it is
# only meaningful under a tight RLIMIT_AS (uncapped it never OOMs and its
# assertion fails). Compile uncapped, run capped. Linux-only: macOS accepts
# but does not enforce RLIMIT_AS.
if(NOT APPLE)
    add_test(NAME dr_oom_sustained
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_sustained.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_sustained && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_sustained")
    set_tests_properties(dr_oom_sustained PROPERTIES TIMEOUT 90)
    add_test(NAME dr_oom_bytes
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_bytes.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_bytes && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_bytes")
    set_tests_properties(dr_oom_bytes PROPERTIES TIMEOUT 90)
    # Container constructors: count-sized data buffers must raise a catchable
    # MemoryError. Same 256MB cap as the bytes sibling, and the cap is load
    # bearing in the other direction too: the 160MB operand has to fit while
    # its copy does not.
    add_test(NAME dr_oom_containers
        COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_containers.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_containers && ulimit -v 262144 && exec ${CMAKE_CURRENT_BINARY_DIR}/oom_containers")
    set_tests_properties(dr_oom_containers PROPERTIES TIMEOUT 90)

    # Container GROW paths abort by design rather than raising (a longjmp out
    # of the armed dragon_shared_mut_begin window would arm a spurious
    # concurrent-mutation fatal later), so these two programs are expected to
    # die and the assertion is on HOW. 134 is SIGABRT with a diagnostic; the
    # regression being pinned is 139, the silent SIGSEGV they used to give.
    # 32768 rather than 262144 on purpose: at 256MB the set finishes growing
    # and never reaches the allocation failure at all.
    foreach(_grow set deque)
        add_test(NAME dr_oom_${_grow}_grow
            COMMAND bash -c "$<TARGET_FILE:dragon> build ${CMAKE_CURRENT_SOURCE_DIR}/dr/oom_${_grow}_grow.dr -o ${CMAKE_CURRENT_BINARY_DIR}/oom_${_grow}_grow && ( ulimit -v 32768 && ${CMAKE_CURRENT_BINARY_DIR}/oom_${_grow}_grow 2>&1 ; echo \"rc=$?\" ) | grep -q 'rc=134'")
        set_tests_properties(dr_oom_${_grow}_grow PROPERTIES TIMEOUT 90)
    endforeach()
endif()

# --- Stdlib import gate ---
# Every stdlib module must at least parse, name-resolve, and type-check. Both
# `uuid` and `errno` once shipped assigning a module global without `global`,
# which made `import uuid` / `import errno` a hard compile error - and no test
# imported either, so it went unnoticed. This gate imports every module in one
# generated file.
#
# `check`, not `run`: we are gating on resolution errors, and a full link would
# fail for unrelated reasons on some hosts (`ui` needs webview symbols,
# `threading` needs pthread_barrier, which macOS does not provide).
#
# One recursive glob covers every depth (`os/path.dr`, `ui/desktop/desktop.dr`),
# and CONFIGURE_DEPENDS re-globs on build so a newly added stdlib module is
# covered without editing this file. Note the check stops at the first module
# that fails, so a second broken module surfaces only after the first is fixed.
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

# D052 - the flagship's generated stub must stay in sync with its declaration.
add_test(NAME dr_ffi_sync_check
    COMMAND $<TARGET_FILE:dragon> ffi sync
        ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_ffi_python.dr --check)

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

