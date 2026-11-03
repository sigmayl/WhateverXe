// SPDX-License-Identifier: BSD-3-Clause
#ifndef WEX_MEDIA_HEVC_BRC_WINDOW_HPP
#define WEX_MEDIA_HEVC_BRC_WINDOW_HPP
#include <stdint.h>

namespace HevcCore {
struct BitrateWindow {
    uint32_t frames, bits;
};
struct FrameBudgetConfig {
    uint32_t maximumRate, bufferBits, initialBits;
    uint32_t fpsNumerator, fpsDenominator, windowCount;
    BitrateWindow windows[2];
    bool operator==(const FrameBudgetConfig &other) const {
        if (maximumRate != other.maximumRate || bufferBits != other.bufferBits ||
            initialBits != other.initialBits || fpsNumerator != other.fpsNumerator ||
            fpsDenominator != other.fpsDenominator || windowCount != other.windowCount)
            return false;
        for (uint32_t i = 0; i < windowCount; ++i)
            if (windows[i].frames != other.windows[i].frames ||
                windows[i].bits != other.windows[i].bits) return false;
        return true;
    }
};

// A candidate picture consumes no credit. Commit only the completed output
// published to the client, after it fits the VBV and every requested window.
class FrameBudget {
public:
    bool configure(const FrameBudgetConfig &config) {
        if (!config.maximumRate || !config.bufferBits ||
            config.initialBits > config.bufferBits || !config.fpsNumerator ||
            !config.fpsDenominator || config.windowCount > 2) return false;
        for (uint32_t i = 0; i < config.windowCount; ++i)
            if (!config.windows[i].frames || config.windows[i].frames > 256 ||
                !config.windows[i].bits) return false;
        if (configured_ && config_ == config) return true;
        if (configured_ && count_) {
            // Drained rate/limit changes do not erase output already inside
            // a requested window or replenish the transmission budget.
            const uint64_t bits = credit_ / config_.fpsNumerator;
            credit_ = (bits < config.bufferBits ? bits : config.bufferBits) * config.fpsNumerator;
            config_ = config;
            return true;
        }
        config_ = config;
        configured_ = true;
        reset();
        return true;
    }
    void reset() {
        credit_ = uint64_t(config_.initialBits) * config_.fpsNumerator;
        count_ = next_ = 0;
        for (auto &bits : history_) bits = 0;
    }
    uint32_t availableBits() const {
        if (!configured_) return 0;
        uint64_t available = nextCredit() / config_.fpsNumerator;
        for (uint32_t w = 0; w < config_.windowCount; ++w) {
            const auto &window = config_.windows[w];
            const uint32_t keep = count_ < window.frames - 1 ? count_ : window.frames - 1;
            uint64_t used = 0;
            for (uint32_t i = 0; i < keep; ++i)
                used += history_[(next_ + 255 - i) % 256];
            uint64_t remaining = used < window.bits ? window.bits - used : 0;
            // Spread the first window over the pictures that have yet to
            // arrive; consuming it in an early burst would starve them.
            if (count_ < window.frames)
                remaining /= window.frames - count_;
            if (remaining < available) available = remaining;
        }
        return uint32_t(available);
    }
    bool commit(uint32_t bits) {
        if (!bits || bits > availableBits()) return false;
        credit_ = nextCredit() - uint64_t(bits) * config_.fpsNumerator;
        history_[next_] = bits;
        next_ = (next_ + 1) % 256;
        if (count_ < 256) ++count_;
        return true;
    }
private:
    uint64_t nextCredit() const {
        const uint64_t cap = uint64_t(config_.bufferBits) * config_.fpsNumerator;
        const uint64_t refill = uint64_t(config_.maximumRate) * config_.fpsDenominator;
        return refill >= cap - credit_ ? cap : credit_ + refill;
    }
    FrameBudgetConfig config_;
    uint64_t credit_;
    uint32_t history_[256], count_, next_;
    bool configured_;
};
}
#endif
