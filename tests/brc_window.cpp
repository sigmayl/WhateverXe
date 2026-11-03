// SPDX-License-Identifier: BSD-3-Clause
// CPU checks for committed-output VBV and byte-window budgets.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <type_traits>
#include "../media/hevc_core/brc_window.hpp"

int main() {
    using namespace HevcCore;
    static_assert(std::is_trivial<FrameBudget>::value, "zero-initialized context state");
    FrameBudget budget {};
    assert(!budget.availableBits() && !budget.commit(1));
    FrameBudgetConfig config {30000, 60000, 45000, 30000, 1001, 2, {{7,7000},{31,28000}}};
    assert(budget.configure(config) && budget.availableBits() == 28000 / 31);
    assert(!budget.commit(904) && budget.availableBits() == 28000 / 31);
    std::vector<uint32_t> packets;
    uint64_t credit = uint64_t(config.initialBits) * config.fpsNumerator;
    uint32_t seed = 0x183729;
    for (unsigned frame = 0; frame < 1000; ++frame) {
        const uint64_t cap = uint64_t(config.bufferBits) * config.fpsNumerator;
        const uint64_t refill = uint64_t(config.maximumRate) * config.fpsDenominator;
        const uint64_t nextCredit = refill >= cap - credit ? cap : credit + refill;
        uint64_t expected = nextCredit / config.fpsNumerator;
        for (const auto &window : config.windows) {
            uint64_t used = 0;
            for (unsigned i = 1; i < window.frames && i <= packets.size(); ++i)
                used += packets[packets.size()-i];
            uint64_t remaining = window.bits > used ? window.bits - used : 0;
            if (packets.size() < window.frames) remaining /= window.frames - packets.size();
            if (remaining < expected) expected = remaining;
        }
        assert(budget.availableBits() == expected);
        const auto before = budget.availableBits();
        assert(!budget.commit(before + 1) && budget.availableBits() == before);
        assert(budget.configure(config) && budget.availableBits() == before);
        seed = seed * 1664525u + 1013904223u;
        const uint32_t bits = before ? 1 + seed % before : 0;
        if (!bits) {
            // A rejected output cannot advance a saturated window.
            assert(!budget.commit(1) && budget.availableBits() == 0);
            break;
        }
        assert(budget.commit(bits));
        credit = nextCredit - uint64_t(bits) * config.fpsNumerator;
        packets.push_back(bits);
        for (const auto &window : config.windows) {
            uint64_t used = 0;
            for (unsigned i = 1; i <= window.frames && i <= packets.size(); ++i)
                used += packets[packets.size()-i];
            assert(used <= window.bits);
        }
    }
    assert(packets.size() > 256);
    const auto remaining = budget.availableBits();
    auto invalid = config;
    invalid.windows[1].frames = 257;
    assert(!budget.configure(invalid) && budget.availableBits() == remaining);
    invalid = config;
    invalid.fpsNumerator = 0;
    assert(!budget.configure(invalid) && budget.availableBits() == remaining);
    config.windows[0].bits = 6000;
    assert(budget.configure(config));
    uint64_t used = 0;
    for (unsigned i = 1; i < 7; ++i) used += packets[packets.size()-i];
    assert(budget.availableBits() <= (used < 6000 ? 6000 - used : 0));
    budget.reset();
    assert(budget.availableBits() == 6000 / 7);
    FrameBudget changing {};
    FrameBudgetConfig rate {5200000,5200000,3900000,30,1,1,{{2,30000},{}}};
    assert(changing.configure(rate) && changing.commit(12000));
    assert(changing.availableBits() == 18000);
    rate.maximumRate = rate.bufferBits = 4000000;
    rate.initialBits = 3000000;
    assert(changing.configure(rate) && changing.availableBits() == 18000);
    assert(!changing.commit(18001) && changing.commit(5000));
    assert(changing.availableBits() == 25000);
    FrameBudget startup {};
    FrameBudgetConfig shortWindow {4705882,2400000,1800000,30,1,1,{{16,2400000},{}}};
    assert(startup.configure(shortWindow));
    for (unsigned i = 0; i < 40; ++i) {
        assert(startup.availableBits() == 150000);
        assert(startup.commit(150000));
    }
    FrameBudgetConfig edge {UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, 0, {}};
    assert(budget.configure(edge) && budget.availableBits() == UINT32_MAX);
    assert(budget.commit(UINT32_MAX) && budget.availableBits() == UINT32_MAX);
    puts("PASS completed-picture bitrate budgets");
}
