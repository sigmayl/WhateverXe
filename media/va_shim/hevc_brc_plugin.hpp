// SPDX-License-Identifier: BSD-3-Clause
#ifndef WEX_MEDIA_VA_SHIM_HEVC_BRC_PLUGIN_HPP
#define WEX_MEDIA_VA_SHIM_HEVC_BRC_PLUGIN_HPP
#include "hevc_plugin.hpp"
#include <stddef.h>

namespace VaShim {
namespace HevcPlugin {

constexpr uintptr_t kScheduleReturn = 0x2622b;
constexpr uintptr_t kEncoderVtable = 0x33fc8;
constexpr uintptr_t kControllerVtable = 0x33b40;
constexpr uintptr_t kVbrVtable = 0x33870;
constexpr uintptr_t kConstantQpVtable = 0x34210;
constexpr uintptr_t kTransmissionVtable = 0x33c08;
using AllocationSize = size_t (*)(const void *);
struct FrameBounds {
    uintptr_t lower {}, upper {}; // active caller stack, supplied by the native adapter
    uintptr_t callerFrame {}; // captured before the forwarding thunk's prologue
};
enum class RateControlReadStatus : uint32_t {
    Matched, Scope, Caller, Payload, Helper, Controller, Dimensions, Parameters,
    RateController, FrameRate, TargetRate, Transmission, LimitCount, LimitValues,
    LimitRate, LimitTarget, LimitWindow, FrameParameters,
    FrameInstructions, FrameAllocation, FrameLink
};

struct RateControlInput {
    uint32_t mode {}; // plug-in mode: 0 constant QP, 1 VBR
    uint32_t targetRate {}, maximumRate {}, vbvSize {}, initialFullness {};
    uint32_t frameRateNumerator {}, frameRateDenominator {}, gopSize {}, qp {};
    uint32_t windowFrames {}, windowMaximumRatio {};
    uint32_t qualityValid {};
    // Exact output budgets, independent of HuC's percentage/window fields.
    uint32_t limitCount {}, limitFrames[2] {}, limitBits[2] {};
};

inline uint32_t rateWord(const void *base, uint32_t offset) {
    uint32_t value;
    memcpy(&value, static_cast<const uint8_t *>(base) + offset, sizeof(value));
    return value;
}
inline uintptr_t ratePointer(const void *base, uint32_t offset) {
    uintptr_t value;
    memcpy(&value, static_cast<const uint8_t *>(base) + offset, sizeof(value));
    return value;
}
inline double rateDouble(const void *base, uint32_t offset) {
    double value;
    memcpy(&value, static_cast<const uint8_t *>(base) + offset, sizeof(value));
    return value;
}
inline bool inFrameStack(uintptr_t pointer, size_t bytes, const FrameBounds *bounds) {
    return bounds && bounds->lower < bounds->upper && pointer >= bounds->lower &&
        pointer <= bounds->upper && bytes <= bounds->upper - pointer;
}

// Guarded adapter for macOS 15.8.2's admitted Intel HEVC encoder. Its
// Gen11 Schedule tail-calls through AVD and preserves return address 0x2622b.
// sub_284C copies this frame's bits/second and float FPS to VBR +148/+152.
// sub_26C94 uses frame +224 for Quality. sub_26524's extra +16 is a
// picture-type-dependent native QP, not the stable ACQP quality setting.
// sub_FBEC preserves DataRateLimits bytes/seconds in the transmission object.
// Allocation extents are checked before any private object-member access.
inline bool readRateControlImpl(const void *payload, uintptr_t caller,
    const uint8_t *image, bool knownDriver, bool enabled, AllocationSize size,
    RateControlInput &out, RateControlReadStatus *reason, bool repairIntraHistory,
    const void *nativeFrame, const FrameBounds *frameBounds, bool qualityMode) {
    const auto reject = [reason](RateControlReadStatus status) {
        if (reason) *reason = status;
        return false;
    };
    if (!enabled || !knownDriver || !image || !payload || !size)
        return reject(RateControlReadStatus::Scope);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    static const uint8_t schedule[] = {
        0xe8, 0x9b, 0x4a, 0xfe, 0xff, 0x48, 0x8b, 0x7b, 0x30};
    static const uint8_t update[] = {
        0xf3, 0x41, 0x0f, 0x10, 0x85, 0xac, 0x00, 0x00, 0x00,
        0xf3, 0x0f, 0x11, 0x83, 0x98, 0x00, 0x00, 0x00,
        0x41, 0x8b, 0x85, 0xa8, 0x00, 0x00, 0x00,
        0x89, 0x83, 0x94, 0x00, 0x00, 0x00};
    if (caller != base + kScheduleReturn ||
        !matches(image + kScheduleReturn - 5, schedule, sizeof(schedule)) ||
        !matches(image + 0x2884, update, sizeof(update)))
        return reject(RateControlReadStatus::Caller);
    const uintptr_t address = reinterpret_cast<uintptr_t>(payload);
    if (address < 192)
        return reject(RateControlReadStatus::Payload);
    const auto *helper = reinterpret_cast<const uint8_t *>(address - 192);
    if (size(helper) < 9800 || ratePointer(helper, 0) != base + kEncoderVtable ||
        ratePointer(payload, 944) != reinterpret_cast<uintptr_t>(helper + 1160) ||
        ratePointer(payload, 952) != reinterpret_cast<uintptr_t>(helper + 1224))
        return reject(RateControlReadStatus::Helper);
    const uintptr_t configAddress = ratePointer(helper, 40);
    if (configAddress < 8)
        return reject(RateControlReadStatus::Controller);
    const auto *controller = reinterpret_cast<const uint8_t *>(configAddress - 8);
    if (size(controller) < 703200 ||
        ratePointer(controller, 0) != base + kControllerVtable ||
        ratePointer(controller, 703176) != reinterpret_cast<uintptr_t>(helper))
        return reject(RateControlReadStatus::Controller);
    const auto *config = controller + 8;
    if (rateWord(config, 24) != rateWord(helper, 8) ||
        rateWord(config, 28) != rateWord(helper, 12))
        return reject(RateControlReadStatus::Dimensions);
    RateControlInput input;
    input.mode = rateWord(config, 40);
    const uint32_t pictureQp = rateWord(helper, 1176);
    input.qp = input.mode && !qualityMode ? pictureQp : rateWord(config, 48);
    if (input.mode > 1 || input.qp > 51 || pictureQp > 51)
        return reject(RateControlReadStatus::Parameters);
    const bool frameQualityEnabled =
        (rateWord(config, 18164) & 0x00ff00ffu) == 0x00010001u;
    if ((!input.mode || qualityMode) && nativeFrame && (frameQualityEnabled ||
            (frameBounds && frameBounds->callerFrame))) {
        static const uint8_t frameRegister[] {0x49,0x89,0xf6,0x48,0x89,0xfb};
        static const uint8_t qualityAccess[] {0xf2,0x41,0x0f,0x5c,0x86,0xe0,0,0,0};
        const uintptr_t frame = reinterpret_cast<uintptr_t>(nativeFrame);
        const uintptr_t owner = reinterpret_cast<uintptr_t>(controller);
        const size_t ownerBytes = size(controller);
        const bool embedded = frame >= owner && frame - owner <= ownerBytes &&
            ownerBytes - (frame - owner) >= 296;
        const bool stack = inFrameStack(frame, 296, frameBounds);
        if (!matches(image + 0x2612a, frameRegister, sizeof(frameRegister)) ||
            !matches(image + 0x26cfa, qualityAccess, sizeof(qualityAccess)) ||
            rateDouble(image, 0x29750) != 1.0 || rateDouble(image, 0x29568) != 51.0)
            return reject(RateControlReadStatus::FrameInstructions);
        if (!embedded && size(nativeFrame) < 296 && !stack)
            return reject(RateControlReadStatus::FrameAllocation);
        if (ratePointer(nativeFrame, 8) != ratePointer(helper, 184))
            return reject(RateControlReadStatus::FrameLink);
        const void *qualityFrame = nativeFrame;
        if (frameBounds && frameBounds->callerFrame) {
            // sub_D1D2 retains the real 240-byte queue picture at RBP-0x38.
            // Its temporary submission picture at RBP-0x190 omits Quality.
            // Validate the complete native call chain before accessing either.
            static const uint8_t prologue[] {0x55,0x48,0x89,0xe5,0x41,0x57,
                0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
            static const uint8_t queueBinding[] {0x48,0x89,0x75,0xc8,0x48,0x8b,
                0x46,0x30,0x48,0x89,0x85,0x70,0xfe,0xff,0xff};
            if (!matches(image + 0x2611c, prologue, sizeof(prologue)) ||
                !matches(image + 0xd1f8, queueBinding, sizeof(queueBinding)))
                return reject(RateControlReadStatus::FrameInstructions);
            const uintptr_t bp = frameBounds->callerFrame;
            if (bp < 16 || !inFrameStack(bp - 16, 32, frameBounds))
                return reject(RateControlReadStatus::FrameAllocation);
            const auto *saved = reinterpret_cast<const void *>(bp - 16);
            if (ratePointer(saved, 0) != owner || ratePointer(saved, 24) != base + 0xd310)
                return reject(RateControlReadStatus::FrameLink);
            const uintptr_t parent = ratePointer(saved, 16);
            if (parent < 0x190 || parent <= bp ||
                !inFrameStack(parent - 0x190, 0x190 + 16, frameBounds) ||
                frame != parent - 0x190)
                return reject(RateControlReadStatus::FrameLink);
            const uintptr_t queue = ratePointer(reinterpret_cast<const void *>(parent - 0x38), 0);
            qualityFrame = reinterpret_cast<const void *>(queue);
            if (!qualityFrame || (!inFrameStack(queue, 240, frameBounds) && size(qualityFrame) < 240))
                return reject(RateControlReadStatus::FrameAllocation);
            if (ratePointer(qualityFrame, 48) != ratePointer(nativeFrame, 0))
                return reject(RateControlReadStatus::FrameLink);
        }
        const double quality = rateDouble(qualityFrame, 224);
        if (!(quality >= 0 && quality <= 1))
            return reject(RateControlReadStatus::FrameParameters);
        // Match the native normalized-quality conversion. The firmware's
        // supported frame-QP range has a minimum of ten.
        input.qp = uint32_t((1.0 - quality) * 51.0);
        if (input.qp < 10) input.qp = 10;
        input.qualityValid = 1;
    }
    const auto *control = reinterpret_cast<const uint8_t *>(
        ratePointer(controller, 703192));
    if (!control || size(control) < (input.mode ? 184u : 40u) ||
        ratePointer(control, 0) != base + (input.mode ? kVbrVtable : kConstantQpVtable) ||
        !ratePointer(control, 8))
        return reject(RateControlReadStatus::RateController);
    // The controller owns its rate object at +703192. Rate +8 borrows the
    // original configuration passed to sub_C326, whereas helper +40 points
    // to the controller's copy; those pointers intentionally differ. Check
    // the VBR constructor's owned GOP/geometry fields without dereferencing
    // that borrowed configuration.
    const uint32_t nativeInterval = input.mode ? rateWord(control, 32) : 0;
    const bool intraHistory = input.mode && rateWord(config, 4) == 1 &&
        !rateWord(config, 8);
    uint8_t *history = nullptr;
    if (intraHistory) {
        // sub_2432 allocates four bytes per reference interval. The admitted
        // plug-in uses a zero interval for all-intra VBR, then sub_284C divides
        // by it on the next picture. A one-picture history is sufficient for
        // all-intra. Use only an independently bounded existing allocation.
        static const uint8_t historyAccess[] {
            0x8b,0x4b,0x20,0x8b,0xb3,0x90,0x00,0x00,0x00,
            0x48,0x8b,0xbb,0xb0,0x00,0x00,0x00,0x49,0x8d,0x47,0xff,
            0x48,0x99,0x48,0xf7,0xf9,0x89,0x34,0x97};
        if (nativeInterval > 1 || (!nativeInterval && !repairIntraHistory) ||
            !matches(image + 0x28ae, historyAccess, sizeof(historyAccess)))
            return reject(RateControlReadStatus::RateController);
        history = reinterpret_cast<uint8_t *>(ratePointer(control, 176));
        if (!history || size(history) < sizeof(uint32_t))
            return reject(RateControlReadStatus::RateController);
    }
    if (input.mode && (rateWord(control, 28) != rateWord(config, 4) ||
            (!intraHistory && (!nativeInterval || nativeInterval != rateWord(config, 8))) ||
            rateWord(control, 40) != ((rateWord(helper, 8) + 15) / 16) *
                ((rateWord(helper, 12) + 15) / 16)))
        return reject(RateControlReadStatus::RateController);
    const uint32_t fpsWord = rateWord(input.mode ? control : config,
        input.mode ? 152 : 112);
    float fps;
    memcpy(&fps, &fpsWord, sizeof(fps));
    if (!(fps > 0 && fps <= 1000))
        return reject(RateControlReadStatus::FrameRate);
    uint32_t numerator = uint32_t(double(fps) * 1000 + 0.5), denominator = 1000;
    if (!numerator)
        return reject(RateControlReadStatus::FrameRate);
    uint32_t a = numerator, b = denominator;
    while (b) {
        const uint32_t rest = a % b;
        a = b;
        b = rest;
    }
    input.frameRateNumerator = numerator / a;
    input.frameRateDenominator = denominator / a;
    const uint32_t gop = rateWord(config, 4);
    input.gopSize = gop && gop < 4001 ? gop : 4001;
    input.targetRate = rateWord(input.mode ? control : config, input.mode ? 148 : 108);
    if ((input.mode && !input.targetRate) || input.targetRate > 0x7fffffff)
        return reject(RateControlReadStatus::TargetRate);
    input.maximumRate = input.targetRate * 2;
    input.vbvSize = input.targetRate ? input.targetRate * 2 : 8000000;

    const auto *transmission = reinterpret_cast<const uint8_t *>(ratePointer(control, 16));
    if (!transmission || size(transmission) < 2160 ||
        ratePointer(transmission, 0) != base + kTransmissionVtable)
        return reject(RateControlReadStatus::Transmission);
    const uint32_t limits = rateWord(transmission, 12);
    if (limits > 2)
        return reject(RateControlReadStatus::LimitCount);
    input.limitCount = limits;
    for (uint32_t i = 0; i < limits; ++i) {
        const double bytes = rateDouble(transmission, 16 + i * 8);
        const double seconds = rateDouble(transmission, 32 + i * 8);
        if (!(bytes >= 1 && bytes <= double(0xffffffffu) / 8) ||
            !(seconds > 0 && seconds * fps <= 256))
            return reject(RateControlReadStatus::LimitValues);
        const double rate = bytes * 8 / seconds;
        if (!(rate >= 1 && rate <= 0xffffffffu))
            return reject(RateControlReadStatus::LimitRate);
        const uint32_t bits = uint32_t(bytes * 8), maximum = uint32_t(rate);
        if (input.targetRate > maximum)
            return reject(RateControlReadStatus::LimitTarget);
        if (bits < input.vbvSize)
            input.vbvSize = bits;
        if (maximum < input.maximumRate)
            input.maximumRate = maximum;
        uint32_t frames = uint32_t(seconds * fps + 0.5);
        if (!frames)
            return reject(RateControlReadStatus::LimitWindow);
        if (!input.windowFrames || frames < input.windowFrames)
            input.windowFrames = frames;
        // Cover the requested duration conservatively with whole pictures.
        uint32_t exactFrames = uint32_t(seconds * fps);
        if (double(exactFrames) < seconds * fps) ++exactFrames;
        input.limitFrames[i] = exactFrames;
        input.limitBits[i] = bits;
    }
    if (input.windowFrames && input.targetRate) {
        input.windowMaximumRatio = uint32_t(uint64_t(input.maximumRate) * 100 /
            input.targetRate);
    }
    input.initialFullness = uint32_t(uint64_t(input.vbvSize) * 3 / 4);
    if (history && !nativeInterval) {
        // Complete every guard before changing native state. The controller
        // retains ownership of the allocation and its ordinary destructor.
        const uint32_t zero = 0;
        memcpy(history, &zero, sizeof(zero));
        auto *interval = reinterpret_cast<uint32_t *>(
            const_cast<uint8_t *>(control) + 32);
        __atomic_store_n(interval, 1u, __ATOMIC_RELEASE);
    }
    out = input;
    if (reason) *reason = RateControlReadStatus::Matched;
    return true;
}

inline bool readRateControl(const void *payload, uintptr_t caller,
    const uint8_t *image, bool knownDriver, bool enabled, AllocationSize size,
    RateControlInput &out, RateControlReadStatus *reason = nullptr,
    const void *nativeFrame = nullptr, const FrameBounds *frameBounds = nullptr,
    bool qualityMode = false) {
    return readRateControlImpl(payload, caller, image, knownDriver, enabled,
        size, out, reason, false, nativeFrame, frameBounds, qualityMode);
}

// Enable the bounded native all-intra history repair only for real BRC
// submission. Probes and the default CQP path use the read-only adapter.
inline bool prepareRateControl(const void *payload, uintptr_t caller,
    const uint8_t *image, bool knownDriver, bool enabled, AllocationSize size,
    RateControlInput &out, RateControlReadStatus *reason = nullptr,
    const void *nativeFrame = nullptr, const FrameBounds *frameBounds = nullptr,
    bool qualityMode = false) {
    return readRateControlImpl(payload, caller, image, knownDriver, enabled,
        size, out, reason, true, nativeFrame, frameBounds, qualityMode);
}

}
}
#endif
