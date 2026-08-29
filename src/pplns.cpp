#include "zano_p2pool/pplns.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zano_p2pool {
namespace {

using boost::multiprecision::cpp_int;

cpp_int work_to_int(const ChainWork& work) {
    cpp_int value = 0;
    for (const std::uint8_t byte : work) {
        value <<= 8;
        value |= byte;
    }
    return value;
}

ChainWork int_to_work(cpp_int value) {
    if (value < 0) {
        throw std::invalid_argument("PPLNS work cannot be negative");
    }

    ChainWork work{};
    for (std::size_t i = work.size(); i-- > 0;) {
        work[i] = static_cast<std::uint8_t>((value & 0xff).convert_to<unsigned>());
        value >>= 8;
    }
    if (value != 0) {
        throw std::overflow_error("PPLNS work exceeds 256 bits");
    }
    return work;
}

}  // namespace

PplnsWindow build_pplns_window(
    const ShareChain& chain,
    const ChainWork& requested_work) {
    const cpp_int requested = work_to_int(requested_work);
    if (requested == 0) {
        throw std::invalid_argument("PPLNS requested work must be nonzero");
    }

    cpp_int remaining = requested;
    std::map<MinerId, cpp_int> credited;

    const ConnectedShare* current = chain.best_tip();
    while (current != nullptr && remaining != 0) {
        const cpp_int current_work = work_to_int(
            share_work(current->share.share_difficulty));
        if (current_work == 0) {
            throw std::logic_error("connected share has zero work");
        }

        const cpp_int included = std::min(current_work, remaining);
        credited[current->share.miner_id] += included;
        remaining -= included;

        if (remaining == 0 || is_zero_share_id(current->share.parent_id)) {
            break;
        }

        current = chain.find(current->share.parent_id);
        if (current == nullptr) {
            throw std::logic_error(
                "best-chain PPLNS traversal encountered a missing parent");
        }
    }

    PplnsWindow result;
    result.requested_work = requested_work;
    result.covered_work = int_to_work(requested - remaining);
    result.complete = remaining == 0;
    result.miners.reserve(credited.size());
    for (const auto& [miner_id, work] : credited) {
        if (work == 0) {
            continue;
        }
        result.miners.push_back(PplnsMinerWork{
            miner_id,
            int_to_work(work),
        });
    }
    return result;
}

std::vector<PplnsPayout> allocate_pplns_reward(
    const PplnsWindow& window,
    std::uint64_t reward_atomic) {
    std::map<MinerId, cpp_int> credited;
    for (const auto& row : window.miners) {
        const cpp_int work = work_to_int(row.work);
        if (work != 0) {
            credited[row.miner_id] += work;
        }
    }

    if (credited.empty()) {
        return {};
    }

    cpp_int total_work = 0;
    for (const auto& [miner_id, work] : credited) {
        static_cast<void>(miner_id);
        total_work += work;
    }
    if (total_work == 0) {
        throw std::logic_error("PPLNS payout has no credited work");
    }

    struct FractionalRow {
        MinerId miner_id{};
        cpp_int remainder{};
        std::size_t payout_index{0};
    };

    std::vector<PplnsPayout> payouts;
    payouts.reserve(credited.size());
    std::vector<FractionalRow> fractional;
    fractional.reserve(credited.size());

    std::uint64_t allocated = 0;
    const cpp_int reward = reward_atomic;
    for (const auto& [miner_id, work] : credited) {
        const cpp_int numerator = reward * work;
        const cpp_int quotient = numerator / total_work;
        const cpp_int remainder = numerator % total_work;
        const std::uint64_t amount = quotient.convert_to<std::uint64_t>();

        if (amount > reward_atomic - allocated) {
            throw std::overflow_error("PPLNS reward allocation overflow");
        }
        allocated += amount;

        const std::size_t index = payouts.size();
        payouts.push_back(PplnsPayout{
            miner_id,
            int_to_work(work),
            amount,
        });
        fractional.push_back(FractionalRow{
            miner_id,
            remainder,
            index,
        });
    }

    std::uint64_t leftover = reward_atomic - allocated;
    if (leftover > fractional.size()) {
        throw std::logic_error(
            "PPLNS largest-remainder invariant exceeded miner count");
    }

    std::sort(
        fractional.begin(),
        fractional.end(),
        [](const FractionalRow& left, const FractionalRow& right) {
            if (left.remainder != right.remainder) {
                return left.remainder > right.remainder;
            }
            return left.miner_id < right.miner_id;
        });

    for (std::uint64_t i = 0; i < leftover; ++i) {
        ++payouts[fractional[static_cast<std::size_t>(i)].payout_index].amount;
    }

    return payouts;
}

}  // namespace zano_p2pool
