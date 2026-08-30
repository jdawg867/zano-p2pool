#include "test_check.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/p2p_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zano_p2pool;

[[nodiscard]] bool throws_runtime_error(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

[[nodiscard]] Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

[[nodiscard]] NodeId node_id_from(std::uint8_t seed) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return id;
}

[[nodiscard]] P2pHandshake make_handshake(std::uint8_t seed) {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    handshake.node_id = node_id_from(seed);
    handshake.capabilities = kP2pCapabilitiesV1;
    return handshake;
}

[[nodiscard]] std::vector<std::uint8_t> make_current_coinbase_suffix() {
    // Same current-HF6 suffix shape used by mining_header_test. Proof bytes are
    // dummy because this checkpoint validates transport/structural anchoring,
    // not miner-transaction cryptographic proofs.
    std::vector<std::uint8_t> suffix{0x00, 0x00, 0x02, 0x2f};
    suffix.insert(suffix.end(), 16, 0xa5);
    suffix.push_back(0x30);
    suffix.insert(suffix.end(), 96, 0x5a);
    suffix.push_back(0x00);  // zero regular transaction hashes
    return suffix;
}

[[nodiscard]] P2pMiningContextProposal make_proposal() {
    constexpr std::string_view kBlockHeaderHex =
        "030000000000000000"
        "ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807"
        "008793c8d40600";

    constexpr std::string_view kMinerPrefixHex =
        "04010096890a064e0016ccb22467a02ed1abc42f8ee99f644156e6fecf78b452d843e8ab3ce3f07692b913177a616e6f2d7032706f6f6c2d6865616465722d7465737415000b02ac950b0262aa023f001203b6984fdcb2419692ca95b836644a8f4748e68ac8f5fef53d774b07cb8dff1129c42b889b5554144a3fd721ff0be9bf1f67b8ea6b0401bdb3b6ea3aa6f5afb935a1f52c1ed634248baea8fcb4c845f8c2acbeb0955fb07d8b69b1d871960374c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626841d49dd386718298edaa6a988fd9e1f3003f00bebdd890b675d084cec18de16ef98991cb57efb7a788b63c2c521471a018dd5f09348bbfd543a0f6452c4ed4e13ebafdbed2158d6b15dada12bf0af8672379062ea9021d8fe336ee2dbd7482d3c7dd6c3745138eeaeb25942eebeab885881af074c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626837286cec4bbae098966a7225863663ba0006";

    P2pMiningContextProposal proposal;
    proposal.zano_height = 165014;
    proposal.prev_hash = hash_from_hex(
        "ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807");
    proposal.network_difficulty = difficulty128_from_decimal("1229990");
    proposal.seed = hash_from_hex(
        "f2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000");
    proposal.block_reward_without_fee = 1'000'000'000'000ULL;
    proposal.block_reward = 1'000'000'000'000ULL;
    proposal.txs_fee = 0;
    proposal.block_template_blob = hex_to_bytes(kBlockHeaderHex);
    const auto miner_prefix = hex_to_bytes(kMinerPrefixHex);
    proposal.block_template_blob.insert(
        proposal.block_template_blob.end(), miner_prefix.begin(), miner_prefix.end());
    const auto suffix = make_current_coinbase_suffix();
    proposal.block_template_blob.insert(
        proposal.block_template_blob.end(), suffix.begin(), suffix.end());
    proposal.miner_tx_tgc_json =
        R"json({"tx_key":"00112233445566778899aabbccddeeff","tx_pub_key_p":"1122","tx_outs_attr":0})json";
    return proposal;
}

[[nodiscard]] P2pMiningAnchor anchor_for(
    const P2pMiningContextProposal& proposal) {
    return P2pMiningAnchor{
        proposal.zano_height,
        proposal.prev_hash,
        proposal.network_difficulty,
        proposal.seed,
        proposal.block_reward_without_fee,
    };
}

}  // namespace

int main() {
    const P2pMiningContextProposal proposal = make_proposal();
    const P2pMiningAnchor anchor = anchor_for(proposal);
    const P2pHandshake peer = make_handshake(0x20);

    const auto payload = serialize_p2p_mining_context_payload(proposal);
    CHECK(payload.size() ==
          kP2pMiningContextFixedPayloadSize +
              proposal.block_template_blob.size() +
              proposal.miner_tx_tgc_json.size());
    CHECK(payload.size() <= kP2pMaxPayloadSize);
    CHECK(deserialize_p2p_mining_context_payload(payload) == proposal);

    const P2pEnvelope envelope = make_p2p_mining_context_envelope(proposal);
    CHECK(envelope.type == P2pMessageType::MiningContextAnnounce);
    CHECK(envelope.payload == payload);
    CHECK(parse_p2p_mining_context_envelope(envelope) == proposal);

    const auto wire = serialize_p2p_envelope(envelope);
    const auto decoded_envelope = deserialize_p2p_envelope(wire);
    CHECK(decoded_envelope == envelope);

    const P2pMiningContextId context_id = p2p_mining_context_id(proposal);
    CHECK(context_id ==
          p2p_mining_context_id(parse_p2p_mining_context_envelope(envelope)));
    CHECK(std::any_of(context_id.begin(), context_id.end(),
                      [](std::uint8_t byte) { return byte != 0; }));

    const auto inspected = inspect_p2p_mining_context(peer, envelope, anchor);
    CHECK(inspected.status ==
          P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);
    CHECK(inspected.proposal_id == context_id);
    CHECK(hash_to_hex(inspected.mining_header_hash) ==
          "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e");
    CHECK(inspected.regular_transaction_count == 0);
    CHECK(std::string(p2p_mining_context_check_status_name(inspected.status)) ==
          "anchored-unverified-miner-tx");

    // Anchoring is deliberately not trust promotion. There is no registry
    // parameter in inspect_p2p_mining_context(), and an unrelated trusted-work
    // registry remains empty after a successful inspection.
    P2pTrustedWorkRegistry trusted_work;
    CHECK(trusted_work.size() == 0);
    static_cast<void>(inspect_p2p_mining_context(peer, envelope, anchor));
    CHECK(trusted_work.size() == 0);

    P2pHandshake no_context_cap = peer;
    no_context_cap.capabilities &= ~kP2pCapabilityMiningContext;
    const auto no_cap = inspect_p2p_mining_context(
        no_context_cap, envelope, anchor);
    CHECK(no_cap.status == P2pMiningContextCheckStatus::CapabilityMissing);

    P2pMiningAnchor wrong_anchor = anchor;
    ++wrong_anchor.zano_height;
    CHECK(inspect_p2p_mining_context(peer, envelope, wrong_anchor).status ==
          P2pMiningContextCheckStatus::AnchorMismatch);
    wrong_anchor = anchor;
    wrong_anchor.prev_hash[0] ^= 0x01U;
    CHECK(inspect_p2p_mining_context(peer, envelope, wrong_anchor).status ==
          P2pMiningContextCheckStatus::AnchorMismatch);
    wrong_anchor = anchor;
    wrong_anchor.network_difficulty = difficulty128_from_decimal("1229991");
    CHECK(inspect_p2p_mining_context(peer, envelope, wrong_anchor).status ==
          P2pMiningContextCheckStatus::AnchorMismatch);
    wrong_anchor = anchor;
    wrong_anchor.seed[0] ^= 0x01U;
    CHECK(inspect_p2p_mining_context(peer, envelope, wrong_anchor).status ==
          P2pMiningContextCheckStatus::AnchorMismatch);
    wrong_anchor = anchor;
    ++wrong_anchor.block_reward_without_fee;
    CHECK(inspect_p2p_mining_context(peer, envelope, wrong_anchor).status ==
          P2pMiningContextCheckStatus::AnchorMismatch);

    P2pMiningContextProposal nonzero_nonce = proposal;
    nonzero_nonce.block_template_blob[1] = 1;
    CHECK(throws_runtime_error([&] {
        (void)inspect_p2p_mining_context(
            peer,
            make_p2p_mining_context_envelope(nonzero_nonce),
            anchor_for(nonzero_nonce));
    }));

    P2pMiningContextProposal blob_prev_mismatch = proposal;
    blob_prev_mismatch.block_template_blob[9] ^= 0x01U;
    CHECK(throws_runtime_error([&] {
        (void)inspect_p2p_mining_context(
            peer,
            make_p2p_mining_context_envelope(blob_prev_mismatch),
            anchor);
    }));

    // Change only proposal/anchor height, leaving txin_gen.height in the blob at
    // 165014. Metadata therefore anchors but the canonical coinbase binding fails.
    P2pMiningContextProposal miner_height_mismatch = proposal;
    ++miner_height_mismatch.zano_height;
    CHECK(throws_runtime_error([&] {
        (void)inspect_p2p_mining_context(
            peer,
            make_p2p_mining_context_envelope(miner_height_mismatch),
            anchor_for(miner_height_mismatch));
    }));

    P2pMiningContextProposal bad_tgc = proposal;
    bad_tgc.miner_tx_tgc_json = "{}";
    CHECK(throws_runtime_error([&] {
        (void)make_p2p_mining_context_envelope(bad_tgc);
    }));
    bad_tgc.miner_tx_tgc_json = "not-json";
    CHECK(throws_runtime_error([&] {
        (void)make_p2p_mining_context_envelope(bad_tgc);
    }));

    P2pEnvelope truncated = envelope;
    truncated.payload.pop_back();
    CHECK(throws_runtime_error([&] {
        (void)parse_p2p_mining_context_envelope(truncated);
    }));

    P2pEnvelope bad_lengths = envelope;
    // block-template length field begins at byte 113 of the fixed payload.
    bad_lengths.payload[113] = 0xff;
    bad_lengths.payload[114] = 0xff;
    bad_lengths.payload[115] = 0xff;
    bad_lengths.payload[116] = 0xff;
    CHECK(throws_runtime_error([&] {
        (void)parse_p2p_mining_context_envelope(bad_lengths);
    }));

    // The live BlockTemplate helpers retain the same anchor/proposal metadata.
    BlockTemplate block_template;
    block_template.height = proposal.zano_height;
    block_template.prev_hash = hash_to_hex(proposal.prev_hash);
    block_template.difficulty = difficulty128_to_decimal(proposal.network_difficulty);
    block_template.seed = hash_to_hex(proposal.seed);
    block_template.block_reward_without_fee = proposal.block_reward_without_fee;
    block_template.block_reward = proposal.block_reward;
    block_template.txs_fee = proposal.txs_fee;
    block_template.blocktemplate_blob = bytes_to_hex(proposal.block_template_blob);
    block_template.miner_tx_tgc_json = proposal.miner_tx_tgc_json;
    CHECK(p2p_mining_anchor_from_template(block_template) == anchor);
    CHECK(p2p_mining_context_proposal_from_template(block_template) == proposal);

    // Real bounded TCP transport accepts type 6 after the normal v1 handshake.
    const P2pHandshake server_handshake = make_handshake(0x50);
    const P2pHandshake client_handshake = make_handshake(0x90);
    P2pTcpListener listener(
        P2pEndpoint{"127.0.0.1", 0}, server_handshake);
    listener.start();
    auto accepted = std::async(std::launch::async, [&listener] {
        return listener.accept_peer();
    });
    P2pTcpConnection outbound = connect_p2p_peer(
        P2pEndpoint{"127.0.0.1", listener.port()}, client_handshake);
    CHECK(accepted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    P2pTcpConnection inbound = accepted.get();

    outbound.send_envelope(envelope);
    const P2pEnvelope received = inbound.receive_envelope();
    const auto live_result = inspect_p2p_mining_context(
        inbound.peer_handshake(), received, anchor);
    CHECK(live_result.status ==
          P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);
    CHECK(live_result.mining_header_hash == inspected.mining_header_hash);

    outbound.close();
    inbound.close();
    listener.stop();

    return 0;
}
