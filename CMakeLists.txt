cmake_minimum_required(VERSION 3.16)
# Copyright (c) 2018 Tersoo Ortserga <ortserga@gmail.com>
file(STRINGS include/dragon.h DRAGON_VERSION_LINE REGEX "constexpr const char\\* VERSION")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" DRAGON_VERSION "${DRAGON_VERSION_LINE}")
if(NOT DRAGON_VERSION)
    message(FATAL_ERROR "could not parse dragon::VERSION from include/dragon.h")
endif()
project(Dragon VERSION ${DRAGON_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DRAGON_WARNINGS -Wall -Wextra)

# Claim LLVM's zstd imported targets with the static archive BEFORE find_package(LLVM),
# else the link pulls Homebrew's dylib and the shipped binary dies on Macs without brew.
if(APPLE)
    find_library(ZSTD_LIBRARY NAMES libzstd.a REQUIRED)
    add_library(zstd::libzstd_shared STATIC IMPORTED)
    set_target_properties(zstd::libzstd_shared PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}")
    add_library(zstd::libzstd_static STATIC IMPORTED)
    set_target_properties(zstd::libzstd_static PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}")
endif()

find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS 21.0)
    message(FATAL_ERROR
        "Dragon requires LLVM >= 21 (found ${LLVM_PACKAGE_VERSION}). The codegen "
        "uses the Triple-based Module API introduced in LLVM 21.")
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

include_directories(${CMAKE_SOURCE_DIR}/include)

set(DRAGON_SOURCES
    src/Token.cpp
    src/Lexer.cpp
    src/AST.cpp
    src/AstClone.cpp
    src/Parser.cpp
    src/ParserStmts.cpp
    src/Sema.cpp
    src/DefiniteAssignment.cpp
    src/OwnershipCheck.cpp
    src/TypeChecker.cpp
    src/TypeCheckerExprs.cpp
    src/TypeCheckerStmts.cpp
    src/TypeCheckerGenerics.cpp
    src/CodeGen.cpp
    src/codegen/Literals.cpp
    src/codegen/Expressions.cpp
    src/codegen/CallExpr.cpp
    src/codegen/CallBuiltins.cpp
    src/codegen/CallMethods.cpp
    src/codegen/Attributes.cpp
    src/codegen/Collections.cpp
    src/codegen/Comprehensions.cpp
    src/codegen/Functions.cpp
    src/codegen/Concurrency.cpp
    src/codegen/Statements.cpp
    src/codegen/Assign.cpp
    src/codegen/AugAnnAssign.cpp
    src/codegen/ForLoop.cpp
    src/codegen/Exceptions.cpp
    src/codegen/Classes.cpp
    src/codegen/ClassSynthesis.cpp
    src/codegen/Module.cpp
    src/codegen/ImplInit.cpp
    src/codegen/ImplMethods.cpp
    src/codegen/ImplMethods2.cpp
    src/codegen/EscapeAnalysis.cpp
    src/DiagnosticFormatter.cpp
    src/TypeHintEnforcer.cpp
    src/ModuleResolver.cpp
    src/PythonMigrator.cpp
    src/TypeInference.cpp
    src/Driver.cpp
    src/FfiSync.cpp
    src/Platform.cpp
    src/dragon.cpp
)

add_library(dragon_lib STATIC ${DRAGON_SOURCES})
target_include_directories(dragon_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(dragon_lib PRIVATE ${DRAGON_WARNINGS})

llvm_map_components_to_libnames(LLVM_LIBS
    core
    support
    irreader
    codegen
    mc
    mcparser
    option
    passes
    target
    native
)

target_link_libraries(dragon_lib PUBLIC ${LLVM_LIBS})

add_library(dragon_llhttp STATIC
    lib/llhttp/llhttp.c
    lib/llhttp/api.c
    lib/llhttp/http.c)
target_include_directories(dragon_llhttp PUBLIC ${CMAKE_SOURCE_DIR}/lib/llhttp)
set_target_properties(dragon_llhttp PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(dragon_llhttp PRIVATE -w)

# DRAGON_ASAN builds a SECOND, ASan-instrumented runtime that only compiled user
# programs link; the dragon compiler keeps the normal one. UAF-hunting only, never ship.
option(DRAGON_ASAN "Link compiled programs against an AddressSanitizer runtime (DEBUG/UAF-hunting only, never ship)" OFF)

add_library(dragon_runtime STATIC
    lib/Runtime/runtime_core.cpp
    lib/Runtime/runtime_string.cpp
    lib/Runtime/runtime_string_methods.cpp
    lib/Runtime/runtime_list.cpp
    lib/Runtime/runtime_dict.cpp
    lib/Runtime/runtime_collections.cpp
    lib/Runtime/runtime_exception.cpp
    lib/Runtime/runtime_builtins.cpp
    lib/Runtime/runtime_concurrency.cpp
    lib/Runtime/runtime_platform.cpp
    lib/Runtime/runtime_fileio.cpp
    lib/Runtime/runtime_zlib.cpp
    lib/Runtime/runtime_zstd.cpp
    lib/Runtime/runtime_box.cpp
    lib/Runtime/runtime_sqltemplate.cpp
    lib/Runtime/runtime_crypto.cpp
    lib/Runtime/runtime_tls.cpp
    lib/Runtime/runtime_ed25519.cpp
    lib/Runtime/runtime_getpass.cpp
    lib/Runtime/runtime_subprocess.cpp
    lib/Runtime/runtime_argon2id.cpp
    lib/ed25519/ed25519.c)
target_include_directories(dragon_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/Minicoro
    ${CMAKE_SOURCE_DIR}/lib/llhttp
    ${CMAKE_SOURCE_DIR}/lib/ed25519
    ${CMAKE_SOURCE_DIR}/lib/Runtime)
target_compile_options(dragon_runtime PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${DRAGON_WARNINGS}>)
target_link_libraries(dragon_runtime PRIVATE dragon_llhttp)
target_link_libraries(dragon_runtime PRIVATE dragon_mbedtls)
target_compile_definitions(dragon_runtime PRIVATE
    DRAGON_CA_BUNDLE="${CMAKE_SOURCE_DIR}/share/dragon/certs/cacert.pem")
find_package(ZLIB REQUIRED)
if(NOT APPLE)
    find_library(ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)
endif()
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
target_include_directories(dragon_runtime PRIVATE ${ZSTD_INCLUDE_DIR})
target_link_libraries(dragon_runtime PRIVATE ZLIB::ZLIB ${ZSTD_LIBRARY})

set(DRAGON_RUNTIME_FOR_PROGRAMS dragon_runtime)
if(DRAGON_ASAN)
    message(WARNING "DRAGON_ASAN=ON: compiled programs link an AddressSanitizer runtime and run ~2x slower. DEBUG/UAF-hunting ONLY - never ship or benchmark this build.")
    get_target_property(_dragon_rt_srcs dragon_runtime SOURCES)
    add_library(dragon_runtime_asan STATIC ${_dragon_rt_srcs})
    target_include_directories(dragon_runtime_asan PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/Minicoro ${CMAKE_SOURCE_DIR}/lib/llhttp
        ${CMAKE_SOURCE_DIR}/lib/ed25519 ${CMAKE_SOURCE_DIR}/lib/Runtime)
    target_link_libraries(dragon_runtime_asan PRIVATE
        dragon_llhttp dragon_mbedtls ZLIB::ZLIB ${ZSTD_LIBRARY})
    target_compile_definitions(dragon_runtime_asan PRIVATE
        DRAGON_CA_BUNDLE="${CMAKE_SOURCE_DIR}/share/dragon/certs/cacert.pem")
    target_compile_options(dragon_runtime_asan PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${DRAGON_WARNINGS}>
        -fsanitize=address -fno-omit-frame-pointer -g -O1)
    set(DRAGON_RUNTIME_FOR_PROGRAMS dragon_runtime_asan)
endif()

add_library(dragon_sqlite3 STATIC lib/SQLite/sqlite3.c)
target_include_directories(dragon_sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/lib/SQLite)
set_target_properties(dragon_sqlite3 PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(dragon_sqlite3 PRIVATE -w)

set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_SHOW_REPORT OFF CACHE BOOL "" FORCE)
set(PCRE2_STATIC_PIC ON CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_VALGRIND OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBBZ2 OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBZ OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBEDIT OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBREADLINE OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/PCRE2)

file(GLOB MBEDTLS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/mbedtls/library/*.c)
add_library(dragon_mbedtls STATIC ${MBEDTLS_SOURCES})
target_include_directories(dragon_mbedtls PUBLIC
    ${CMAKE_SOURCE_DIR}/lib/mbedtls/include
    ${CMAKE_SOURCE_DIR}/lib/mbedtls/library
    ${CMAKE_SOURCE_DIR}/lib/mbedtls)
target_compile_definitions(dragon_mbedtls PUBLIC
    MBEDTLS_USER_CONFIG_FILE="dragon_mbedtls_config.h")
set_target_properties(dragon_mbedtls PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(dragon_mbedtls PRIVATE -w)

set(DRAGON_EGG_OUT ${CMAKE_BINARY_DIR}/dragon-egg${CMAKE_EXECUTABLE_SUFFIX})
set(DRAGON_EGG_SRC_FILE ${CMAKE_SOURCE_DIR}/tools/egg/egg.dr)

# Baked on every platform: bare -lzstd misses /usr/local/lib on the BSDs, so
# compiled programs link zstd by the discovered absolute path.
set(DRAGON_ZSTD_DEF DRAGON_ZSTD_LIB="${ZSTD_LIBRARY}")
target_compile_definitions(dragon_lib PRIVATE
    DRAGON_RUNTIME_LIB="$<TARGET_FILE:${DRAGON_RUNTIME_FOR_PROGRAMS}>"
    DRAGON_SQLITE3_LIB="$<TARGET_FILE:dragon_sqlite3>"
    DRAGON_PCRE2_LIB="$<TARGET_FILE:pcre2-8-static>"
    DRAGON_LLHTTP_LIB="$<TARGET_FILE:dragon_llhttp>"
    DRAGON_MBEDTLS_LIB="$<TARGET_FILE:dragon_mbedtls>"
    ${DRAGON_ZSTD_DEF}
    DRAGON_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib"
    DRAGON_PLATFORM_DIR="${CMAKE_SOURCE_DIR}/platform"
    DRAGON_EGG_BIN="${DRAGON_EGG_OUT}"
    DRAGON_EGG_SRC="${DRAGON_EGG_SRC_FILE}"
    $<$<BOOL:${DRAGON_ASAN}>:DRAGON_ASAN_BUILD=1>)

add_executable(dragon tools/dragon/main.cpp)
target_compile_options(dragon PRIVATE ${DRAGON_WARNINGS})
target_link_libraries(dragon PRIVATE dragon_lib dragon_runtime)
if(APPLE)
    target_link_options(dragon PRIVATE LINKER:-dead_strip_dylibs)
endif()

target_compile_definitions(dragon PRIVATE
    DRAGON_RUNTIME_LIB="$<TARGET_FILE:${DRAGON_RUNTIME_FOR_PROGRAMS}>"
    DRAGON_SQLITE3_LIB="$<TARGET_FILE:dragon_sqlite3>"
    DRAGON_PCRE2_LIB="$<TARGET_FILE:pcre2-8-static>"
    DRAGON_LLHTTP_LIB="$<TARGET_FILE:dragon_llhttp>"
    DRAGON_MBEDTLS_LIB="$<TARGET_FILE:dragon_mbedtls>"
    ${DRAGON_ZSTD_DEF}
    DRAGON_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib"
    DRAGON_PLATFORM_DIR="${CMAKE_SOURCE_DIR}/platform"
    DRAGON_EGG_BIN="${DRAGON_EGG_OUT}"
    DRAGON_EGG_SRC="${DRAGON_EGG_SRC_FILE}"
    $<$<BOOL:${DRAGON_ASAN}>:DRAGON_ASAN_BUILD=1>)

add_custom_command(
    OUTPUT ${DRAGON_EGG_OUT}
    COMMAND $<TARGET_FILE:dragon> build ${DRAGON_EGG_SRC_FILE} -o ${DRAGON_EGG_OUT}
    DEPENDS dragon ${DRAGON_EGG_SRC_FILE} ${CMAKE_SOURCE_DIR}/stdlib/drs.dr
    COMMENT "Building dragon-egg sidecar (spec-22 package CLI)"
    VERBATIM)
add_custom_target(dragon_egg ALL DEPENDS ${DRAGON_EGG_OUT})

if(DRAGON_ASAN)
    add_dependencies(dragon dragon_runtime_asan)
endif()

add_custom_command(TARGET dragon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        $<TARGET_FILE_NAME:dragon> $<TARGET_FILE_DIR:dragon>/dr
    COMMENT "Creating dr -> dragon symlink")

include(GNUInstallDirs)

install(TARGETS dragon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(PROGRAMS ${DRAGON_EGG_OUT}
    DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS dragon_runtime dragon_sqlite3 dragon_llhttp dragon_mbedtls
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dragon)

install(FILES $<TARGET_FILE:pcre2-8-static>
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/dragon)

if(APPLE)
    install(FILES ${ZSTD_LIBRARY}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/dragon)
endif()

install(DIRECTORY stdlib/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragon/stdlib
    FILES_MATCHING PATTERN "*.dr" PATTERN "*.py")

install(DIRECTORY platform/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragon/platform
    FILES_MATCHING PATTERN "*.cpp" PATTERN "*.mm" PATTERN "*.h")

install(DIRECTORY share/dragon/certs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragon/certs
    FILES_MATCHING PATTERN "*.pem")

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(NOT WIN32)
    install(CODE "
        set(_dr_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
        # Symlink target is the bare filename so the link is relative -
        # works whether the package is installed at /usr or extracted to /opt.
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink dragon dr
            WORKING_DIRECTORY \"\${_dr_bindir}\")
    ")
endif()

install(CODE "
    set(_prefix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    file(REMOVE
        \"\${_prefix}/bin/pcre2-config\"
        \"\${_prefix}/include/pcre2.h\"
        \"\${_prefix}/include/pcre2posix.h\"
        \"\${_prefix}/lib/libpcre2-8.a\"
        \"\${_prefix}/lib/libpcre2-posix.a\"
        \"\${_prefix}/lib/pkgconfig/libpcre2-8.pc\"
        \"\${_prefix}/lib/pkgconfig/libpcre2-posix.pc\"
        \"\${_prefix}/cmake/pcre2-config.cmake\"
        \"\${_prefix}/cmake/pcre2-config-version.cmake\")
    # Remove now-empty dirs (only if empty - RPATH/other tools may share them).
    foreach(_d \"\${_prefix}/cmake\" \"\${_prefix}/lib/pkgconfig\")
        file(GLOB _contents \"\${_d}/*\")
        if(NOT _contents)
            file(REMOVE_RECURSE \"\${_d}\")
        endif()
    endforeach()
")

option(DRAGON_BUILD_TESTS "Build Dragon tests" OFF)
if(DRAGON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

set(CPACK_PACKAGE_NAME "dragon")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Dragon Language")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Dragon: a typed, compiled language inspired by Python, targeting LLVM")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://dragonlang.org")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Dragon")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_CONTACT "Dragon Team <hello@dragonlang.org>")
set(CPACK_STRIP_FILES TRUE)

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Dragon Team <hello@dragonlang.org>")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "gcc | clang, zlib1g-dev, libzstd-dev")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "build-essential")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
    set(CPACK_RPM_PACKAGE_REQUIRES "(gcc or clang), zlib-devel, libzstd-devel")
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/share" "/usr/share/man" "/usr/include")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_SYSTEM_NAME "Darwin")
elseif(WIN32)
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_WIX_PRODUCT_GUID "*")
    set(CPACK_WIX_UPGRADE_GUID "8B5F2C1A-3E7D-4A9B-9C6E-1D2F3A4B5C6D")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Dragon")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Dragon")
endif()

set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/CPackOptions.cmake")

include(CPack)

message(STATUS "")
message(STATUS "Dragon Configuration:")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  LLVM Version: ${LLVM_PACKAGE_VERSION}")
message(STATUS "  CPack:        ${CPACK_GENERATOR}")
message(STATUS "")
