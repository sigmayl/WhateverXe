// SPDX-License-Identifier: BSD-3-Clause
#ifndef WEX_MEDIA_HEVC_BRC_WINDOW_HPP
#define WEX_MEDIA_HEVC_BRC_WINDOW_HPP
#include <stdint.h>

namespace HevcCore {
struct BitrateWindow {
    uint32_t frames, bits;
    uint64_t durationNs;
};
struct FrameBudgetConfig {
    uint32_t maximumRate, bufferBits, initialBits;
    uint32_t fpsNumerator, fpsDenominator, windowCount;
    BitrateWindow windows[2];
    bool timed;
    bool operator==(const FrameBudgetConfig &other) const {
        if (maximumRate != other.maximumRate || bufferBits != other.bufferBits ||
            initialBits != other.initialBits || fpsNumerator != other.fpsNumerator ||
            fpsDenominator != other.fpsDenominator || windowCount != other.windowCount ||
            timed != other.timed)
            return false;
        for (uint32_t i = 0; i < windowCount; ++i)
            if (windows[i].frames != other.windows[i].frames ||
                windows[i].bits != other.windows[i].bits ||
                windows[i].durationNs != other.windows[i].durationNs) return false;
        return true;
    }
};

// A candidate picture consumes no credit. Commit only the completed output
// published to the client, after it fits the VBV and every requested window.
class FrameBudget {
public:
    static constexpr uint64_t second = 1000000000;
    // Advance only from a monotonic clock. FPS is a firmware hint, not an
    // elapsed-time measurement for timestamped output windows.
    bool advance(uint64_t now) {
        if (clockValid_ && now < now_) return false;
        if (configured_ && config_.timed && clockValid_) {
            const uint64_t room = uint64_t(config_.bufferBits) * second - credit_;
            const uint64_t elapsed = now - now_;
            credit_ += elapsed >= (room + config_.maximumRate - 1) / config_.maximumRate
                ? room : elapsed * config_.maximumRate;
        }
        now_ = now;
        clockValid_ = true;
        trimTimed();
        return true;
    }
    bool configure(const FrameBudgetConfig &config) {
        if (!config.maximumRate || !config.bufferBits ||
            config.initialBits > config.bufferBits || !config.fpsNumerator ||
            !config.fpsDenominator || config.windowCount > 2) return false;
        for (uint32_t i = 0; i < config.windowCount; ++i)
            if (!config.windows[i].bits || (config.timed
                ? !config.windows[i].durationNs || config.windows[i].durationNs > 256 * second
                : !config.windows[i].frames || config.windows[i].frames > 256)) return false;
        if (configured_ && config_ == config) return true;
        if (configured_ && (count_ || config_.timed)) {
            if (config_.timed != config.timed && count_) return false;
            // Drained rate/limit changes do not erase output already inside
            // a requested window or replenish the transmission budget.
            if (config_.timed && config.timed) {
                const uint64_t cap = uint64_t(config.bufferBits) * second;
                if (credit_ > cap) credit_ = cap;
            } else {
                const uint64_t bits = credit_ / (config_.timed ? second : config_.fpsNumerator);
                credit_ = (bits < config.bufferBits ? bits : config.bufferBits) *
                    (config.timed ? second : config.fpsNumerator);
            }
            config_ = config;
            trimTimed();
            return true;
        }
        config_ = config;
        configured_ = true;
        reset();
        return true;
    }
    void reset() {
        credit_ = uint64_t(config_.initialBits) * (config_.timed ? second : config_.fpsNumerator);
        count_ = next_ = 0;
        discarded_ = false;
        for (auto &bits : history_) bits = 0;
    }
    uint32_t availableBits() const {
        if (!configured_) return 0;
        if (config_.timed) {
            if (!clockValid_ || count_ == 256) return 0;
            uint64_t available = credit_ / second;
            for (uint32_t w = 0; w < config_.windowCount; ++w) {
                const auto &window = config_.windows[w];
                // A longer, newly requested window must not forgive output
                // discarded under the previous retention interval.
                if (discarded_ && now_ - discardedAt_ < window.durationNs) return 0;
                const uint64_t used = usedIn(window.durationNs);
                const uint64_t remaining = used < window.bits ? window.bits - used : 0;
                if (remaining < available) available = remaining;
            }
            return uint32_t(available);
        }
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
        if (config_.timed) {
            credit_ -= uint64_t(bits) * second;
            if (!config_.windowCount) { discarded_ = true; discardedAt_ = now_; return true; }
            times_[next_] = now_;
        } else
        credit_ = nextCredit() - uint64_t(bits) * config_.fpsNumerator;
        history_[next_] = bits;
        next_ = (next_ + 1) % 256;
        if (count_ < 256) ++count_;
        return true;
    }
    // Earliest conservative publication time. No output/history is committed
    // while waiting; a valid completed reconstruction stays available.
    uint64_t nextTimeFor(uint32_t bits) const {
        if (!configured_ || !config_.timed || !clockValid_ || !bits ||
            bits > config_.bufferBits) return UINT64_MAX;
        uint64_t deadline = now_;
        const uint64_t required = uint64_t(bits) * second;
        if (required > credit_) {
            const uint64_t wait = (required - credit_ + config_.maximumRate - 1) / config_.maximumRate;
            deadline = addTime(now_, wait);
        }
        for (uint32_t w = 0; w < config_.windowCount; ++w) {
            const auto &window = config_.windows[w];
            if (bits > window.bits) return UINT64_MAX;
            if (discarded_ && now_ - discardedAt_ < window.durationNs)
                deadline = later(deadline, addTime(discardedAt_, window.durationNs));
            uint64_t used = usedIn(window.durationNs);
            for (uint32_t i = 0; used + bits > window.bits && i < count_; ++i) {
                const uint32_t at = (next_ + 256 - count_ + i) % 256;
                if (now_ - times_[at] >= window.durationNs) continue;
                used -= history_[at];
                deadline = later(deadline, addTime(times_[at], window.durationNs));
            }
        }
        // Bounded storage: pace until an entry expires rather than losing
        // unexpired output or rejecting a valid high-frame-rate request.
        if (count_ == 256)
            deadline = later(deadline, addTime(times_[next_], longestWindow()));
        return deadline;
    }
private:
    static uint64_t later(uint64_t a, uint64_t b) { return a > b ? a : b; }
    static uint64_t addTime(uint64_t at, uint64_t duration) {
        return duration > UINT64_MAX - at ? UINT64_MAX : at + duration;
    }
    uint64_t longestWindow() const {
        uint64_t duration = 0;
        for (uint32_t i = 0; i < config_.windowCount; ++i)
            duration = later(duration, config_.windows[i].durationNs);
        return duration;
    }
    uint64_t usedIn(uint64_t duration) const {
        uint64_t used = 0;
        for (uint32_t i = 0; i < count_; ++i) {
            const uint32_t at = (next_ + 256 - count_ + i) % 256;
            if (now_ - times_[at] < duration) used += history_[at];
        }
        return used;
    }
    void trimTimed() {
        if (!configured_ || !config_.timed || !clockValid_) return;
        const uint64_t keep = longestWindow();
        while (count_) {
            const uint32_t at = (next_ + 256 - count_) % 256;
            if (now_ - times_[at] < keep) break;
            discarded_ = true;
            discardedAt_ = times_[at];
            --count_;
        }
    }
    uint64_t nextCredit() const {
        const uint64_t cap = uint64_t(config_.bufferBits) * config_.fpsNumerator;
        const uint64_t refill = uint64_t(config_.maximumRate) * config_.fpsDenominator;
        return refill >= cap - credit_ ? cap : credit_ + refill;
    }
    FrameBudgetConfig config_;
    uint64_t credit_;
    uint32_t history_[256], count_, next_;
    bool configured_;
    uint64_t times_[256], now_, discardedAt_;
    bool clockValid_, discarded_;
};
}
#endif
