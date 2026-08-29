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
set(ZANO_P2POOL_ZANO_SOURCE_DIR "" CACHE PATH
    "Path to a Zano source tree used to enable exact ProgPoWZ support")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_library(zano_p2pool_core
    src/block_template.cpp
    src/crypto_hash.cpp
    src/mining_header.cpp
    src/p2p_protocol.cpp
    src/p2p_share.cpp
    src/p2p_sync.cpp
    src/p2p_transport.cpp
    src/pow_target.cpp
    src/progpowz.cpp
    src/rpc_client.cpp
    src/share.cpp
    src/share_chain.cpp
    src/share_validation.cpp
    src/stratum_protocol.cpp
    src/stratum_server.cpp
    src/stratum_session.cpp
    src/stratum_submission.cpp
    src/template_refresh.cpp
)

target_include_directories(zano_p2pool_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(zano_p2pool_core
    PUBLIC
        PkgConfig::CURL
        PkgConfig::JSONC
        Threads::Threads
)

target_compile_options(zano_p2pool_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(ZANO_P2POOL_ZANO_SOURCE_DIR)
    get_filename_component(
        _zano_source_dir
        "${ZANO_P2POOL_ZANO_SOURCE_DIR}"
        ABSOLUTE
    )
    set(_zano_libethash_dir
        "${_zano_source_dir}/contrib/ethereum/libethash")

    if(NOT EXISTS "${_zano_libethash_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "ZANO_P2POOL_ZANO_SOURCE_DIR does not point to a Zano source tree: "
            "${_zano_source_dir}")
    endif()

    enable_language(C)

    add_subdirectory(
        "${_zano_libethash_dir}"
        "${CMAKE_BINARY_DIR}/zano-libethash"
        EXCLUDE_FROM_ALL
    )

    target_include_directories(ethash PUBLIC "${_zano_libethash_dir}")
    target_link_libraries(ethash PUBLIC Threads::Threads)

    target_compile_definitions(zano_p2pool_core PUBLIC
        ZANO_P2POOL_HAVE_PROGPOWZ=1
    )
    target_link_libraries(zano_p2pool_core PUBLIC ethash)

    message(STATUS
        "ProgPoWZ enabled from Zano source: ${_zano_libethash_dir}")
else()
    message(STATUS
        "ProgPoWZ backend disabled; set ZANO_P2POOL_ZANO_SOURCE_DIR to enable it")
endif()

add_executable(zano-p2pool src/main.cpp)
target_link_libraries(zano-p2pool PRIVATE zano_p2pool_core)

add_executable(zano-p2pool-header src/header_check.cpp)
target_link_libraries(zano-p2pool-header PRIVATE zano_p2pool_core)

if(ZANO_P2POOL_BUILD_TESTS)
    enable_testing()

    add_executable(block_template_test tests/block_template_test.cpp)
    target_link_libraries(block_template_test PRIVATE zano_p2pool_core)
    add_test(NAME block_template_test COMMAND block_template_test)

    add_executable(testnet_fixture_test tests/testnet_fixture_test.cpp)
    target_link_libraries(testnet_fixture_test PRIVATE zano_p2pool_core)
    target_compile_definitions(testnet_fixture_test PRIVATE
        ZANO_P2POOL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME testnet_fixture_test COMMAND testnet_fixture_test)

    add_executable(pow_target_test tests/pow_target_test.cpp)
    target_link_libraries(pow_target_test PRIVATE zano_p2pool_core)
    add_test(NAME pow_target_test COMMAND pow_target_test)

    add_executable(share_validation_test tests/share_validation_test.cpp)
    target_link_libraries(share_validation_test PRIVATE zano_p2pool_core)
    add_test(NAME share_validation_test COMMAND share_validation_test)

    add_executable(progpowz_test tests/progpowz_test.cpp)
    target_link_libraries(progpowz_test PRIVATE zano_p2pool_core)
    add_test(NAME progpowz_test COMMAND progpowz_test)

    add_executable(mining_header_test tests/mining_header_test.cpp)
    target_link_libraries(mining_header_test PRIVATE zano_p2pool_core)
    add_test(NAME mining_header_test COMMAND mining_header_test)

    add_executable(share_test tests/share_test.cpp)
    target_link_libraries(share_test PRIVATE zano_p2pool_core)
    add_test(NAME share_test COMMAND share_test)

    add_executable(share_chain_test tests/share_chain_test.cpp)
    target_link_libraries(share_chain_test PRIVATE zano_p2pool_core)
    add_test(NAME share_chain_test COMMAND share_chain_test)

    add_executable(stratum_protocol_test tests/stratum_protocol_test.cpp)
    target_link_libraries(stratum_protocol_test PRIVATE zano_p2pool_core)
    add_test(NAME stratum_protocol_test COMMAND stratum_protocol_test)

    add_executable(stratum_session_test tests/stratum_session_test.cpp)
    target_link_libraries(stratum_session_test PRIVATE zano_p2pool_core)
    add_test(NAME stratum_session_test COMMAND stratum_session_test)

    add_executable(stratum_submission_test tests/stratum_submission_test.cpp)
    target_link_libraries(stratum_submission_test PRIVATE zano_p2pool_core)
    add_test(NAME stratum_submission_test COMMAND stratum_submission_test)

    add_executable(stratum_server_test tests/stratum_server_test.cpp)
    target_link_libraries(stratum_server_test PRIVATE zano_p2pool_core)
    add_test(NAME stratum_server_test COMMAND stratum_server_test)

    add_executable(template_refresh_test tests/template_refresh_test.cpp)
    target_link_libraries(template_refresh_test PRIVATE zano_p2pool_core)
    add_test(NAME template_refresh_test COMMAND template_refresh_test)

    add_executable(p2p_protocol_test tests/p2p_protocol_test.cpp)
    target_link_libraries(p2p_protocol_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_protocol_test COMMAND p2p_protocol_test)

    add_executable(p2p_transport_test tests/p2p_transport_test.cpp)
    target_link_libraries(p2p_transport_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_transport_test COMMAND p2p_transport_test)

    add_executable(p2p_share_test tests/p2p_share_test.cpp)
    target_link_libraries(p2p_share_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_share_test COMMAND p2p_share_test)

    add_executable(p2p_sync_test tests/p2p_sync_test.cpp)
    target_link_libraries(p2p_sync_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_sync_test COMMAND p2p_sync_test)
endif()
