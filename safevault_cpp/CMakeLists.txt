cmake_minimum_required(VERSION 3.16)
project(fileguard CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ─────────────────────────────────────────────
# SQLite3 amalgamation (bundled)
# ─────────────────────────────────────────────
add_library(sqlite3_lib STATIC third_party/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC third_party)
if(UNIX)
    target_link_libraries(sqlite3_lib PUBLIC dl pthread)
endif()
if(MSVC)
    target_compile_options(sqlite3_lib PRIVATE /W0)
else()
    target_compile_options(sqlite3_lib PRIVATE -w)
endif()

# ─────────────────────────────────────────────
# FileGuard Core Library
# ─────────────────────────────────────────────
set(CORE_SOURCES
    src/journal.cpp
    src/passthrough.cpp
    src/search.cpp
    src/watch.cpp
    src/sha256.cpp
    src/filesystem_service.cpp
    src/recovery_service.cpp
    src/command_manager.cpp
    src/event_monitor.cpp
    src/benchmark_service.cpp
    src/api_server.cpp
    tests/test_main.cpp
)

add_library(fileguard_core STATIC ${CORE_SOURCES})
target_compile_definitions(fileguard_core PRIVATE FILEGUARD_NO_TEST_MAIN)
target_include_directories(fileguard_core PUBLIC
    include
    third_party
)
target_link_libraries(fileguard_core PUBLIC sqlite3_lib)

if(WIN32)
    target_link_libraries(fileguard_core PUBLIC winhttp ws2_32)
    target_compile_definitions(fileguard_core PUBLIC
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

if(UNIX)
    target_link_libraries(fileguard_core PUBLIC pthread)
endif()

if(MSVC)
    target_compile_options(fileguard_core PRIVATE /W3 /wd4996)
else()
    target_compile_options(fileguard_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ─────────────────────────────────────────────
# SafeVault / FileGuard executable (CLI & API server)
# ─────────────────────────────────────────────
add_executable(fileguard_server src/main.cpp)
target_link_libraries(fileguard_server PRIVATE fileguard_core)
add_executable(safevault src/main.cpp)
target_link_libraries(safevault PRIVATE fileguard_core)

# ─────────────────────────────────────────────
# Automated Test Suite executable
# ─────────────────────────────────────────────
add_executable(fileguard_tests tests/test_main.cpp)
target_link_libraries(fileguard_tests PRIVATE fileguard_core)
