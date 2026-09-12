#include "zano_p2pool/p2p_work_retrieval.hpp"
#include "zano_p2pool/mining_header.hpp"
#include <algorithm>
#include <stdexcept>

namespace zano_p2pool {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void append(std::vector<std::uint8_t>& out, std::uint64_t n, unsigned bytes) {
    for (unsigned i=bytes; i>0; --i) out.push_back(static_cast<std::uint8_t>(n >> ((i-1)*8)));
}
std::uint64_t integer(std::span<const std::uint8_t> bytes) {
    std::uint64_t n=0; for (auto b: bytes) n=(n<<8)|b; return n;
}
void check_envelope(const P2pEnvelope& e, P2pMessageType type) {
    require(e.version==kP2pProtocolVersion && e.type==type && e.flags==0 &&
            e.payload.size()<=kP2pMaxPayloadSize, "invalid work-retrieval envelope");
}
void check_peer(const P2pHandshake& peer) {
    require((peer.capabilities & kP2pCapabilityWorkRetrieval)!=0, "peer lacks work-retrieval capability");
}
void check_request(const MiningWorkRequest& request) {
    require(request.key.second != Hash256{} && request.offset < kP2pMaxPayloadSize &&
            request.offset % kMiningWorkChunkSize == 0, "invalid work request key/offset");
}
MiningWorkRequest decode_request(std::span<const std::uint8_t> p) {
    MiningWorkRequest r;
    r.key.first=integer(p.first(8));
    std::copy_n(p.begin()+8,32,r.key.second.begin());
    r.offset=static_cast<std::uint32_t>(integer(p.subspan(40,4)));
    check_request(r); return r;
}
MiningWorkKey checked_key(const P2pMiningContextProposal& proposal) {
    return {proposal.zano_height, derive_mining_header_work(proposal.block_template_blob).header_hash};
}
void check_response(const MiningWorkResponse& r) {
    check_request(r.request);
    if (r.total_size==0) { require(r.chunk.empty(),"not-found work response contains data"); return; }
    require(r.total_size>=kP2pMiningContextFixedPayloadSize && r.total_size<=kP2pMaxPayloadSize &&
            r.request.offset<r.total_size, "invalid work response total");
    require(r.chunk.size()==std::min<std::size_t>(kMiningWorkChunkSize,r.total_size-r.request.offset),
            "invalid work response chunk size");
}
}
P2pEnvelope make_mining_work_request(const MiningWorkRequest& r) {
    check_request(r);
    P2pEnvelope e; e.type=P2pMessageType::MiningWorkRequest;
    append(e.payload,r.key.first,8);
    e.payload.insert(e.payload.end(),r.key.second.begin(),r.key.second.end());
    append(e.payload,r.offset,4); return e;
}
MiningWorkRequest parse_mining_work_request(const P2pEnvelope& e) {
    check_envelope(e,P2pMessageType::MiningWorkRequest);
    require(e.payload.size()==44,"invalid work request length");
    return decode_request(e.payload);
}
P2pEnvelope make_mining_work_response(const MiningWorkResponse& r) {
    check_response(r);
    auto e=make_mining_work_request(r.request); e.type=P2pMessageType::MiningWorkResponse;
    append(e.payload,r.total_size,4);
    e.payload.insert(e.payload.end(),r.chunk.begin(),r.chunk.end()); return e;
}
MiningWorkResponse parse_mining_work_response(const P2pEnvelope& e) {
    check_envelope(e,P2pMessageType::MiningWorkResponse);
    require(e.payload.size()>=48 && e.payload.size()<=48+kMiningWorkChunkSize,"invalid work response length");
    MiningWorkResponse r; r.request=decode_request(e.payload);
    r.total_size=static_cast<std::uint32_t>(integer(std::span<const std::uint8_t>(e.payload).subspan(44,4)));
    r.chunk.assign(e.payload.begin()+48,e.payload.end()); check_response(r); return r;
}
P2pWorkRetrieval::P2pWorkRetrieval(MiningWorkArchive& archive): archive_(archive) {
    // Build a read-only lookup from complete local records after restart.
    static_cast<void>(archive_.verify_all());
    for (const auto& entry: std::filesystem::directory_iterator(archive_.path())) {
        if (entry.path().extension()!=".work") continue;
        const auto name=hex_to_bytes(entry.path().stem().string());
        Hash256 id{}; std::copy(name.begin(),name.end(),id.begin());
        const auto proposal=deserialize_p2p_mining_context_payload(archive_.read(id));
        local_.try_emplace(checked_key(proposal),id);
    }
}
void P2pWorkRetrieval::remember_local(const P2pMiningContextProposal& proposal) {
    const auto id=p2p_mining_context_id(proposal);
    const auto key=checked_key(proposal);
    std::lock_guard lock(mutex_); local_.try_emplace(key,id);
}
void P2pWorkRetrieval::expire(std::uint64_t now) {
    for (auto it=pending_.begin(); it!=pending_.end();) {
        if (now<it->second.started || now-it->second.started>=kMiningWorkRequestLifetime)
            it=pending_.erase(it);
        else ++it;
    }
}
std::optional<P2pEnvelope> P2pWorkRetrieval::begin(
    const P2pHandshake& peer,const MiningWorkKey& key,std::uint64_t now) {
    if (!(peer.capabilities & kP2pCapabilityWorkRetrieval)) return std::nullopt;
    const auto request=make_mining_work_request({key,0});
    std::lock_guard lock(mutex_); expire(now);
    if (received_.contains(key) || pending_.contains({peer.node_id,key}) ||
        pending_.size()>=kMiningWorkMaxPending) return std::nullopt;
    const auto peer_count=std::count_if(pending_.begin(),pending_.end(),[&](const auto& item) {
        return item.first.first==peer.node_id;
    });
    if (peer_count>=2) return std::nullopt;
    pending_.emplace(std::make_pair(peer.node_id,key),Pending{now,0,false,{}});
    return request;
}
P2pEnvelope P2pWorkRetrieval::answer(const P2pHandshake& peer,const P2pEnvelope& envelope) {
    check_peer(peer); const auto request=parse_mining_work_request(envelope);
    std::optional<Hash256> id;
    { std::lock_guard lock(mutex_); const auto it=local_.find(request.key); if (it!=local_.end()) id=it->second; }
    MiningWorkResponse response{request,0,{}};
    if (id) {
        const auto payload=archive_.read(*id);
        require(request.offset<payload.size(),"work request offset beyond record");
        response.total_size=static_cast<std::uint32_t>(payload.size());
        const auto count=std::min<std::size_t>(kMiningWorkChunkSize,payload.size()-request.offset);
        response.chunk.assign(payload.begin()+request.offset,payload.begin()+request.offset+count);
    }
    return make_mining_work_response(response);
}
MiningWorkReceiveResult P2pWorkRetrieval::receive(
    const P2pHandshake& peer,const P2pEnvelope& envelope,std::uint64_t now) {
    check_peer(peer); const auto response=parse_mining_work_response(envelope);
    std::lock_guard lock(mutex_); expire(now);
    const auto it=pending_.find({peer.node_id,response.request.key});
    require(it!=pending_.end() && !it->second.finished,"unsolicited/expired work response");
    auto& pending=it->second;
    try {
        require(response.request.offset==pending.bytes.size(),"out-of-order work response");
        if (response.total_size==0) {
            pending.finished=true; pending.bytes.clear(); return {};
        }
        require(pending.total==0 || pending.total==response.total_size,"work response total changed");
        pending.total=response.total_size;
        pending.bytes.insert(pending.bytes.end(),response.chunk.begin(),response.chunk.end());
        if (pending.bytes.size()<pending.total)
            return {make_mining_work_request({response.request.key,static_cast<std::uint32_t>(pending.bytes.size())}),std::nullopt};
        const auto proposal=deserialize_p2p_mining_context_payload(pending.bytes);
        require(checked_key(proposal)==response.request.key,"work evidence does not match requested header/height");
        const auto id=p2p_mining_context_id(proposal);
        if (received_.size()>=kMiningWorkMaxReceived) received_.erase(received_.begin());
        received_[response.request.key]=std::move(pending.bytes);
        pending.finished=true;
        return {std::nullopt,id};
    } catch (...) { pending.finished=true; pending.bytes.clear(); throw; }
}
std::optional<std::vector<std::uint8_t>> P2pWorkRetrieval::take_untrusted(const MiningWorkKey& key) {
    std::lock_guard lock(mutex_); const auto it=received_.find(key);
    if (it==received_.end()) return std::nullopt;
    auto bytes=std::move(it->second); received_.erase(it); return bytes;
}
}
