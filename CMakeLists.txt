cmake_minimum_required(VERSION 3.16)
# Copyright (c) 2018 Tersoo Ortserga <ortserga@gmail.com>
project(Dragon VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find LLVM.
# Find LLVM.
#
# No version arg on find_package. LLVM's LLVMConfigVersion.cmake matches the
# major version *exactly*, so that `find_package(LLVM 22)` will reject both older
# 21 and a newer trunk (23) tree. We want a floor with no ceiling, so we find any
# LLVM and assert the niminum.
#
# Floor is 21: the codegen uses the Triple-based Module API
# (Module::setTargetTriple(Triple) / getTargetTriple() -> const Triple&,
# createTargetMachine(const Triple&, ...)) that landed in LLVM 21. Anything
# older needs the string-based overloads instead. This accepts 21, 22, and
# trunk; CI and releases build against the latest packaged release (22).
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

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Dragon library sources
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
    src/StdlibRegistry.cpp
    src/DiagnosticFormatter.cpp
    src/TypeHintEnforcer.cpp
    src/ModuleResolver.cpp
    src/Driver.cpp
    src/Platform.cpp
    src/dragon.cpp
)

# Dragon library
add_library(dragon_lib STATIC ${DRAGON_SOURCES})
target_include_directories(dragon_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Get LLVM libraries. Dragon is a pure AOT compiler (build Module -> PassBuilder
# -> addPassesToEmitFile -> object -> system linker); it has no JIT, so the ORC
# components are not linked.
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

# llhttp (bundled, HTTP/1.1 parser from Node.js - MIT licensed)
add_library(dragon_llhttp STATIC
    lib/llhttp/llhttp.c
    lib/llhttp/api.c
    lib/llhttp/http.c)
target_include_directories(dragon_llhttp PUBLIC ${CMAKE_SOURCE_DIR}/lib/llhttp)
set_target_properties(dragon_llhttp PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(dragon_llhttp PRIVATE -w)

# Runtime library (split into domain-specific TUs)
# =============================================================================
#  DRAGON_ASAN - DEBUG / UAF-HUNTING ONLY. NEVER enable for a release or a     
#  benchmark.                                                                  
#                                                                              
#  OFF by default (and for every packaged build), so the normal build is       
#  byte-identical whether or not this option exists - zero effect on shipped   
#  code or compiler size when off.                                             
#                                                                              
#  When ON it builds a SECOND, AddressSanitizer-instrumented copy of the       
#  runtime that ONLY compiled user programs link (the `dragon` compiler keeps  
#  the normal runtime, so compiling stays clean and fast). Use it solely to    
#  chase use-after-free / double-free / heap-overflow in the runtime - e.g.    
#  refcount work. Instrumented programs run ~2× slower. Recipe:                
#    cmake -DDRAGON_ASAN=ON -B build-asan && cmake --build build-asan          
#    ASAN_OPTIONS=detect_leaks=0:fast_unwind_on_fatal=1 ./your_program         
# =============================================================================
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
target_link_libraries(dragon_runtime PRIVATE dragon_llhttp)
# mbedTLS engine for runtime_tls.cpp. Linking propagates dragon_mbedtls's PUBLIC
# include dir + MBEDTLS_USER_CONFIG_FILE, so the shim compiles against the same
# trimmed config the library was built with (struct-layout consistency).
target_link_libraries(dragon_runtime PRIVATE dragon_mbedtls)
# Path to the bundled Mozilla CA bundle for dragon_default_ca_file()'s final
# fallback. This is the dev-tree source path; installed Linux/BSD resolve via the
# system-path probe, and a binary-relative resolve for installed macOS/Windows is
# a follow-up (see the design spec).
target_compile_definitions(dragon_runtime PRIVATE
    DRAGON_CA_BUNDLE="${CMAKE_SOURCE_DIR}/share/dragon/certs/cacert.pem")
# Compression: link zlib + zstd. zlib is a system lib everywhere (macOS ships
# /usr/lib/libz.1.dylib; distros ship zlib1g). zstd is NOT part of macOS: only
# Homebrew provides it, so on Apple we must link the static archive - a .dylib
# dependency would bake the build machine's /opt/homebrew path into the shipped
# binary and dyld aborts on any Mac without brew zstd (v0.0.1 shipped like
# that). Naming libzstd.a exactly forces the static pick even though the same
# Homebrew dir also holds libzstd.dylib. Linux keeps the shared lib: the
# .deb/.rpm declare the libzstd dependency and apt/dnf resolve it.
find_package(ZLIB REQUIRED)
if(APPLE)
    find_library(ZSTD_LIBRARY NAMES libzstd.a REQUIRED)
else()
    find_library(ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)
endif()
# zstd has no imported target carrying its include dir (unlike ZLIB::ZLIB).
# On arm64 macOS Homebrew lives at /opt/homebrew, which is NOT on the default
# header search path, so resolve zstd.h explicitly.
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
target_include_directories(dragon_runtime PRIVATE ${ZSTD_INCLUDE_DIR})
target_link_libraries(dragon_runtime PRIVATE ZLIB::ZLIB ${ZSTD_LIBRARY})

# DRAGON_ASAN (see the banner above) - the instrumented runtime twin. Compiled
# user programs link THIS via DRAGON_RUNTIME_LIB; the `dragon` compiler still
# links the plain dragon_runtime, so the compiler itself is never instrumented.
# Reuses dragon_runtime's source list + mirrors its include/def/link deps, then
# adds the sanitizer flags. None of this exists when DRAGON_ASAN is OFF.
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
        -fsanitize=address -fno-omit-frame-pointer -g -O1)
    set(DRAGON_RUNTIME_FOR_PROGRAMS dragon_runtime_asan)
endif()

# SQLite3 amalgamation (bundled, no external dependency)
add_library(dragon_sqlite3 STATIC lib/SQLite/sqlite3.c)
target_include_directories(dragon_sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/lib/SQLite)
set_target_properties(dragon_sqlite3 PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(dragon_sqlite3 PRIVATE -w)  # suppress warnings from third-party code

# PCRE2 (bundled, 8-bit static only - no external dependency)
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
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/PCRE2)

# mbedTLS (bundled, static - TLS 1.2/1.3 + X.509 + crypto engine). Apache-2.0.
# Built SQLite-style: compile the library TUs directly rather than pulling in
# mbedTLS's own CMake. library/ is on the include path because the release
# tarball's pre-generated headers (psa_crypto_driver_wrappers.h, version_features.c,
# error.c) live there. Uses mbedTLS's default config for now; a trimmed in-scope
# config (legacy protocols/ciphers compiled out) is a follow-up per the design spec.
file(GLOB MBEDTLS_SOURCES ${CMAKE_SOURCE_DIR}/lib/mbedtls/library/*.c)
add_library(dragon_mbedtls STATIC ${MBEDTLS_SOURCES})
target_include_directories(dragon_mbedtls PUBLIC
    ${CMAKE_SOURCE_DIR}/lib/mbedtls/include
    ${CMAKE_SOURCE_DIR}/lib/mbedtls/library
    ${CMAKE_SOURCE_DIR}/lib/mbedtls)
# Dragon-owned delta config that trims out-of-scope modules, layered over the
# mbedTLS default via MBEDTLS_USER_CONFIG_FILE. PUBLIC so consumers (the TLS
# shim) compile against the IDENTICAL config - a mismatch would mean different
# mbedTLS struct sizes on each side of the boundary.
target_compile_definitions(dragon_mbedtls PUBLIC
    MBEDTLS_USER_CONFIG_FILE="dragon_mbedtls_config.h")
set_target_properties(dragon_mbedtls PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(dragon_mbedtls PRIVATE -w)  # suppress third-party warnings

# dragon-egg sidecar paths (spec-22). Defined here so BOTH dragon_lib (which holds
# Driver.cpp's resolveEggBin/buildEggArgv) and the dragon CLI bake them in.
set(DRAGON_EGG_OUT ${CMAKE_BINARY_DIR}/dragon-egg${CMAKE_EXECUTABLE_SUFFIX})
set(DRAGON_EGG_SRC_FILE ${CMAKE_SOURCE_DIR}/tools/egg/egg.dr)

# Pass runtime library path to source for LLVM CodeGen linking. The runtime
# linked into COMPILED PROGRAMS is the instrumented twin under DRAGON_ASAN;
# DRAGON_ASAN_BUILD then tells CodeGen::linkExecutable to add the sanitizer
# link flags. Both are no-ops when DRAGON_ASAN is OFF.
if(APPLE)
    set(DRAGON_ZSTD_DEF DRAGON_ZSTD_LIB="${ZSTD_LIBRARY}")
else()
    set(DRAGON_ZSTD_DEF "")
endif()
target_compile_definitions(dragon_lib PRIVATE
    DRAGON_RUNTIME_LIB="$<TARGET_FILE:${DRAGON_RUNTIME_FOR_PROGRAMS}>"
    DRAGON_SQLITE3_LIB="$<TARGET_FILE:dragon_sqlite3>"
    DRAGON_PCRE2_LIB="$<TARGET_FILE:pcre2-8-static>"
    DRAGON_LLHTTP_LIB="$<TARGET_FILE:dragon_llhttp>"
    DRAGON_MBEDTLS_LIB="$<TARGET_FILE:dragon_mbedtls>"
    ${DRAGON_ZSTD_DEF}
    DRAGON_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib"
    DRAGON_EGG_BIN="${DRAGON_EGG_OUT}"
    DRAGON_EGG_SRC="${DRAGON_EGG_SRC_FILE}"
    $<$<BOOL:${DRAGON_ASAN}>:DRAGON_ASAN_BUILD=1>)

# dragon CLI tool
add_executable(dragon tools/dragon/main.cpp)
target_link_libraries(dragon PRIVATE dragon_lib dragon_runtime)
# Drop load commands for dylibs no symbol is used from. Homebrew's LLVM is
# built against libz3 (its solver support), which lands on our link line via
# LLVMConfig even though Dragon uses zero z3 symbols; without this flag I believe
# ld64 records the absolute /opt/homebrew path and the shipped binary will abort on
# Macs without brew. 
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
    DRAGON_EGG_BIN="${DRAGON_EGG_OUT}"
    DRAGON_EGG_SRC="${DRAGON_EGG_SRC_FILE}"
    $<$<BOOL:${DRAGON_ASAN}>:DRAGON_ASAN_BUILD=1>)

# Compile the sidecar with the freshly-built dragon. Depends on `dragon` and
# the stdlib drs.dr it imports, so it rebuilds when either changes.
add_custom_command(
    OUTPUT ${DRAGON_EGG_OUT}
    COMMAND $<TARGET_FILE:dragon> build ${DRAGON_EGG_SRC_FILE} -o ${DRAGON_EGG_OUT}
    DEPENDS dragon ${DRAGON_EGG_SRC_FILE} ${CMAKE_SOURCE_DIR}/stdlib/drs.dr
    COMMENT "Building dragon-egg sidecar (spec-22 package CLI)"
    VERBATIM)
add_custom_target(dragon_egg ALL DEPENDS ${DRAGON_EGG_OUT})

# Force the instrumented runtime twin to build under DRAGON_ASAN. It's referenced
# only by a $<TARGET_FILE:...> path in a compile definition (no link edge), which
# doesn't imply a build-order dependency on its own.
if(DRAGON_ASAN)
    add_dependencies(dragon dragon_runtime_asan)
endif()

# Create `dr` symlink in build directory
add_custom_command(TARGET dragon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        $<TARGET_FILE_NAME:dragon> $<TARGET_FILE_DIR:dragon>/dr
    COMMENT "Creating dr -> dragon symlink")

# --- Installation ---
# Layout (matches Driver.cpp's resolveStdlibDir / resolveRuntimeLib):
#   <prefix>/bin/dragon         - CLI binary
#   <prefix>/lib/dragon/        - Bundled static libs the linker passes to cc
#   <prefix>/share/dragon/stdlib/  - Dragon stdlib (.dr files)
#   <prefix>/include/           - Public headers
include(GNUInstallDirs)

install(TARGETS dragon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# dragon-egg sidecar - installed alongside the dragon binary so the Driver's
# <prefix>/bin/dragon-egg resolution finds it in packaged builds.
install(PROGRAMS ${DRAGON_EGG_OUT}
    DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS dragon_runtime dragon_sqlite3 dragon_llhttp dragon_mbedtls
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dragon)

# pcre2-8-static is a third-party target; pull its file directly.
install(FILES $<TARGET_FILE:pcre2-8-static>
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/dragon)

# macOS only: ship the zstd static archive so user programs that use
# gzip/zstd/tarfile link without Homebrew on the target machine (the Driver
# resolves it via findBundledLib, same as sqlite3/pcre2). zstd is BSD-licensed.
# Linux relies on the distro libzstd declared as a package dependency.
if(APPLE)
    install(FILES ${ZSTD_LIBRARY}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/dragon)
endif()

# dragon_lib is the compiler frontend - only needed if someone embeds it.
# Not shipped in the default package; uncomment if you want to expose the
# embedder API in distribution builds.
# install(TARGETS dragon_lib
#     ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY stdlib/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragon/stdlib
    FILES_MATCHING PATTERN "*.dr" PATTERN "*.py")

# Bundled Mozilla CA trust store (ssl.load_default_certs fallback).
install(DIRECTORY share/dragon/certs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragon/certs
    FILES_MATCHING PATTERN "*.pem")

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# `dr` symlink alongside the `dragon` binary (POSIX only - Windows can't symlink
# without elevation; users invoke `dragon` directly there).
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

# Strip the third-party files PCRE2's own install rules pulled in. We can't
# disable those rules cleanly (CMAKE_SKIP_INSTALL_RULES isn't per-directory
# and PCRE2 has no opt-out flag), so the simplest reliable approach is to
# delete them after install runs. We keep the copy at lib/dragon/ that
# dragon's CLI links against - only the duplicates at standard system
# locations get removed.
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

# Testing (optional)
option(DRAGON_BUILD_TESTS "Build Dragon tests" OFF)
if(DRAGON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# --- Packaging (CPack) ---
# Generates installers straight from the install() rules above (single source
# of truth for the on-disk layout). Run from the build dir:
#     cpack -G DEB        # dragon_<ver>_amd64.deb       (Debian/Ubuntu)
#     cpack -G RPM        # dragon-<ver>-1.x86_64.rpm    (Fedora/RHEL)
#     cpack -G TGZ        # dragon-<ver>-Linux.tar.gz    (portable)
#     cpack -G DragNDrop  # dragon-<ver>-Darwin.dmg      (macOS)
#     cpack -G WIX        # dragon-<ver>-win64.msi       (Windows)
# The release CI (.github/workflows/release.yml) drives this per-OS and renames
# the artifacts to the scheme the download page (src/views/www.dr) expects.
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
    # Build all three Linux artifacts; CI selects what to publish.
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    # Land at /usr/bin/dragon + /usr/share/dragon/stdlib so the binary's
    # install-prefix probe (Driver.cpp resolveStdlibDir) finds the stdlib.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    # Debian / Ubuntu (.deb)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Dragon Team <hello@dragonlang.org>")
    # Dragon's runtime is statically linked; only libc/libstdc++/libgcc needed.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # gcc/clang: dragon shells out to cc for the final link. zlib1g-dev /
    # libzstd-dev: programs using gzip/zstd/tarfile link -lz / -lzstd, which
    # needs the dev .so symlinks, not just the runtime libs.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "gcc | clang, zlib1g-dev, libzstd-dev")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "build-essential")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

    # Fedora / RHEL / openSUSE (.rpm)
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
    set(CPACK_RPM_PACKAGE_REQUIRES "(gcc or clang), zlib-devel, libzstd-devel")
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
    # The stdlib lives under /usr/share; don't let rpm claim ownership of the
    # shared system dirs it merely populates.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/share" "/usr/share/man" "/usr/include")
elseif(APPLE)
    # No .dmg: macOS installs go through the curl install script (install.sh),
    # which avoids the Gatekeeper quarantine an unsigned disk image would carry.
    # The .tar.gz is what the script downloads and what manual installers use.
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_SYSTEM_NAME "Darwin")
elseif(WIN32)
    # MSI via the WiX toolset (requires WiX on the build machine / CI runner).
    # A portable .zip is produced alongside it.
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_WIX_PRODUCT_GUID "*")  # regenerated per build (new product code)
    # UPGRADE_GUID must stay constant across versions so MSIs upgrade in place.
    set(CPACK_WIX_UPGRADE_GUID "8B5F2C1A-3E7D-4A9B-9C6E-1D2F3A4B5C6D")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Dragon")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Dragon")
endif()

# Per-generator overrides, evaluated once per generator with CPACK_GENERATOR set.
# Keeps /usr for the .deb/.rpm (system packages need it) while giving the .tar.gz
# a clean bin/lib/share/include root on every platform. See cmake/CPackOptions.cmake.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/CPackOptions.cmake")

include(CPack)

# Print configuration
message(STATUS "")
message(STATUS "Dragon Configuration:")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  LLVM Version: ${LLVM_PACKAGE_VERSION}")
message(STATUS "  CPack:        ${CPACK_GENERATOR}")
message(STATUS "")
