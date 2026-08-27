cmake_minimum_required(VERSION 3.20)

project(zano_p2pool
    VERSION 0.1.0
    DESCRIPTION "Experimental decentralized P2Pool implementation for Zano"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZANO_P2POOL_BUILD_TESTS "Build zano-p2pool tests" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_library(zano_p2pool_core
    src/block_template.cpp
    src/rpc_client.cpp
)

target_include_directories(zano_p2pool_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(zano_p2pool_core
    PUBLIC
        PkgConfig::CURL
        PkgConfig::JSONC
)

target_compile_options(zano_p2pool_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(zano-p2pool src/main.cpp)
target_link_libraries(zano-p2pool PRIVATE zano_p2pool_core)

if(ZANO_P2POOL_BUILD_TESTS)
    enable_testing()

    add_executable(block_template_test tests/block_template_test.cpp)
    target_link_libraries(block_template_test PRIVATE zano_p2pool_core)

    add_test(NAME block_template_test COMMAND block_template_test)
endif()
