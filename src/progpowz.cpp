#include "zano_p2pool/progpowz.hpp"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
#include <ethash/ethash.hpp>
#include <ethash/progpow.hpp>
#endif

namespace zano_p2pool {
namespace {

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
ProgPowZResult copy_result(const ethash::result& result) {
    ProgPowZResult out{};
    std::memcpy(
        out.final_hash.data(), result.final_hash.bytes, out.final_hash.size());
    std::memcpy(
        out.mix_hash.data(), result.mix_hash.bytes, out.mix_hash.size());
    return out;
}
#endif

}  // namespace

std::uint64_t progpowz_epoch(std::uint64_t height) noexcept {
    return height / kProgPowZEpochLength;
}

bool progpowz_available() noexcept {
#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    return true;
#else
    return false;
#endif
}

const char* progpowz_revision() noexcept {
#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    return progpow::revision;
#else
    return "unavailable";
#endif
}

ProgPowZResult progpowz_hash(
    std::uint64_t height,
    const Hash256& header_hash,
    std::uint64_t nonce,
    ProgPowZContextMode mode) {
#ifndef ZANO_P2POOL_HAVE_PROGPOWZ
    (void)height;
    (void)header_hash;
    (void)nonce;
    (void)mode;
    throw std::runtime_error(
        "ProgPoWZ support is not enabled; configure with "
        "-DZANO_P2POOL_ZANO_SOURCE_DIR=/absolute/path/to/zano");
#else
    if (height > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::out_of_range(
            "height exceeds the block-number range supported by ProgPoWZ");
    }

    const int block_number = static_cast<int>(height);
    const int epoch = static_cast<int>(progpowz_epoch(height));
    const auto eth_header =
        ethash::hash256_from_bytes(header_hash.data());

    if (mode == ProgPowZContextMode::Light) {
        const auto& context = ethash::get_global_epoch_context(epoch);
        return copy_result(
            progpow::hash(context, block_number, eth_header, nonce));
    }

    auto context = ethash::get_global_epoch_context_full(epoch);
    if (!context) {
        throw std::bad_alloc();
    }

    return copy_result(
        progpow::hash(*context, block_number, eth_header, nonce));
#endif
}

}  // namespace zano_p2pool
