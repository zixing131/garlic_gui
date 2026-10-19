cmake_minimum_required(VERSION 3.26)

project(garlic C)

set(CMAKE_C_STANDARD 99)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -Wall")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-function")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter")

option(GARLIC_STRICT_WARNINGS "Treat implicit declarations, pointer type mismatches and discarded qualifiers as errors" OFF)
if (GARLIC_STRICT_WARNINGS)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=incompatible-pointer-types")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=incompatible-function-pointer-types")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=discarded-qualifiers")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-implicit-function-declaration")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-incompatible-pointer-types")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-discarded-qualifiers")
endif ()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-misleading-indentation")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-format")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-braces")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-parentheses")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-cpp")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-but-set-variable")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unknown-warning-option")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-variable")

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--gc-sections")
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")

if(NOT WIN32)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-ignored-optimization-argument")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto")
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    # MinGW ld needs --enable-stdcall-fixup to resolve Win32 API calls
    # (e.g. CreateDirectoryA -> CreateDirectoryA@8) without discarding
    # the import-stub sections from libkernel32.a.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--enable-stdcall-fixup")
endif ()

if (UNIX)
    SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -pthread")
endif ()

include_directories(src/common)
include_directories(src/libs/memory)
include_directories(src/libs/hashmap)
include_directories(src/libs/list)
include_directories(src/libs/bitset)
include_directories(src/libs/queue)
include_directories(src/libs/str)
include_directories(src/libs/zip)
include_directories(src/libs/threadpool)
include_directories(src/libs/cjson)
include_directories(src/libs/md4c)
include_directories(libs/include)
include_directories(src/ai)
include_directories(src/jar)
include_directories(src/dalvik)
include_directories(src/decompiler)
include_directories(src/decompiler/transformer)
include_directories(src)

# Local edits live in fork/overlay and shadow the matching upstream path.
set(GARLIC_OVERLAY_ROOT "${CMAKE_SOURCE_DIR}/fork/overlay")
set(_garlic_overlay_includes "")
foreach(inc
        src/common src/libs/memory src/libs/hashmap src/libs/list
        src/libs/bitset src/libs/queue src/libs/str src/libs/zip
        src/libs/threadpool src/libs/cjson src/libs/md4c
        src/ai src/jar src/dalvik src/decompiler src/decompiler/transformer
        src)
    if(EXISTS "${GARLIC_OVERLAY_ROOT}/${inc}")
        list(APPEND _garlic_overlay_includes "${GARLIC_OVERLAY_ROOT}/${inc}")
    endif()
endforeach()
if(_garlic_overlay_includes)
    include_directories(BEFORE ${_garlic_overlay_includes})
endif()

function(garlic_use_overlay out_var)
    set(picked "")
    foreach(src ${ARGN})
        if(EXISTS "${GARLIC_OVERLAY_ROOT}/${src}")
            list(APPEND picked "${GARLIC_OVERLAY_ROOT}/${src}")
        else()
            list(APPEND picked "${src}")
        endif()
    endforeach()
    set(${out_var} "${picked}" PARENT_SCOPE)
endfunction()

aux_source_directory(src/common COMMON)
aux_source_directory(src/libs/list LIST)
aux_source_directory(src/libs/hashmap HASHMAP)
aux_source_directory(src/parser/class/ CLASS_PARSER)
aux_source_directory(src/parser/dex DEX_PARSER)
aux_source_directory(src/parser/pe PE_PARSER)
aux_source_directory(src/libs/queue DQUEUE)
aux_source_directory(src/libs/str STR)
aux_source_directory(src/libs/memory MEMORY_POOL)
aux_source_directory(src/libs/zip ZIP)
aux_source_directory(src/libs/threadpool THREADPOOL)
aux_source_directory(src/libs/bitset BITSET)
aux_source_directory(src/libs/cjson CJSON)
aux_source_directory(src/decompiler DECOMPILER)
aux_source_directory(src/decompiler/transformer TRANSFORMER)
aux_source_directory(src/jar JAR)
aux_source_directory(src/jvm JVM)
aux_source_directory(src/apk APK)
aux_source_directory(src/dalvik DEX)
aux_source_directory(src/libs/trie TRIE)
aux_source_directory(src/analyzer ANALYZER)
aux_source_directory(src/ai AI)
set(MD4C
    src/libs/md4c/md4c.c
    src/libs/md4c/md4c-html.c
    src/libs/md4c/entity.c
)

set(REPORT
    src/report/report.c
)

set(HTTP_SERVER
    src/ai/http_server.c
)

garlic_use_overlay(COMMON ${COMMON})
garlic_use_overlay(LIST ${LIST})
garlic_use_overlay(HASHMAP ${HASHMAP})
garlic_use_overlay(CLASS_PARSER ${CLASS_PARSER})
garlic_use_overlay(DEX_PARSER ${DEX_PARSER})
garlic_use_overlay(PE_PARSER ${PE_PARSER})
garlic_use_overlay(DQUEUE ${DQUEUE})
garlic_use_overlay(STR ${STR})
garlic_use_overlay(MEMORY_POOL ${MEMORY_POOL})
garlic_use_overlay(ZIP ${ZIP})
garlic_use_overlay(THREADPOOL ${THREADPOOL})
garlic_use_overlay(BITSET ${BITSET})
garlic_use_overlay(CJSON ${CJSON})
garlic_use_overlay(DECOMPILER ${DECOMPILER})
garlic_use_overlay(TRANSFORMER ${TRANSFORMER})
garlic_use_overlay(JAR ${JAR})
garlic_use_overlay(JVM ${JVM})
garlic_use_overlay(APK ${APK})
garlic_use_overlay(DEX ${DEX})
garlic_use_overlay(TRIE ${TRIE})
garlic_use_overlay(ANALYZER ${ANALYZER})
garlic_use_overlay(AI ${AI})
garlic_use_overlay(MD4C ${MD4C})
garlic_use_overlay(REPORT ${REPORT})
garlic_use_overlay(HTTP_SERVER ${HTTP_SERVER})

set(JAVA_DEC
        ${COMMON}
        ${MEMORY_POOL}
        ${LIST}
        ${HASHMAP}
        ${BITSET}
        ${DQUEUE}
        ${STR}
        ${TRIE}
        ${DECOMPILER}
        ${TRANSFORMER}
        ${CLASS_PARSER}
        ${DEX_PARSER}
        ${JVM}
        ${ZIP}
        ${JAR}
        ${APK}
        ${DEX}
        ${THREADPOOL}
        ${CJSON}
        ${AI}
        ${ANALYZER}
        ${MD4C}
        ${REPORT}
        ${HTTP_SERVER}
)

# ---------------------------------------------------------------------------
# librosemarylib — locate the pre-built shared library for the target
# platform, then embed it as a C data array so the garlic binary is
# self-contained (extracted to temp + dlopen'd at runtime).
# ---------------------------------------------------------------------------

if(DEFINED PLATFORM_NAME)
    # Cross-compilation: PLATFORM_NAME matches the lib subdirectory
    # (linux-aarch64, macos-x64, win64, etc.)
    if(PLATFORM_NAME MATCHES "^win")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/${PLATFORM_NAME}/librosemarylib.dll")
    elseif(PLATFORM_NAME MATCHES "^macos")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/${PLATFORM_NAME}/librosemarylib.dylib")
    else()
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/${PLATFORM_NAME}/librosemarylib.so")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/linux-aarch64/librosemarylib.so")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/linux-x64/librosemarylib.so")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i686|i386")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/linux-i686/librosemarylib.so")
    else()
        message(FATAL_ERROR "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/macos-aarch64/librosemarylib.dylib")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/macos-x64/librosemarylib.dylib")
    else()
        message(FATAL_ERROR "Unsupported macOS architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows on ARM runs the x64 package. There is no separate ARM64 build.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|aarch64|arm64|ARM64")
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/win64/librosemarylib.dll")
    else()
        set(ROSEMARYLIB_FILE "${CMAKE_SOURCE_DIR}/libs/win32/librosemarylib.dll")
    endif()
else()
    message(FATAL_ERROR "Unsupported operating system: ${CMAKE_SYSTEM_NAME}")
endif()

if(ROSEMARYLIB_FILE AND EXISTS "${ROSEMARYLIB_FILE}")
    # Convert the shared library to a C source file using a pure-CMake
    # script so that no external tool (xxd, Python, etc.) is needed.
    set(EMBED_SRC ${CMAKE_BINARY_DIR}/rosemarylib_data.c)
    add_custom_command(
        OUTPUT ${EMBED_SRC}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${ROSEMARYLIB_FILE}"
                -DOUTPUT=${EMBED_SRC}
                -DVARNAME=rosemarylib_data
                -P ${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake
        MAIN_DEPENDENCY "${ROSEMARYLIB_FILE}"
        COMMENT "Embedding ${ROSEMARYLIB_FILE} into garlic binary"
    )
    add_compile_definitions(ROSEMARYLIB_EMBEDDED=1)
    message(STATUS "Embedding librosemarylib: ${ROSEMARYLIB_FILE}")
else()
    # Without a pre-built library the ELF-analysis (-n) feature is disabled
    message(STATUS "librosemarylib embedding disabled (library not found: ${ROSEMARYLIB_FILE})")
endif()

# -------------------------------------------------------------------------

# Modern Windows runners use UCRT; keep CRT paths, argv and miniz's UTF-8
# paths in the same encoding, independent of the user's system language.
if(WIN32)
    enable_language(RC)
    configure_file(cmake/windows-utf8.rc.in ${CMAKE_BINARY_DIR}/windows-utf8.rc @ONLY)
endif()

garlic_use_overlay(GARLIC_ENTRY src/garlic.c src/rosemary/rosemary_embed.c)
add_executable(garlic
        ${JAVA_DEC}
        ${GARLIC_ENTRY}
)

if(WIN32)
    target_sources(garlic PRIVATE ${CMAKE_BINARY_DIR}/windows-utf8.rc)
endif()

if(ROSEMARYLIB_FILE AND EXISTS "${ROSEMARYLIB_FILE}")
    target_sources(garlic PRIVATE ${CMAKE_BINARY_DIR}/rosemarylib_data.c)
endif()


if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(garlic pthread dl)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(garlic pthread)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
  target_link_libraries(garlic dl)
elseif (WIN32)
  target_link_libraries(garlic ws2_32 advapi32 winpthread)
endif ()


if (RELEASE)
    if (DEFINED PLATFORM_NAME)
    add_executable(garlic-${PLATFORM_NAME}
            ${JAVA_DEC}
            ${GARLIC_ENTRY}
    )

    if(WIN32)
        target_sources(garlic-${PLATFORM_NAME} PRIVATE ${CMAKE_BINARY_DIR}/windows-utf8.rc)
    endif()
    if(ROSEMARYLIB_FILE AND EXISTS "${ROSEMARYLIB_FILE}")
        target_sources(garlic-${PLATFORM_NAME} PRIVATE ${CMAKE_BINARY_DIR}/rosemarylib_data.c)
    endif()
    endif()


    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(DEFINED PLATFORM_NAME)
            target_link_libraries(garlic-${PLATFORM_NAME} pthread dl)
        else()
            target_link_libraries(garlic pthread dl)
        endif()
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(DEFINED PLATFORM_NAME)
            target_link_libraries(garlic-${PLATFORM_NAME} pthread)
        else()
            target_link_libraries(garlic pthread)
        endif()
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
        # pthread is part of bionic libc (no libpthread stub since NDK r27);
        # libdl is only needed when the embedded ELF analyzer is present.
        if(DEFINED PLATFORM_NAME)
            target_link_libraries(garlic-${PLATFORM_NAME} dl)
        else()
            target_link_libraries(garlic dl)
        endif()
    elseif (WIN32)
        if (DEFINED PLATFORM_NAME)
            target_link_libraries(garlic-${PLATFORM_NAME} ws2_32 advapi32 winpthread)
        else()
             target_link_libraries(garlic ws2_32 advapi32 winpthread)
        endif()
    endif ()

    if (DEFINED STRIP_COMMAND)
        if (DEFINED PLATFORM_NAME)
            add_custom_command(
                    TARGET garlic-${PLATFORM_NAME} POST_BUILD
                    COMMAND ${STRIP_COMMAND} $<TARGET_FILE:garlic-${PLATFORM_NAME}>
            )
        else()
            add_custom_command(
                    TARGET garlic POST_BUILD
                    COMMAND ${STRIP_COMMAND} $<TARGET_FILE:garlic>
            )
        endif()
    endif ()

endif ()

# Native desktop browser; CLI-only builds keep their existing dependencies.
option(GARLIC_BUILD_GUI "Build the C++ / Qt 6 desktop browser" OFF)
if(GARLIC_BUILD_GUI)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(gui)
endif()

# ---- tests -----------------------------------------------------------------
# `ctest --test-dir build` runs every fixture under tests/fixtures. Fixtures do
# not need a toolchain: each carries a prebuilt input plus the exact output
# garlic is expected to produce. See tests/README.md.
#option(GARLIC_BUILD_TESTS "Build the fixture test suite" ON)
#if (GARLIC_BUILD_TESTS
#    AND NOT CMAKE_CROSSCOMPILING
#    AND NOT DEFINED PLATFORM_NAME)
#    enable_testing()
#    add_subdirectory(tests)
#endif()
