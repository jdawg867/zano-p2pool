#include "zano_p2pool/p2p_work_retrieval.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "hf6_test_suffix.hpp"
#include "test_check.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <unistd.h>

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
    handshake.capabilities = kP2pCapabilitiesV1 | kP2pCapabilityWorkRetrieval;
    return handshake;
}

[[nodiscard]] std::vector<std::uint8_t> make_current_coinbase_suffix() {
    // Same current-HF6 suffix shape used by mining_header_test. Proof bytes are
    // dummy because this checkpoint validates transport/structural anchoring,
    // not miner-transaction cryptographic proofs.
    std::vector<std::uint8_t> suffix{0x00, 0x00, 0x02, 0x2f};
    const auto range = make_structural_range_proof();
    suffix.insert(suffix.end(), range.begin(), range.end());
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


struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::string pattern=(std::filesystem::temp_directory_path()/"zano-retrieval-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        path=pattern;
    }
    ~Temporary() { std::filesystem::remove_all(path); }
};
bool wait_for(const std::function<bool()>& fn) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (std::chrono::steady_clock::now()<deadline) {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fn();
}
} // namespace
int main() {
    Temporary temp;
    Hash256 chain_id{}; chain_id[0]=1;
    MiningWorkArchive provider_archive(temp.path/"provider",chain_id);
    MiningWorkArchive receiver_archive(temp.path/"receiver",chain_id);
    auto proposal=make_proposal();
    // Exercise a maximum-size archived proposal: framing must not force a
    // reduction in the existing 64 KiB mining-context bound.
    const auto short_size=serialize_p2p_mining_context_payload(proposal).size();
    proposal.miner_tx_tgc_json.append(kP2pMaxPayloadSize-short_size,' ');
    const auto payload=serialize_p2p_mining_context_payload(proposal);
    CHECK(payload.size()==kP2pMaxPayloadSize);
    const auto context_id=provider_archive.put(payload);
    P2pWorkRetrieval provider(provider_archive); // restart rebuilds lookup
    P2pWorkRetrieval receiver(receiver_archive);
    const auto peer=make_handshake(10);
    const MiningWorkKey key{proposal.zano_height,derive_mining_header_work(proposal.block_template_blob).header_hash};

    // Restart-built local evidence must be retrievable only by its exact
    // height/header key. Reading it does not copy anything into a peer cache or
    // grant trusted-work authority.
    const auto local_payload=provider.read_local(key);
    CHECK(local_payload);
    CHECK(*local_payload==payload);

    auto wrong_local_height=key;
    ++wrong_local_height.first;
    CHECK(!provider.read_local(wrong_local_height));

    auto wrong_local_header=key;
    wrong_local_header.second[0]^=1;
    CHECK(!provider.read_local(wrong_local_header));

    CHECK(!receiver.read_local(key));
    CHECK(receiver_archive.verify_all()==0);

    auto request=receiver.begin(peer,key,100);
    CHECK(request);
    CHECK(!receiver.begin(peer,key,100));
    std::size_t chunks=0;
    while (request) {
        auto response=provider.answer(peer,*request);
        response=deserialize_p2p_envelope(serialize_p2p_envelope(response));
        const auto result=receiver.receive(peer,response,101);
        request=result.followup;
        if (!request) CHECK(result.received_id==context_id);
        ++chunks;
    }
    CHECK(chunks==4);
    CHECK(receiver.take_untrusted(key)==payload);
    CHECK(receiver_archive.verify_all()==0); // peer evidence is not local trust
    CHECK(!receiver.begin(peer,key,102)); // completed request cooldown

    auto unknown=key; ++unknown.first;
    auto unknown_request=receiver.begin(peer,unknown,102);
    CHECK(unknown_request);
    auto not_found=provider.answer(peer,*unknown_request);
    CHECK(parse_mining_work_response(not_found).total_size==0);
    CHECK(!receiver.receive(peer,not_found,102).received_id);
    CHECK(!receiver.begin(peer,unknown,103));

    // Finished requests remain same-key cooldown entries, but they must not
    // consume the two active-request slots for this peer. Historical ancestry
    // recovery can require more than two sequential work keys.
    auto third_key=key;
    third_key.first+=2;
    auto third_request=receiver.begin(peer,third_key,103);
    CHECK(third_request);
    auto third_not_found=provider.answer(peer,*third_request);
    CHECK(parse_mining_work_response(third_not_found).total_size==0);
    CHECK(!receiver.receive(peer,third_not_found,103).received_id);

    // The bounded pending table must also continue making sequential progress
    // beyond kMiningWorkMaxPending completed cooldown entries. Finished entries
    // may be recycled, while the existing all-active hard limit stays intact.
    P2pWorkRetrieval sequential(receiver_archive);
    for (std::size_t i=0;i<kMiningWorkMaxPending+4;++i) {
        auto sequential_key=key;
        sequential_key.first+=100+i;
        auto sequential_request=
            sequential.begin(peer,sequential_key,300);
        CHECK(sequential_request);
        auto sequential_not_found=
            provider.answer(peer,*sequential_request);
        CHECK(parse_mining_work_response(
                  sequential_not_found).total_size==0);
        CHECK(!sequential.receive(
                  peer,sequential_not_found,300).received_id);
    }

    auto old_peer=peer; old_peer.capabilities=kP2pCapabilitiesV1;
    CHECK(!receiver.begin(old_peer,key,200));
    CHECK(throws_runtime_error([&] { static_cast<void>(provider.answer(old_peer,*unknown_request)); }));

    P2pWorkRetrieval adversarial(receiver_archive);
    auto first=adversarial.begin(peer,key,100);
    auto response=provider.answer(peer,*first);
    auto wrong_peer=make_handshake(90);
    CHECK(throws_runtime_error([&] { static_cast<void>(adversarial.receive(wrong_peer,response,100)); }));
    CHECK(throws_runtime_error([&] { static_cast<void>(adversarial.receive(peer,response,160)); }));
    CHECK(adversarial.begin(peer,key,161));
    auto wrong_offset=parse_mining_work_response(response);
    wrong_offset.request.offset=kMiningWorkChunkSize;
    CHECK(throws_runtime_error([&] { static_cast<void>(adversarial.receive(peer,make_mining_work_response(wrong_offset),161)); }));
    CHECK(!adversarial.begin(peer,key,162));
    auto malformed=response; malformed.payload.pop_back();
    CHECK(throws_runtime_error([&] { static_cast<void>(parse_mining_work_response(malformed)); }));
    malformed=response; malformed.flags=1;
    CHECK(throws_runtime_error([&] { static_cast<void>(parse_mining_work_response(malformed)); }));
    CHECK(throws_runtime_error([&] { static_cast<void>(make_mining_work_request({key,1})); }));

    // A well-formed proposal for a different requested header is still rejected.
    P2pWorkRetrieval mismatch(receiver_archive);
    auto wrong_key=key; wrong_key.second[0]^=1;
    CHECK(mismatch.begin(peer,wrong_key,100));
    for (std::uint32_t offset=0; offset<payload.size(); offset+=kMiningWorkChunkSize) {
        MiningWorkResponse part{{wrong_key,offset},static_cast<std::uint32_t>(payload.size()),
            {payload.begin()+offset,payload.begin()+offset+kMiningWorkChunkSize}};
        if (offset+part.chunk.size()==payload.size()) {
            CHECK(throws_runtime_error([&] { static_cast<void>(mismatch.receive(peer,make_mining_work_response(part),101)); }));
        } else CHECK(mismatch.receive(peer,make_mining_work_response(part),101).followup);
    }
    CHECK(!mismatch.take_untrusted(wrong_key));
    P2pWorkRetrieval limits(receiver_archive);
    for (std::size_t i=0;i<kMiningWorkMaxPending;++i) {
        auto p=make_handshake(static_cast<std::uint8_t>(i+1));
        CHECK(limits.begin(p,key,100));
    }
    CHECK(!limits.begin(make_handshake(99),key,100));
    CHECK(limits.begin(make_handshake(99),key,160));

    // Real socket path: unknown ShareAnnounce -> work request -> chunked
    // response, with no change to the receiver's trusted registry or chain.
    P2pWorkRetrieval socket_receiver(receiver_archive);
    ShareChain sender_chain,receiver_chain;
    P2pTrustedWorkRegistry sender_trust,receiver_trust;
    std::mutex sender_mutex,receiver_mutex;
    P2pNodeProtocol sender_node(sender_chain,sender_trust,sender_mutex);
    P2pNodeProtocol receiver_node(receiver_chain,receiver_trust,receiver_mutex);
    sender_node.set_work_retrieval(&provider);
    receiver_node.set_work_retrieval(&socket_receiver);
    P2pRuntime *sender_ptr=nullptr,*receiver_ptr=nullptr;
    std::atomic<bool> completed{false},failed{false};
    P2pRuntime sender_runtime(P2pRuntimeConfig{P2pEndpoint{"127.0.0.1",0},make_handshake(30)},
        [&](const P2pHandshake& p,const P2pEnvelope& e) {
            try { static_cast<void>(sender_node.handle(*sender_ptr,p,e,100)); }
            catch (...) { failed=true; }
        });
    P2pRuntime receiver_runtime(P2pRuntimeConfig{P2pEndpoint{"127.0.0.1",0},make_handshake(60)},
        [&](const P2pHandshake& p,const P2pEnvelope& e) {
            try { if (receiver_node.handle(*receiver_ptr,p,e,100).untrusted_work_received) completed=true; }
            catch (...) { failed=true; }
        });
    sender_ptr=&sender_runtime; receiver_ptr=&receiver_runtime;
    sender_runtime.start(); receiver_runtime.start();
    receiver_runtime.connect_peer({"127.0.0.1",sender_runtime.listen_port()});
    CHECK(wait_for([&] { return sender_runtime.peer_count()==1 && receiver_runtime.peer_count()==1; }));
    Share share; share.zano_height=key.first; share.mining_header_hash=key.second;
    share.share_difficulty=difficulty128_from_decimal("1");
    share.network_difficulty=proposal.network_difficulty;
    sender_runtime.broadcast(make_p2p_share_announce_envelope(share));
    CHECK(wait_for([&] { return completed.load() || failed.load(); }));
    sender_runtime.stop(); receiver_runtime.stop();
    CHECK(completed && !failed);
    CHECK(socket_receiver.take_untrusted(key)==payload);
    CHECK(receiver_chain.connected_size()==0 && receiver_chain.orphan_size()==0);
    CHECK(receiver_trust.size()==0);
    return 0;
}
