cmake_minimum_required(VERSION 3.20)
project(fly-cc CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- LLVM ---------------------------------------------------------------
# Pin a specific LLVM major version per docs/architecture.md ("pin a version
# rather than floating"). Tested against LLVM 17/18. Point CMake at your
# LLVM install if it's not on the default search path, e.g.:
#   cmake -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(${LLVM_DIR}/AddLLVM.cmake OPTIONAL)
include_directories(${LLVM_INCLUDE_DIRS})

# LLVM_DEFINITIONS can arrive as a single quoted command-line string on
# Windows/MinGW. Split it into individual compiler arguments before applying it.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_compile_options(${LLVM_DEFINITIONS_LIST})

llvm_map_components_to_libnames(LLVM_LIBS
    core support irreader
    x86codegen x86asmparser x86desc x86info x86disassembler
    aarch64codegen aarch64asmparser aarch64desc aarch64info aarch64disassembler
    mc mcparser target
)

# --- OpenSSL (TLS/HTTPS follow-up milestone) ------------------------------
# Milestone 8 §5: "inspect what is installed/available" rather than
# assuming. This build environment has libssl3/libssl-dev (OpenSSL 3.0.x,
# see runtime/net.c's header comment for the version-specific API notes)
# but no mbedTLS/LibreSSL dev packages -- OpenSSL is both what's actually
# available AND the most widely-deployed choice with a stable native C
# API, so it's what runtime/net.c's TLS support is built on. Required (not
# optional): once runtime/net.c references libssl/libcrypto symbols, the
# runtime library needs a real TLS implementation to link against -- there
# is no "TLS disabled" build mode (see net.c's header comment on why a
# stub isn't an acceptable fallback here).
find_package(OpenSSL REQUIRED)
message(STATUS "Found OpenSSL ${OPENSSL_VERSION}")

# --- libflyrt (C runtime, plain C ABI) -----------------------------------
add_library(flyrt STATIC
    runtime/value.c
    runtime/show.c
    runtime/arith.c
    runtime/eh.cpp
    runtime/text.c
    runtime/coll.c
    runtime/board.c
    runtime/strbuild.c
    runtime/cast.c
    runtime/iter.c
    runtime/path.c
    runtime/filesystem.c
    runtime/environment.c
    runtime/system.c
    runtime/process.c
    runtime/net.c
)
target_include_directories(flyrt PUBLIC runtime)
target_link_libraries(flyrt PUBLIC OpenSSL::SSL OpenSSL::Crypto)
# Windows portability pass: the runtime's sockets layer (net.c) and Winsock
# hostname lookup (system.c) need ws2_32. flyrt is a STATIC library, so this
# link SPEC is mainly what makes any in-tree consumer resolve Winsock
# correctly; the binaries fly-cc actually produces are linked by driver.cpp's
# own `cc` invocation, which appends -lws2_32 itself (see src/driver/
# driver.cpp). Both must stay in sync.
if(WIN32)
    target_link_libraries(flyrt PUBLIC ws2_32)
    # Use Microsoft ABI for struct returns: 16-byte structs with two integer
    # members (like FlyValue={i64,i64}) are returned in RAX:RDX registers
    # per the Microsoft x64 ABI. This matches LLVM's MinGW backend behavior,
    # avoiding the sret vs register-return ABI mismatch with MinGW GCC's
    # default (sret for all 16-byte structs).
    target_compile_options(flyrt PRIVATE -mabi=ms)
endif()

# --- fly-cc (the compiler itself) ----------------------------------------
add_executable(fly-cc
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ast/ast.cpp
    src/sema/sema.cpp
    src/codegen/codegen.cpp
    src/driver/driver.cpp
)
target_include_directories(fly-cc PRIVATE include)
target_link_libraries(fly-cc PRIVATE ${LLVM_LIBS})
target_compile_definitions(fly-cc PRIVATE FLYRT_LIB_PATH="$<TARGET_FILE:flyrt>")
# Milestone 7 (§3.6 point 2): the bundled Fly standard library ships as
# plain .fly sources in the repo (see stdlib/) rather than anything
# compiled -- `bring process` etc. resolve here by default, see driver.cpp's
# resolveModule. Pointing this at the source tree (rather than an installed
# copy) is the same "no real install step yet" MVP simplification the rest
# of this build already makes (e.g. FLYRT_LIB_PATH above points at the
# build tree, not an installed libflyrt).
target_compile_definitions(fly-cc PRIVATE FLY_BUILTIN_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib")
add_dependencies(fly-cc flyrt)

# --- fly-repl (interactive REPL, milestone 7 §5-§14) ----------------------
# Deliberately does NOT link LLVM/Sema/CodeGen: per docs/architecture.md
# §6.1 ("The REPL may use fly-cc and libflyrt internally"), each turn is
# compiled and run by shelling out to the fly-cc binary above and then
# executing the native binary it produces -- the exact same Lexer/Parser/
# Sema/CodeGen pipeline every other Fly program goes through, with no
# second implementation to keep in sync. The Lexer/Parser/AST sources are
# relinked here only so fly-repl can classify an already-known-good turn's
# statements into "persist across turns" vs "run once" (see
# src/repl/repl.cpp's header comment) -- not to duplicate compilation.
add_executable(fly-repl
    src/repl/repl.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ast/ast.cpp
)
target_include_directories(fly-repl PRIVATE include)
add_dependencies(fly-repl fly-cc)

# --- fly (toolchain bootstrap/launcher, docs/architecture.md §7) ---------
# Project-level commands run inside `fly` itself (project.cpp); the official
# formatter (format.cpp) reuses the compiler's Lexer + Parser as its
# token/validation backbone -- the same relink-flycclib pattern fly-repl
# already uses (see the fly-repl note above), without duplicating a private
# grammar implementation in the toolchain binary.
add_executable(fly
    src/fly/fly.cpp
    src/fly/project.cpp
    src/fly/format.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
)
target_include_directories(fly PRIVATE include)
add_dependencies(fly fly-repl fly-cc)
# SLEEP/NET follow-up, §8/§9: `fly -deps`'s local Dump mirror (see
# dump-mirror/'s own header comment and fly.cpp's -deps implementation for
# why this isn't a real network fetch yet).
target_compile_definitions(fly PRIVATE FLY_DUMP_MIRROR_DIR="${CMAKE_SOURCE_DIR}/dump-mirror")
target_compile_definitions(fly PRIVATE FLY_DEFAULT_ICON="${CMAKE_SOURCE_DIR}/fly.ico")

# --- Windows icon resources -------------------------------------------------
# Embed the repository fly.ico as a PE icon resource (RT_GROUP_ICON) in
# every Fly toolchain executable, so Explorer/the taskbar show the Fly icon
# instead of a generic blank one. On non-Windows hosts this is a no-op
# (Linux builds never embed PE resources). Each target gets its own .rc
# written into the build tree; CMake's RC support (windres under MinGW)
# compiles it into the target at link time -- the same windres mechanism
# fly-cc's driver uses for the icons it embeds into user executables.
if(WIN32)
    function(fly_embed_default_icon TARGET)
        set(_rc ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_fly_icon.rc)
        file(WRITE ${_rc} "1 ICON \"${CMAKE_SOURCE_DIR}/fly.ico\"\n")
        set_source_files_properties(${_rc} PROPERTIES LANGUAGE RC)
        target_sources(${TARGET} PRIVATE ${_rc})
    endfunction()
else()
    function(fly_embed_default_icon TARGET)
    endfunction()
endif()

fly_embed_default_icon(fly-cc)
fly_embed_default_icon(fly-repl)
fly_embed_default_icon(fly)

enable_testing()
add_subdirectory(tests)
