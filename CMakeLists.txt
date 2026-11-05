cmake_minimum_required(VERSION 3.5...3.29)
option(DUCKDB_WASM_EXTENSIONS "Whether compiling for Wasm target" OFF)

###
# Configuration
###

# CMake-only build config
if(DEFINED DUCKDB_MODULE_BASE_DIR) 
    set(EXTENSION_NAME odbc_scanner)
    set(${EXTENSION_NAME}_CMAKE_ONLY_BUILD TRUE)

    # Expected DUCKDB_VERSION formats: "v2.1.0-alpha42" (release) or "v2.1.0-dev123" (dev)
    if("${DUCKDB_VERSION}" MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-dev")
        message(STATUS "ODBC extension configured with C API V2 (unstable) dev version: ${DUCKDB_VERSION}, commit: ${GIT_COMMIT_HASH}")
        set(TARGET_DUCKDB_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(TARGET_DUCKDB_VERSION_MINOR ${CMAKE_MATCH_2})
        set(TARGET_DUCKDB_VERSION_PATCH ${CMAKE_MATCH_3}-dev)
        set(DUCKDB_EXTENSION_API_VERSION_COMBINED ${GIT_COMMIT_HASH})
    elseif("${DUCKDB_VERSION}" MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+(-[0-9A-Za-z]+)?)$")
        message(STATUS "ODBC extension configured with C API V2 (unstable) version: ${DUCKDB_VERSION}")
        set(TARGET_DUCKDB_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(TARGET_DUCKDB_VERSION_MINOR ${CMAKE_MATCH_2})
        set(TARGET_DUCKDB_VERSION_PATCH ${CMAKE_MATCH_3})
        set(DUCKDB_EXTENSION_API_VERSION_COMBINED ${DUCKDB_VERSION})
    else()
        message(WARNING "ODBC: Unexpected DUCKDB_VERSION format: '${DUCKDB_VERSION}', using 'v2.1.0-dev' instead")
        set(TARGET_DUCKDB_VERSION_MAJOR 2)
        set(TARGET_DUCKDB_VERSION_MINOR 1)
        set(TARGET_DUCKDB_VERSION_PATCH 0-dev)
        set(DUCKDB_EXTENSION_API_VERSION_COMBINED ${DUCKDB_VERSION})
    endif()
    set(DUCKDB_EXTENSION_API_VERSION_UNSTABLE 1)

    find_package(Git)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short=10 HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE ${EXTENSION_NAME}_GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        message(FATAL_ERROR "Unable to get Git version for extension: ${EXTENSION_NAME}")
    endif()
endif()
# end CMake-only build config

if(NOT DEFINED EXTENSION_NAME)
    message(FATAL_ERROR "DuckDB extension name is required")
endif()
if (DEFINED TARGET_DUCKDB_VERSION_MAJOR)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_MAJOR=${TARGET_DUCKDB_VERSION_MAJOR})
endif()
if (DEFINED TARGET_DUCKDB_VERSION_MINOR)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_MINOR=${TARGET_DUCKDB_VERSION_MINOR})
endif()
if (DEFINED TARGET_DUCKDB_VERSION_PATCH)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_PATCH=${TARGET_DUCKDB_VERSION_PATCH})
endif()
if (NOT DEFINED DUCKDB_EXTENSION_API_VERSION_COMBINED)
    set(DUCKDB_EXTENSION_API_VERSION_COMBINED v${TARGET_DUCKDB_VERSION_MAJOR}.${TARGET_DUCKDB_VERSION_MINOR}.${TARGET_DUCKDB_VERSION_PATCH})
endif()

add_definitions(-DDUCKDB_EXTENSION_NAME=${EXTENSION_NAME})

if (DEFINED DUCKDB_EXTENSION_API_VERSION_UNSTABLE)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_EXTENSION_API_VERSION_UNSTABLE})
endif()

###
# Build
###
project(${EXTENSION_NAME} LANGUAGES C CXX)

option(ODBC_NATIVE_ATTACH "Build native read-only ATTACH support (requires matching DuckDB C++ sources)" OFF)
set(ODBC_DUCKDB_SOURCE_DIR "" CACHE PATH "DuckDB v2.0.0 source checkout for native ATTACH")
set(ODBC_EXTENSION_ABI C_STRUCT_UNSTABLE)
if(ODBC_NATIVE_ATTACH)
    set(ODBC_EXTENSION_ABI CPP)
    if(NOT EXISTS "${ODBC_DUCKDB_SOURCE_DIR}/src/include/duckdb.hpp")
        message(FATAL_ERROR "ODBC_NATIVE_ATTACH requires ODBC_DUCKDB_SOURCE_DIR pointing to DuckDB v2.0.0 sources")
    endif()
    # The native interfaces and C API vtable must both match the target library exactly.
    foreach(header duckdb_v2.h duckdb_extension_v2.h)
        file(SHA256 "${ODBC_DUCKDB_SOURCE_DIR}/src/include/${header}" native_header_hash)
        file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/duckdb_capi_v2/${header}" vendored_header_hash)
        if(NOT native_header_hash STREQUAL vendored_header_hash)
            message(FATAL_ERROR "Native ATTACH requires DuckDB sources matching the vendored ${header}")
        endif()
    endforeach()
endif()

if (APPLE AND "$ENV{DUCKDB_PLATFORM}" STREQUAL osx_amd64)
    set(CMAKE_OSX_ARCHITECTURES x86_64)
    execute_process(
        COMMAND /bin/bash -c "/usr/local/bin/brew ls -v unixodbc | grep libodbc.dylib"
        OUTPUT_VARIABLE ODBC_LIBRARIES
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
        ECHO_OUTPUT_VARIABLE
    )
else()
    find_package(ODBC REQUIRED)
    message(STATUS "ODBC library found at: " ${ODBC_LIBRARIES})

    if (NOT MSVC)
        get_filename_component(ODBC_LIB_DIR ${ODBC_LIBRARIES} DIRECTORY)
        find_library(ODBCINST_LIB NAMES odbcinst libodbcinst REQUIRED HINTS ${ODBC_LIB_DIR})
    endif()
endif()

# C++ API requires C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC) 
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_BINARY_DIR})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Create Extension library
set(EXTENSION_SOURCES
    src/binary.cpp
    src/capi_entry_point.cpp
    src/columns.cpp
    src/connection.cpp
    src/dbms_quirks.cpp
    src/diagnostics.cpp
    src/driver_manager.cpp
    src/function_args.cpp
    src/odbc_scanner.cpp
    src/params.cpp
    src/registries.cpp
    src/scanner_value.cpp
    src/strings.cpp
    src/temporal.cpp
    src/widechar.cpp
    src/functions/odbc_begin_transaction.cpp
    src/functions/odbc_cancel_query.cpp
    src/functions/odbc_commit.cpp
    src/functions/odbc_close.cpp
    src/functions/odbc_connect.cpp
    src/functions/odbc_copy.cpp
    src/functions/odbc_list_data_sources.cpp
    src/functions/odbc_list_drivers.cpp
    src/functions/odbc_query.cpp
    src/functions/odbc_rollback.cpp
    src/mappings/db2_mapping.cpp
    src/mappings/firebird_mapping.cpp
    src/mappings/generic_mapping.cpp
    src/mappings/mappings.cpp
    src/mappings/mssql_mapping.cpp
    src/mappings/oracle_mapping.cpp
    src/types/blob_type.cpp
    src/types/bool_type.cpp
    src/types/decimal_type.cpp
    src/types/float_types.cpp
    src/types/integer_types.cpp
    src/types/null_type.cpp
    src/types/temporal_types.cpp
    src/types/types.cpp
    src/types/uuid_type.cpp
    src/types/varchar_type.cpp
)

if(DEFINED ${EXTENSION_NAME}_CMAKE_ONLY_BUILD)
    LIST(APPEND EXTENSION_SOURCES
        ${DUCKDB_MODULE_BASE_DIR}/tools/cpp/duckdb_cpp.cpp
    )
else()
    LIST(APPEND EXTENSION_SOURCES
        duckdb_capi_v2/cpp/duckdb_cpp.cpp
    )
endif()

if(ODBC_NATIVE_ATTACH)
    list(APPEND EXTENSION_SOURCES src/odbc_storage.cpp src/odbc_storage_metadata.cpp)
endif()
add_library(${EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})
if(ODBC_NATIVE_ATTACH)
    target_include_directories(${EXTENSION_NAME} SYSTEM PRIVATE "${ODBC_DUCKDB_SOURCE_DIR}/src/include")
    target_compile_definitions(${EXTENSION_NAME} PRIVATE DUCKDB_BUILD_LOADABLE_EXTENSION)
    # The official Linux binary disables RTTI, while macOS retains it. Match the host:
    # mixing these settings breaks both inherited vtables and exception handling.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(ODBC_NATIVE_RTTI_DEFAULT ON)
    else()
        set(ODBC_NATIVE_RTTI_DEFAULT OFF)
    endif()
    option(ODBC_NATIVE_DISABLE_RTTI "Match a DuckDB host built with DISABLE_RTTI" ${ODBC_NATIVE_RTTI_DEFAULT})
    if(ODBC_NATIVE_DISABLE_RTTI)
        if(MSVC)
            set_source_files_properties(src/odbc_storage.cpp PROPERTIES COMPILE_OPTIONS "/GR-")
        else()
            set_source_files_properties(src/odbc_storage.cpp PROPERTIES COMPILE_OPTIONS "-fno-rtti")
        endif()
    endif()
    if(APPLE)
        target_link_options(${EXTENSION_NAME} PRIVATE "-undefined" "dynamic_lookup")
    elseif(WIN32)
        set(ODBC_DUCKDB_LIBRARY "" CACHE FILEPATH "Matching DuckDB import library")
        if(NOT EXISTS "${ODBC_DUCKDB_LIBRARY}")
            message(FATAL_ERROR "Native Windows builds require ODBC_DUCKDB_LIBRARY (matching DuckDB import library)")
        endif()
        target_link_libraries(${EXTENSION_NAME} PRIVATE "${ODBC_DUCKDB_LIBRARY}")
    endif()
endif()

# C++ API calls are routed through the extension API struct
target_compile_definitions(${EXTENSION_NAME} PRIVATE DUCKDB_CPP_API_LOADABLE)

if(MSVC)
    # C++ API sources: integer conversions and local variables shadowing class members
    set_source_files_properties(duckdb_capi_v2/cpp/duckdb_cpp.cpp PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4458")
    # DUCKDB_EXTENSION_ENTRYPOINT declares the entrypoint with 'dllexport' twice
    set_source_files_properties(src/capi_entry_point.cpp PROPERTIES COMPILE_OPTIONS /wd4141)
else()
    # Extension API struct is zero-initialized in DUCKDB_EXTENSION_ENTRYPOINT
    set_source_files_properties(src/capi_entry_point.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
endif()

if(DEFINED ${EXTENSION_NAME}_CMAKE_ONLY_BUILD)
    add_dependencies(${EXTENSION_NAME} duckdb_platform)
    add_dependencies(duckdb_local_extension_repo ${EXTENSION_NAME})
    add_custom_command(
        TARGET odbc_scanner
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${EXTENSION_NAME}>
            ${CMAKE_CURRENT_BINARY_DIR}/${EXTENSION_NAME}.duckdb_extension
        COMMAND ${CMAKE_COMMAND}
            -DABI_TYPE=${ODBC_EXTENSION_ABI}
            -DEXTENSION=${CMAKE_CURRENT_BINARY_DIR}/odbc_scanner.duckdb_extension
            -DPLATFORM_FILE=${DuckDB_BINARY_DIR}/duckdb_platform_out
            -DVERSION_FIELD=${DUCKDB_EXTENSION_API_VERSION_COMBINED}
            -DEXTENSION_VERSION=${${EXTENSION_NAME}_GIT_COMMIT_HASH}
            -DNULL_FILE=${DUCKDB_MODULE_BASE_DIR}/scripts/null.txt
            -P ${DUCKDB_MODULE_BASE_DIR}/scripts/append_metadata.cmake
    )
    target_include_directories(${EXTENSION_NAME} PRIVATE
        ${DUCKDB_MODULE_BASE_DIR}/src/include
        ${DUCKDB_MODULE_BASE_DIR}/tools/cpp
    )
else()
    target_include_directories(${EXTENSION_NAME} PRIVATE
        duckdb_capi_v2
        duckdb_capi_v2/cpp
    )
endif()

# Hide symbols
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(VISIBILITY_INLINES_HIDDEN ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} -s")
elseif (APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} -Wl,-x")
endif()

target_include_directories(${EXTENSION_NAME} PRIVATE
    third_party/utf8cpp
    ${ODBC_INCLUDE_DIRS}
    src/include
)

if(NOT MSVC)
    target_compile_options(${EXTENSION_NAME} PRIVATE
        -Wall
        -Werror
        -Wextra
    )
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${EXTENSION_NAME} PRIVATE
            -O0
        )
    endif()
else()
    if(NOT DEFINED ${EXTENSION_NAME}_CMAKE_ONLY_BUILD)
        target_compile_options(${EXTENSION_NAME} PRIVATE
            /W4
            /WX
        )
    endif()
    target_compile_definitions(${EXTENSION_NAME} PRIVATE
        -D_CRT_SECURE_NO_WARNINGS
    )
endif()

# ODBC driver manager is not linked, it is loaded dynamically at runtime
target_link_libraries(${EXTENSION_NAME} PRIVATE
  ${CMAKE_DL_LIBS}
)

SET(CAPI_ERROR_MSG "C API test suite is disabled, to enable it specify DuckDB shared library in 'DUCKDB_CAPI_LIB_PATH' environment variable.")

if(ENABLE_C_API_TESTS)
    message(STATUS "C API test suite is enabled using shared lib: $ENV{DUCKDB_CAPI_LIB_PATH}")

    if(NOT DEFINED ENV{DUCKDB_CAPI_LIB_PATH} OR NOT EXISTS $ENV{DUCKDB_CAPI_LIB_PATH})
        message(FATAL_ERROR ${CAPI_ERROR_MSG})
    endif()

    add_subdirectory(test)
else()
    message(STATUS ${CAPI_ERROR_MSG})
endif()
