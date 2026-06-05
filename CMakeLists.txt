cmake_minimum_required(VERSION 3.22)
project(rsharp VERSION 1.0.2 LANGUAGES C CXX)

if(WIN32)
    enable_language(RC)
    set(RSHARP_WIN_RESOURCES resources.rc)
else()
    set(RSHARP_WIN_RESOURCES)
endif()

set(CMAKE_C_STANDARD   11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-unused-function)

option(RSHARP_WITH_LLVM   "Build with LLVM backend (requires LLVM 17+)" OFF)
option(RSHARP_TARGET_WASM "Build for WASM (cross-compile via WASI SDK)"  OFF)

# ── Runtime memory library ──────────────────────────────────────────
add_library(rsharp_runtime STATIC runtime/memory/arena.c)
target_include_directories(rsharp_runtime PUBLIC runtime/memory)

# ── Diagnostic library ──────────────────────────────────────────────
add_library(rsharp_diag STATIC compiler/error/diagnostic.c)
target_include_directories(rsharp_diag PUBLIC compiler/error)
target_link_libraries(rsharp_diag PRIVATE rsharp_runtime)

# ── Lexer library ────────────────────────────────────────────────────
add_library(rsharp_lexer STATIC compiler/lexer/lexer.c)
target_include_directories(rsharp_lexer PUBLIC compiler/lexer)
target_link_libraries(rsharp_lexer PRIVATE rsharp_runtime rsharp_diag)

# ── Parser library ───────────────────────────────────────────────────
add_library(rsharp_parser STATIC compiler/parser/parser.c)
target_include_directories(rsharp_parser PUBLIC compiler/parser)
target_link_libraries(rsharp_parser PRIVATE rsharp_lexer)

# ── Sema (type checker) ──────────────────────────────────────────────
add_library(rsharp_sema STATIC compiler/sema/sema.c)
target_include_directories(rsharp_sema PUBLIC compiler/sema)
target_link_libraries(rsharp_sema PRIVATE rsharp_parser rsharp_diag)

# ── LLVM codegen (optional) ──────────────────────────────────────────
if(RSHARP_WITH_LLVM)
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_VERSION}")
    include_directories(${LLVM_INCLUDE_DIRS})
    add_definitions(${LLVM_DEFINITIONS})
    add_library(rsharp_codegen STATIC compiler/codegen/llvm_backend.cpp)
    target_include_directories(rsharp_codegen PUBLIC compiler/codegen)
    target_link_libraries(rsharp_codegen PRIVATE rsharp_sema)
    llvm_map_components_to_libnames(LLVM_LIBS
        core support irreader passes
        x86codegen amdgpucodegen aarch64codegen
        webassemblycodegen
        lto linker orcjit)
    target_link_libraries(rsharp_codegen PRIVATE ${LLVM_LIBS})
endif()

# ── rsc compiler driver ─────────────────────────────────────────────
if(NOT RSHARP_TARGET_WASM)
    add_executable(rsc tools/rsc/main.c ${RSHARP_WIN_RESOURCES})
    target_link_libraries(rsc PRIVATE rsharp_sema rsharp_lexer rsharp_diag rsharp_runtime)
    if(RSHARP_WITH_LLVM)
        target_link_libraries(rsc PRIVATE rsharp_codegen)
        target_compile_definitions(rsc PRIVATE RSHARP_WITH_LLVM)
    endif()

    # ── rsl / rsharp CLI ─────────────────────────────────────────────────
    add_executable(rsl tools/rsl/main.c ${RSHARP_WIN_RESOURCES})
    # Create a Windows-friendly rsharp.exe shim from rsl.exe.
    add_custom_command(TARGET rsl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rsl>
                $<TARGET_FILE_DIR:rsl>/rsharp$<TARGET_FILE_SUFFIX:rsl>
        COMMENT "Creating rsharp executable shim")
endif()

# ── Tests ─────────────────────────────────────────────────────────────
if(NOT RSHARP_TARGET_WASM)

enable_testing()

add_executable(test_lexer
    tests/unit/test_lexer.c
    compiler/lexer/lexer.c
    runtime/memory/arena.c
    compiler/error/diagnostic.c)
target_include_directories(test_lexer PRIVATE compiler/lexer compiler/error runtime/memory)
add_test(NAME lexer COMMAND test_lexer)

add_executable(test_parser
    tests/unit/test_parser.c
    compiler/parser/parser.c
    compiler/lexer/lexer.c
    compiler/error/diagnostic.c
    runtime/memory/arena.c)
target_include_directories(test_parser PRIVATE
    compiler/parser compiler/lexer compiler/error runtime/memory)
add_test(NAME parser COMMAND test_parser)

add_executable(test_sema
    tests/unit/test_sema.c
    compiler/sema/sema.c
    compiler/parser/parser.c
    compiler/lexer/lexer.c
    compiler/error/diagnostic.c
    runtime/memory/arena.c)
target_include_directories(test_sema PRIVATE
    compiler/sema compiler/parser compiler/lexer compiler/error runtime/memory)
add_test(NAME sema COMMAND test_sema)

# ── Install ────────────────────────────────────────────────────────────
install(TARGETS rsc rsl DESTINATION bin)
install(PROGRAMS $<TARGET_FILE_DIR:rsl>/rsharp$<TARGET_FILE_SUFFIX:rsl> DESTINATION bin)
install(DIRECTORY stdlib/ DESTINATION share/rsharp/stdlib
        FILES_MATCHING
        PATTERN "*.rsl" PATTERN "*.rsh" PATTERN "*.rss"
        PATTERN "*.rslib" PATTERN "*.rsp")
install(DIRECTORY docs/ DESTINATION share/rsharp/docs
        FILES_MATCHING PATTERN "*.md")

# ── rsrun ────────────────────────────────────────────────────────────
add_executable(rsrun tools/rsrun/main.c ${RSHARP_WIN_RESOURCES})

# ── rsfmt ────────────────────────────────────────────────────────────
add_executable(rsfmt tools/rsfmt/main.c ${RSHARP_WIN_RESOURCES})

# ── rsdoc ────────────────────────────────────────────────────────────
add_executable(rsdoc tools/rsdoc/main.c ${RSHARP_WIN_RESOURCES})

install(TARGETS rsrun rsfmt rsdoc DESTINATION bin)

endif()
