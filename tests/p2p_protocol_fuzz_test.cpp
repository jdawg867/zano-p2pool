#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/p2p_sync.hpp"
#include "zano_p2pool/p2p_tip.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using namespace zano_p2pool;

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x;
    }

    [[nodiscard]] std::size_t bounded(std::size_t limit) noexcept {
        return static_cast<std::size_t>(next() % limit);
    }

    [[nodiscard]] std::uint8_t byte() noexcept {
        return static_cast<std::uint8_t>(next() & 0xffU);
    }

private:
    std::uint64_t state_;
};

template <std::size_t N>
void fill_pattern(std::array<std::uint8_t, N>& value, std::uint8_t seed) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<std::uint8_t>(seed + i);
    }
}

P2pHandshake make_handshake() {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    fill_pattern(handshake.node_id, 0x10);
    handshake.capabilities = kP2pCapabilitiesV1;
    handshake.listen_port = 37888;
    fill_pattern(handshake.best_share_id, 0x50);
    handshake.best_share_height = 42;
    return handshake;
}

Share make_share() {
    Share share;
    share.version = kShareVersion1;
    share.share_height = 0;
    share.timestamp = 1'700'300'000;
    share.zano_height = 170000;
    fill_pattern(share.mining_header_hash, 0x20);
    share.nonce = UINT64_C(0x0123456789abcdef);
    share.share_difficulty = difficulty128_from_decimal("3");
    share.network_difficulty = difficulty128_from_decimal("4");
    fill_pattern(share.miner_id, 0x80);
    return share;
}

P2pMiningContextProposal make_mining_context() {
    P2pMiningContextProposal proposal;
    proposal.version = kP2pMiningContextVersion1;
    proposal.zano_height = 170000;
    fill_pattern(proposal.prev_hash, 0x30);
    proposal.network_difficulty = difficulty128_from_decimal("123456");
    fill_pattern(proposal.seed, 0x60);
    proposal.block_reward_without_fee = 10'000;
    proposal.block_reward = 10'100;
    proposal.txs_fee = 100;
    proposal.block_template_blob = {0x00, 0x01, 0x02, 0x03};
    proposal.miner_tx_tgc_json = R"({"tx_key":"00"})";
    return proposal;
}

bool semantic_parser_is_safe(const P2pEnvelope& envelope) {
    try {
        switch (envelope.type) {
        case P2pMessageType::Handshake: {
            const P2pHandshake parsed = parse_p2p_handshake_envelope(envelope);
            return make_p2p_handshake_envelope(parsed) == envelope;
        }
        case P2pMessageType::ShareAnnounce: {
            const Share parsed = parse_p2p_share_announce_envelope(envelope);
            return make_p2p_share_announce_envelope(parsed) == envelope;
        }
        case P2pMessageType::ShareRequest: {
            const ShareId parsed = parse_p2p_share_request_envelope(envelope);
            return make_p2p_share_request_envelope(parsed) == envelope;
        }
        case P2pMessageType::ShareResponse: {
            const P2pShareResponse parsed =
                parse_p2p_share_response_envelope(envelope);
            const Share* share = parsed.share.has_value() ? &*parsed.share : nullptr;
            return make_p2p_share_response_envelope(parsed.requested_id, share) ==
                   envelope;
        }
        case P2pMessageType::TipAnnounce: {
            const P2pTipHint parsed = parse_p2p_tip_announce_envelope(envelope);
            return make_p2p_tip_announce_envelope(parsed) == envelope;
        }
        case P2pMessageType::MiningContextAnnounce: {
            const P2pMiningContextProposal parsed =
                parse_p2p_mining_context_envelope(envelope);
            return make_p2p_mining_context_envelope(parsed) == envelope;
        }
        }
    } catch (const std::runtime_error&) {
        return true;
    } catch (const std::invalid_argument&) {
        return true;
    } catch (const std::out_of_range&) {
        return true;
    }
    return false;
}

bool framed_input_is_safe(const std::vector<std::uint8_t>& bytes) {
    try {
        const P2pEnvelope envelope = deserialize_p2p_envelope(bytes);
        if (serialize_p2p_envelope(envelope) != bytes) {
            return false;
        }
        return semantic_parser_is_safe(envelope);
    } catch (const std::runtime_error&) {
        return true;
    } catch (const std::invalid_argument&) {
        return true;
    } catch (const std::out_of_range&) {
        return true;
    }
}

void write_u32_be(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value) {
    CHECK(offset + 4 <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::vector<P2pEnvelope> canonical_envelopes() {
    const Share share = make_share();
    ShareId request_id{};
    fill_pattern(request_id, 0xa0);

    P2pTipHint tip;
    fill_pattern(tip.share_id, 0xc0);
    tip.share_height = 77;

    return {
        make_p2p_handshake_envelope(make_handshake()),
        make_p2p_share_announce_envelope(share),
        make_p2p_share_request_envelope(request_id),
        make_p2p_share_response_envelope(request_id, nullptr),
        make_p2p_share_response_envelope(request_id, &share),
        make_p2p_tip_announce_envelope(tip),
        make_p2p_mining_context_envelope(make_mining_context()),
    };
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    const std::vector<P2pEnvelope> seeds = canonical_envelopes();
    std::vector<std::vector<std::uint8_t>> seed_frames;
    seed_frames.reserve(seeds.size());

    // Every canonical message must survive the same parse/serialize path that
    // fuzzed inputs exercise. Then systematically truncate, append and flip
    // every byte so regressions around parser boundaries are deterministic.
    for (const P2pEnvelope& envelope : seeds) {
        CHECK(semantic_parser_is_safe(envelope));
        const std::vector<std::uint8_t> frame = serialize_p2p_envelope(envelope);
        CHECK(framed_input_is_safe(frame));
        seed_frames.push_back(frame);

        for (std::size_t cut = 0; cut < frame.size(); ++cut) {
            const std::vector<std::uint8_t> truncated(frame.begin(), frame.begin() +
                static_cast<std::ptrdiff_t>(cut));
            CHECK(framed_input_is_safe(truncated));
        }

        for (const std::uint8_t trailing : {std::uint8_t{0x00}, std::uint8_t{0xff}}) {
            std::vector<std::uint8_t> extended = frame;
            extended.push_back(trailing);
            CHECK(framed_input_is_safe(extended));
        }

        for (std::size_t i = 0; i < frame.size(); ++i) {
            for (const std::uint8_t mask :
                 {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xff}}) {
                std::vector<std::uint8_t> mutated = frame;
                mutated[i] ^= mask;
                CHECK(framed_input_is_safe(mutated));
            }
        }

        for (const std::uint32_t payload_size : {
                 std::uint32_t{0},
                 std::uint32_t{1},
                 static_cast<std::uint32_t>(kP2pMaxPayloadSize),
                 static_cast<std::uint32_t>(kP2pMaxPayloadSize + 1),
                 std::numeric_limits<std::uint32_t>::max(),
             }) {
            std::vector<std::uint8_t> mutated = frame;
            write_u32_be(mutated, 8, payload_size);
            CHECK(framed_input_is_safe(mutated));
        }
    }

    DeterministicRng rng(UINT64_C(0x7a6e6f7032703266));

    // Fuzz each semantic parser directly with valid framing and arbitrary
    // peer-controlled payload bytes. This reaches much deeper than raw random
    // frames, which usually fail at magic/version/type before message parsing.
    constexpr std::size_t kSemanticCasesPerType = 4096;
    for (std::uint8_t type_value =
             static_cast<std::uint8_t>(P2pMessageType::Handshake);
         type_value <=
             static_cast<std::uint8_t>(P2pMessageType::MiningContextAnnounce);
         ++type_value) {
        for (std::size_t case_index = 0;
             case_index < kSemanticCasesPerType;
             ++case_index) {
            P2pEnvelope envelope;
            envelope.type = static_cast<P2pMessageType>(type_value);
            const std::size_t payload_size = rng.bounded(513);
            envelope.payload.resize(payload_size);
            for (std::uint8_t& byte : envelope.payload) {
                byte = rng.byte();
            }
            CHECK(semantic_parser_is_safe(envelope));
            CHECK(framed_input_is_safe(serialize_p2p_envelope(envelope)));
        }

        P2pEnvelope oversized;
        oversized.type = static_cast<P2pMessageType>(type_value);
        oversized.payload.resize(kP2pMaxPayloadSize + 1, 0xa5);
        CHECK(semantic_parser_is_safe(oversized));
    }

    // Deterministic structured fuzzing starts from valid frames, applies several
    // independent mutations, and therefore preserves enough structure to reach
    // payload parsers while still exploring malformed lengths and boundaries.
    constexpr std::size_t kStructuredCases = 20'000;
    for (std::size_t case_index = 0; case_index < kStructuredCases; ++case_index) {
        std::vector<std::uint8_t> bytes =
            seed_frames[rng.bounded(seed_frames.size())];
        const std::size_t operations = 1 + rng.bounded(4);
        for (std::size_t operation = 0; operation < operations; ++operation) {
            switch (rng.bounded(5)) {
            case 0:
                if (!bytes.empty()) {
                    bytes[rng.bounded(bytes.size())] ^=
                        static_cast<std::uint8_t>(1U << rng.bounded(8));
                }
                break;
            case 1:
                if (!bytes.empty()) {
                    bytes[rng.bounded(bytes.size())] = rng.byte();
                }
                break;
            case 2:
                if (!bytes.empty()) {
                    bytes.resize(rng.bounded(bytes.size() + 1));
                }
                break;
            case 3: {
                const std::size_t count = 1 + rng.bounded(8);
                for (std::size_t i = 0; i < count; ++i) {
                    bytes.push_back(rng.byte());
                }
                break;
            }
            case 4:
                if (bytes.size() >= kP2pEnvelopeHeaderSize) {
                    write_u32_be(
                        bytes,
                        8,
                        static_cast<std::uint32_t>(rng.next()));
                }
                break;
            }
        }
        CHECK(framed_input_is_safe(bytes));
    }

    return 0;
}
