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
    "Path to a Zano source tree used to enable exact ProgPoWZ/Zano crypto support")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_library(zano_p2pool_core
    src/block_submitter.cpp
    src/block_template.cpp
    src/crypto_hash.cpp
    src/metrics_server.cpp
    src/mining_header.cpp
    src/p2p_miner_tx_binding.cpp
    src/p2p_miner_tx_proofs.cpp
    src/p2p_payout_policy.cpp
    src/p2p_mining_context.cpp
    src/p2p_mining_context_trust.cpp
    src/p2p_node.cpp
    src/p2p_protocol.cpp
    src/p2p_runtime.cpp
    src/p2p_share.cpp
    src/p2p_sync.cpp
    src/p2p_tip.cpp
    src/p2p_transport.cpp
    src/pplns.cpp
    src/pplns_coinbase.cpp
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
    src/zano_address.cpp
    src/zano_curve.cpp
    src/zano_miner_tx.cpp
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
    find_package(Boost REQUIRED COMPONENTS serialization)
    find_package(ZLIB REQUIRED)

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

    set(_zano_curve_sources
        "${_zano_source_dir}/src/crypto/crypto.cpp"
        "${_zano_source_dir}/src/crypto/crypto-sugar.cpp"
        "${_zano_source_dir}/src/crypto/range_proofs.cpp"
        "${_zano_source_dir}/src/crypto/one_out_of_many_proofs.cpp"
        "${_zano_source_dir}/src/crypto/zarcanum.cpp"
        "${_zano_source_dir}/src/crypto/crypto-ops.c"
        "${_zano_source_dir}/src/crypto/crypto-ops-data.c"
        "${_zano_source_dir}/src/crypto/random.c"
        "${_zano_source_dir}/src/crypto/hash.c"
        "${_zano_source_dir}/src/crypto/keccak.c"
        "${_zano_source_dir}/src/crypto/blake2b-ref.c"
    )

    if(EXISTS "${_zano_source_dir}/src/crypto/crypto.cpp")
        add_library(zano_p2pool_zano_curve STATIC ${_zano_curve_sources})
        target_include_directories(zano_p2pool_zano_curve
            PUBLIC
                "${_zano_source_dir}/src"
                "${_zano_source_dir}/contrib"
                "${_zano_source_dir}/contrib/epee/include"
                "${Boost_INCLUDE_DIRS}"
        )
        target_compile_definitions(zano_p2pool_zano_curve PRIVATE STATICLIB=1)
        target_compile_options(zano_p2pool_zano_curve PRIVATE
            -ffunction-sections
            -fdata-sections
        )
        target_link_libraries(zano_p2pool_zano_curve PUBLIC Threads::Threads)

        target_compile_definitions(zano_p2pool_core PUBLIC
            ZANO_P2POOL_HAVE_ZANO_CURVE=1
        )
        target_link_libraries(zano_p2pool_core PUBLIC zano_p2pool_zano_curve)

        message(STATUS
            "Exact Zano curve backend enabled from: ${_zano_source_dir}/src/crypto")

        # Pull only the canonical miner-transaction constructor and the
        # transaction helpers it directly needs. Function/data sections plus
        # linker GC keep unrelated daemon/database code out of P2Pool while
        # preserving the exact HF6 output/range/balance-proof implementation
        # reached by construct_miner_tx().
        add_library(zano_p2pool_zano_miner_tx STATIC
            "${_zano_source_dir}/src/currency_core/currency_format_utils.cpp"
            "${_zano_source_dir}/src/currency_core/currency_format_utils_transactions.cpp"
        )
        target_include_directories(zano_p2pool_zano_miner_tx
            PUBLIC
                "${_zano_source_dir}/src"
                "${_zano_source_dir}/contrib"
                "${_zano_source_dir}/contrib/epee/include"
                "${_zano_source_dir}/contrib/eos_portable_archive"
        )
        target_compile_definitions(zano_p2pool_zano_miner_tx PRIVATE STATICLIB=1)
        target_compile_options(zano_p2pool_zano_miner_tx PRIVATE
            -ffunction-sections
            -fdata-sections
        )
        target_link_libraries(zano_p2pool_zano_miner_tx
            PUBLIC
                zano_p2pool_zano_curve
                Boost::serialization
                Threads::Threads
                ZLIB::ZLIB
        )

        target_compile_definitions(zano_p2pool_core PUBLIC
            ZANO_P2POOL_HAVE_ZANO_MINER_TX=1
        )
        target_link_libraries(zano_p2pool_core PUBLIC zano_p2pool_zano_miner_tx)
        target_link_options(zano_p2pool_core INTERFACE -Wl,--gc-sections)

        message(STATUS
            "Exact Zano miner-tx constructor enabled from: ${_zano_source_dir}/src/currency_core/currency_format_utils.cpp")
    else()
        message(STATUS
            "Exact Zano curve backend disabled: src/crypto not present in Zano source tree")
    endif()
else()
    message(STATUS
        "Exact Zano backends disabled; set ZANO_P2POOL_ZANO_SOURCE_DIR to enable them")
endif()

add_executable(zano-p2pool src/main.cpp)
target_link_libraries(zano-p2pool PRIVATE zano_p2pool_core)

add_executable(zano-p2pool-header src/header_check.cpp)
target_link_libraries(zano-p2pool-header PRIVATE zano_p2pool_core)

if(ZANO_P2POOL_BUILD_TESTS)
    enable_testing()

    add_executable(block_submitter_test tests/block_submitter_test.cpp)
    target_link_libraries(block_submitter_test PRIVATE zano_p2pool_core)
    add_test(NAME block_submitter_test COMMAND block_submitter_test)

    add_executable(block_template_test tests/block_template_test.cpp)
    target_link_libraries(block_template_test PRIVATE zano_p2pool_core)
    add_test(NAME block_template_test COMMAND block_template_test)

    add_executable(rpc_client_test tests/rpc_client_test.cpp)
    target_link_libraries(rpc_client_test PRIVATE zano_p2pool_core)
    add_test(NAME rpc_client_test COMMAND rpc_client_test)

    add_executable(testnet_fixture_test tests/testnet_fixture_test.cpp)
    target_link_libraries(testnet_fixture_test PRIVATE zano_p2pool_core)
    target_compile_definitions(testnet_fixture_test PRIVATE
        ZANO_P2POOL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME testnet_fixture_test COMMAND testnet_fixture_test)

    add_executable(pow_target_test tests/pow_target_test.cpp)
    target_link_libraries(pow_target_test PRIVATE zano_p2pool_core)
    add_test(NAME pow_target_test COMMAND pow_target_test)

    add_executable(pplns_test tests/pplns_test.cpp)
    target_link_libraries(pplns_test PRIVATE zano_p2pool_core)
    add_test(NAME pplns_test COMMAND pplns_test)

    add_executable(pplns_coinbase_test tests/pplns_coinbase_test.cpp)
    target_link_libraries(pplns_coinbase_test PRIVATE zano_p2pool_core)
    add_test(NAME pplns_coinbase_test COMMAND pplns_coinbase_test)

    add_executable(share_validation_test tests/share_validation_test.cpp)
    target_link_libraries(share_validation_test PRIVATE zano_p2pool_core)
    add_test(NAME share_validation_test COMMAND share_validation_test)

    add_executable(progpowz_test tests/progpowz_test.cpp)
    target_link_libraries(progpowz_test PRIVATE zano_p2pool_core)
    add_test(NAME progpowz_test COMMAND progpowz_test)

    add_executable(mining_header_test tests/mining_header_test.cpp)
    target_link_libraries(mining_header_test PRIVATE zano_p2pool_core)
    add_test(NAME mining_header_test COMMAND mining_header_test)

    add_executable(metrics_server_test tests/metrics_server_test.cpp)
    target_link_libraries(metrics_server_test PRIVATE zano_p2pool_core)
    add_test(NAME metrics_server_test COMMAND metrics_server_test)

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

    add_executable(p2p_protocol_fuzz_test tests/p2p_protocol_fuzz_test.cpp)
    target_link_libraries(p2p_protocol_fuzz_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_protocol_fuzz_test COMMAND p2p_protocol_fuzz_test)

    add_executable(p2p_transport_test tests/p2p_transport_test.cpp)
    target_link_libraries(p2p_transport_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_transport_test COMMAND p2p_transport_test)

    add_executable(p2p_runtime_test tests/p2p_runtime_test.cpp)
    target_link_libraries(p2p_runtime_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_runtime_test COMMAND p2p_runtime_test)

    add_executable(p2p_node_test tests/p2p_node_test.cpp)
    target_link_libraries(p2p_node_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_node_test COMMAND p2p_node_test)

    add_executable(p2p_consensus_reorg_test tests/p2p_consensus_reorg_test.cpp)
    target_link_libraries(p2p_consensus_reorg_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_consensus_reorg_test COMMAND p2p_consensus_reorg_test)

    add_executable(p2p_share_test tests/p2p_share_test.cpp)
    target_link_libraries(p2p_share_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_share_test COMMAND p2p_share_test)

    add_executable(p2p_sync_test tests/p2p_sync_test.cpp)
    target_link_libraries(p2p_sync_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_sync_test COMMAND p2p_sync_test)

    add_executable(p2p_tip_test tests/p2p_tip_test.cpp)
    target_link_libraries(p2p_tip_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_tip_test COMMAND p2p_tip_test)

    add_executable(p2p_mining_context_test tests/p2p_mining_context_test.cpp)
    target_link_libraries(p2p_mining_context_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_mining_context_test COMMAND p2p_mining_context_test)

    add_executable(p2p_miner_tx_binding_test tests/p2p_miner_tx_binding_test.cpp)
    target_link_libraries(p2p_miner_tx_binding_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_miner_tx_binding_test COMMAND p2p_miner_tx_binding_test)

    add_executable(p2p_payout_policy_test tests/p2p_payout_policy_test.cpp)
    target_link_libraries(p2p_payout_policy_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_payout_policy_test COMMAND p2p_payout_policy_test)

    add_executable(p2p_miner_tx_proofs_test tests/p2p_miner_tx_proofs_test.cpp)
    target_link_libraries(p2p_miner_tx_proofs_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_miner_tx_proofs_test COMMAND p2p_miner_tx_proofs_test)

    add_executable(p2p_mining_context_trust_test tests/p2p_mining_context_trust_test.cpp)
    target_link_libraries(p2p_mining_context_trust_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_mining_context_trust_test COMMAND p2p_mining_context_trust_test)

    add_executable(p2p_mining_context_runtime_test tests/p2p_mining_context_runtime_test.cpp)
    target_link_libraries(p2p_mining_context_runtime_test PRIVATE zano_p2pool_core)
    add_test(NAME p2p_mining_context_runtime_test COMMAND p2p_mining_context_runtime_test)

    add_executable(zano_address_test tests/zano_address_test.cpp)
    target_link_libraries(zano_address_test PRIVATE zano_p2pool_core)
    add_test(NAME zano_address_test COMMAND zano_address_test)

    add_executable(zano_miner_tx_test tests/zano_miner_tx_test.cpp)
    target_link_libraries(zano_miner_tx_test PRIVATE zano_p2pool_core)
    add_test(NAME zano_miner_tx_test COMMAND zano_miner_tx_test)
endif()
