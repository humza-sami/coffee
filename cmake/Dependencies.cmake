# Pinned dependencies are fetched by scripts/bootstrap.sh into .tools/deps (see DEPENDENCIES.md).
set(COFFEE_DEPS_DIR "${CMAKE_SOURCE_DIR}/.tools/deps" CACHE PATH "Directory populated by scripts/bootstrap.sh")

foreach(_dep sqlite/sqlite3.c yaml-cpp/CMakeLists.txt json/nlohmann/json.hpp httplib/httplib.h)
  if(NOT EXISTS "${COFFEE_DEPS_DIR}/${_dep}")
    message(FATAL_ERROR "Missing ${COFFEE_DEPS_DIR}/${_dep}. Run scripts/bootstrap.sh first.")
  endif()
endforeach()

# SQLite amalgamation (public domain). Compiled as C without project warning flags.
add_library(coffee_sqlite STATIC "${COFFEE_DEPS_DIR}/sqlite/sqlite3.c")
target_include_directories(coffee_sqlite SYSTEM PUBLIC "${COFFEE_DEPS_DIR}/sqlite")
target_compile_definitions(coffee_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
set_target_properties(coffee_sqlite PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(coffee_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# nlohmann/json single header (MIT)
add_library(coffee_third_party_json INTERFACE)
target_include_directories(coffee_third_party_json SYSTEM INTERFACE "${COFFEE_DEPS_DIR}/json")

# cpp-httplib single header (MIT); no OpenSSL/zlib/brotli features are enabled (plain HTTP only)
add_library(coffee_httplib INTERFACE)
target_include_directories(coffee_httplib SYSTEM INTERFACE "${COFFEE_DEPS_DIR}/httplib")
target_link_libraries(coffee_httplib INTERFACE Threads::Threads)

# yaml-cpp (MIT), static, no tests/tools
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${COFFEE_DEPS_DIR}/yaml-cpp" "${CMAKE_BINARY_DIR}/yaml-cpp" EXCLUDE_FROM_ALL SYSTEM)

# Catch2 amalgamated (BSL-1.0) for tests
add_library(coffee_catch2 STATIC "${COFFEE_DEPS_DIR}/catch2/catch_amalgamated.cpp")
target_include_directories(coffee_catch2 SYSTEM PUBLIC "${COFFEE_DEPS_DIR}/catch2")
