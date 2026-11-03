// SPDX-License-Identifier: BSD-3-Clause
#include "../media/va_shim/hevc_brc_plugin.hpp"
#include <array>
#include <vector>
#include <cassert>
#include <cstdio>

using namespace VaShim::HevcPlugin;
static std::array<std::pair<const void *, size_t>, 6> allocations;
static unsigned sizeCalls;
static size_t allocationSize(const void *pointer) {
    ++sizeCalls;
    for (const auto &entry : allocations)
        if (entry.first == pointer)
            return entry.second;
    return 0;
}
template <typename T> static void set(std::vector<uint8_t> &bytes, size_t at, T value) {
    memcpy(bytes.data() + at, &value, sizeof(value));
}

int main() {
    std::vector<uint8_t> image(0x34300), helper(9800), controller(703208),
        vbr(184), transmission(2160), history(16);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
    const uint8_t call[] {0xe8, 0x9b, 0x4a, 0xfe, 0xff, 0x48, 0x8b, 0x7b, 0x30};
    const uint8_t update[] {0xf3, 0x41, 0x0f, 0x10, 0x85, 0xac, 0x00, 0x00, 0x00,
        0xf3, 0x0f, 0x11, 0x83, 0x98, 0x00, 0x00, 0x00,
        0x41, 0x8b, 0x85, 0xa8, 0x00, 0x00, 0x00,
        0x89, 0x83, 0x94, 0x00, 0x00, 0x00};
    memcpy(image.data() + 0x26226, call, sizeof(call));
    memcpy(image.data() + 0x2884, update, sizeof(update));
    const uint8_t historyAccess[] {
        0x8b,0x4b,0x20,0x8b,0xb3,0x90,0x00,0x00,0x00,
        0x48,0x8b,0xbb,0xb0,0x00,0x00,0x00,0x49,0x8d,0x47,0xff,
        0x48,0x99,0x48,0xf7,0xf9,0x89,0x34,0x97};
    memcpy(image.data() + 0x28ae, historyAccess, sizeof(historyAccess));
    set(helper, 0, base + kEncoderVtable);
    set(helper, 8, uint32_t(1920));
    set(helper, 12, uint32_t(1080));
    set(helper, 40, reinterpret_cast<uintptr_t>(controller.data() + 8));
    set(helper, 1136, reinterpret_cast<uintptr_t>(helper.data() + 1160));
    set(helper, 1144, reinterpret_cast<uintptr_t>(helper.data() + 1224));
    set(helper, 1176, uint32_t(33));
    set(controller, 0, base + kControllerVtable);
    set(controller, 8 + 4, uint32_t(16));
    set(controller, 8 + 8, uint32_t(1));
    set(controller, 8 + 24, uint32_t(1920));
    set(controller, 8 + 28, uint32_t(1080));
    set(controller, 8 + 40, uint32_t(1));
    set(controller, 8 + 48, uint32_t(26));
    set(controller, 8 + 108, uint32_t(3000000));
    set(controller, 8 + 112, float(24));
    set(controller, 703176, reinterpret_cast<uintptr_t>(helper.data()));
    set(controller, 703192, reinterpret_cast<uintptr_t>(vbr.data()));
    set(vbr, 0, base + kVbrVtable);
    // The native rate object borrows the original configuration, not the
    // controller's copy. The adapter must never read this unowned pointer.
    set(vbr, 8, uintptr_t(0x12345678));
    set(vbr, 16, reinterpret_cast<uintptr_t>(transmission.data()));
    set(vbr, 28, uint32_t(16));
    set(vbr, 32, uint32_t(1));
    set(vbr, 40, uint32_t(120 * 68));
    set(vbr, 148, uint32_t(4000000));
    set(vbr, 152, float(30));
    set(transmission, 0, base + kTransmissionVtable);
    allocations = {{{helper.data(), helper.size()}, {controller.data(), controller.size()},
        {vbr.data(), vbr.size()}, {transmission.data(), transmission.size()}, {nullptr, 0}, {nullptr, 0}}};
    const auto *payload = helper.data() + 192;
    RateControlInput input;
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(input.mode == 1 && input.qp == 33 && input.gopSize == 16);
    // Current frame parameters override the initial configuration.
    assert(input.targetRate == 4000000 && input.maximumRate == 8000000);
    assert(input.frameRateNumerator == 30 && input.frameRateDenominator == 1);
    assert(input.vbvSize == 8000000 && input.initialFullness == 6000000);
    set(transmission, 12, uint32_t(2));
    set(transmission, 16, double(750000));
    set(transmission, 24, double(2500000));
    set(transmission, 32, double(1));
    set(transmission, 40, double(5));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(input.maximumRate == 4000000 && input.vbvSize == 6000000);
    assert(input.initialFullness == 4500000 && input.windowFrames == 30);
    assert(input.windowMaximumRatio == 100);
    assert(input.limitCount == 2 && input.limitFrames[0] == 30 && input.limitFrames[1] == 150);
    assert(input.limitBits[0] == 6000000 && input.limitBits[1] == 20000000);
    set(transmission, 32, double(1.01));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(input.limitFrames[0] == 31 && input.limitBits[0] == 6000000);

    set(transmission, 12, uint32_t(0));
    set(vbr, 152, float(29.97));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(input.frameRateNumerator == 2997 && input.frameRateDenominator == 100);
    const auto saved = input;
    sizeCalls = 0;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), false,
        true, allocationSize, input) && sizeCalls == 0);
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        false, allocationSize, input) && sizeCalls == 0);
    assert(!readRateControl(payload, base + kScheduleReturn + 1, image.data(), true,
        true, allocationSize, input) && sizeCalls == 0);
    image[0x2884] ^= 1;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input) && sizeCalls == 0);
    image[0x2884] ^= 1;
    std::array<uint8_t, 8> small {};
    allocations[4] = {small.data(), small.size()};
    const auto *smallPayload = reinterpret_cast<const void *>(
        reinterpret_cast<uintptr_t>(small.data()) + 192);
    assert(!readRateControl(smallPayload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(!memcmp(&input, &saved, sizeof(input)));
    allocations[1].second = 8;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    allocations[1].second = controller.size();
    set(vbr, 152, uint32_t(0x7fc00000)); // NaN FPS
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    set(vbr, 152, float(30));
    set(helper, 1176, uint32_t(52));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    set(helper, 1176, uint32_t(26));
    set(vbr, 40, uint32_t(1));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    set(vbr, 40, uint32_t(120 * 68));
    // A native all-intra VBR configuration can construct a zero reference
    // interval. Matching owned fields alone must not admit that controller:
    // it divides by the interval as soon as a completed picture is returned.
    set(controller, 8 + 8, uint32_t(0));
    set(vbr, 32, uint32_t(0));
    RateControlReadStatus reason;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason));
    assert(reason == RateControlReadStatus::RateController);
    set(controller, 8 + 4, uint32_t(1));
    set(vbr, 28, uint32_t(1));
    set(vbr, 176, reinterpret_cast<uintptr_t>(history.data()));
    set(history, 0, uint32_t(0x12345678));
    allocations[4] = {history.data(), history.size()};
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(rateWord(vbr.data(), 32) == 0 && rateWord(history.data(), 0) == 0x12345678);
    allocations[4].second = 3;
    assert(!prepareRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    allocations[4].second = history.size();
    image[0x28c4] ^= 1;
    assert(!prepareRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    image[0x28c4] ^= 1;
    assert(rateWord(vbr.data(), 32) == 0);
    set(transmission, 12, uint32_t(3));
    assert(!prepareRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason));
    assert(reason == RateControlReadStatus::LimitCount);
    assert(rateWord(vbr.data(), 32) == 0 && rateWord(history.data(), 0) == 0x12345678);
    set(transmission, 12, uint32_t(0));
    assert(prepareRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(input.mode == 1 && input.gopSize == 1);
    assert(rateWord(vbr.data(), 32) == 1 && rateWord(history.data(), 0) == 0);
    assert(rateWord(controller.data(), 8 + 8) == 0);
    set(history, 0, uint32_t(0x87654321));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(prepareRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(rateWord(history.data(), 0) == 0x87654321);
    set(controller, 8 + 4, uint32_t(16));
    set(vbr, 28, uint32_t(16));
    set(controller, 8 + 8, uint32_t(1));
    set(vbr, 32, uint32_t(1));
    set(transmission, 12, uint32_t(3));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    set(transmission, 12, uint32_t(1));
    set(transmission, 32, double(10)); // exceeds the plug-in's frame window
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    set(transmission, 12, uint32_t(0));
    set(controller, 8 + 40, uint32_t(0));
    set(vbr, 0, base + kConstantQpVtable);
    allocations[2].second = 40;
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input));
    assert(input.mode == 0 && input.qp == 26 && input.targetRate == 3000000);
    assert(input.frameRateNumerator == 24 && input.frameRateDenominator == 1);
    // The native per-picture QP may differ by picture type. It must not reset
    // an ACQP history whose requested sequence quality has not changed.
    set(helper, 1176, uint32_t(32));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input) && input.qp == 26 && !input.qualityValid);
    std::vector<uint8_t> frame(296);
    allocations[5] = {frame.data(), frame.size()};
    set(frame, 8, uintptr_t(0x12340000));
    set(helper, 184, uintptr_t(0x12340000));
    set(controller, 8 + 18164, uint32_t(0x00010001));
    const uint8_t frameRegister[] {0x49,0x89,0xf6,0x48,0x89,0xfb};
    memcpy(image.data() + 0x2612a, frameRegister, sizeof(frameRegister));
    const uint8_t qualityAccess[] {0xf2,0x41,0x0f,0x5c,0x86,0xe0,0,0,0};
    memcpy(image.data() + 0x26cfa, qualityAccess, sizeof(qualityAccess));
    set(image, 0x29750, double(1));
    set(image, 0x29568, double(51));
    set(frame, 224, double(.5));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    assert(input.qualityValid && input.qp == 25);
    set(frame, 224, double(.8));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()) && input.qp == 10);
    const auto quality = input;
    for (double invalid : {-0.1, 1.1}) {
        set(frame, 224, invalid);
        assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
            true, allocationSize, input, &reason, frame.data()));
        assert(reason == RateControlReadStatus::FrameParameters);
        assert(!memcmp(&quality, &input, sizeof(input)));
    }
    set(frame, 224, uint64_t(0x7ff8000000000000));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    set(frame, 224, double(.5));
    allocations[5].second = 295;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    allocations[5].second = frame.size();
    set(frame, 8, uintptr_t(0x12340001));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    set(frame, 8, uintptr_t(0x12340000));
    image[0x2612a] ^= 1;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    image[0x2612a] ^= 1;
    image[0x26cfa] ^= 1;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    image[0x26cfa] ^= 1;
    set(image, 0x29568, double(50));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data()));
    set(image, 0x29568, double(51));
    // The native frame can also be an active, independently bounded caller
    // stack object. Neither a heap size nor arbitrary stack contents suffice.
    allocations[5].second = 0;
    FrameBounds bounds {reinterpret_cast<uintptr_t>(frame.data()),
        reinterpret_cast<uintptr_t>(frame.data() + frame.size())};
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data(), &bounds) && input.qp == 25);
    --bounds.upper;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data(), &bounds));
    ++bounds.upper;
    ++bounds.lower;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, frame.data(), &bounds));
    // A frame embedded in the independently bounded owned controller is also
    // valid. An unrelated interior pointer has no such lifetime proof.
    set(controller, 2000 + 8, uintptr_t(0x12340000));
    set(controller, 2000 + 224, double(.5));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, controller.data() + 2000) && input.qp == 25);
    // Native Schedule receives a temporary frame, while Quality remains in
    // the queue picture held by sub_D1D2. Check the admitted unwind, ownership
    // and source link instead of interpreting zero-filled temporary fields.
    std::vector<uint8_t> stack(2048);
    const uintptr_t parent = reinterpret_cast<uintptr_t>(stack.data() + 1024);
    const uintptr_t bp = parent - 0x1a0;
    const auto *temporary = stack.data() + 1024 - 0x190;
    const auto *queued = stack.data() + 1280;
    set(stack, 1024 - 0x1a0, parent);
    set(stack, 1024 - 0x1a0 + 8, base + 0xd310);
    set(stack, 1024 - 0x1a0 - 16, reinterpret_cast<uintptr_t>(controller.data()));
    set(stack, 1024 - 0x38, reinterpret_cast<uintptr_t>(queued));
    set(stack, 1024 - 0x190, uintptr_t(0x98760000));
    set(stack, 1024 - 0x190 + 8, uintptr_t(0x12340000));
    set(stack, 1280 + 48, uintptr_t(0x98760000));
    set(stack, 1280 + 224, double(.4));
    set(controller, 8 + 18164, uint32_t(0));
    const uint8_t prologue[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x50};
    const uint8_t queueBinding[] {0x48,0x89,0x75,0xc8,0x48,0x8b,0x46,0x30,
        0x48,0x89,0x85,0x70,0xfe,0xff,0xff};
    memcpy(image.data() + 0x2611c, prologue, sizeof(prologue));
    memcpy(image.data() + 0xd1f8, queueBinding, sizeof(queueBinding));
    FrameBounds caller {reinterpret_cast<uintptr_t>(stack.data()),
        reinterpret_cast<uintptr_t>(stack.data() + stack.size()), bp};
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller) && input.qp == 30);
    set(stack, 1280 + 224, double(.8));
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller) && input.qp == 10);
    const auto requested = input;
    set(stack, 1024 - 0x1a0 + 8, base + 0xd311);
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller));
    assert(!memcmp(&requested, &input, sizeof(input)));
    set(stack, 1024 - 0x1a0 + 8, base + 0xd310);
    set(stack, 1024 - 0x1a0 - 16, reinterpret_cast<uintptr_t>(helper.data()));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller));
    set(stack, 1024 - 0x1a0 - 16, reinterpret_cast<uintptr_t>(controller.data()));
    set(stack, 1280 + 48, uintptr_t(0x98760001));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller));
    set(stack, 1280 + 48, uintptr_t(0x98760000));
    set(stack, 1024 - 0x38, uintptr_t(0x12345678));
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller));
    set(stack, 1024 - 0x38, reinterpret_cast<uintptr_t>(queued));
    image[0xd1f8] ^= 1;
    assert(!readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller));
    image[0xd1f8] ^= 1;
    // Explicit ACQP must use Quality even if a client also sets AverageBitRate
    // and therefore constructs the native VBR controller.
    set(controller, 8 + 40, uint32_t(1));
    set(vbr, 0, base + kVbrVtable);
    allocations[2].second = vbr.size();
    assert(readRateControl(payload, base + kScheduleReturn, image.data(), true,
        true, allocationSize, input, &reason, temporary, &caller, true));
    assert(input.mode == 1 && input.qualityValid && input.qp == 10);
    std::puts("PASS exact HEVC plug-in rate/quality/VBV fields and allocation guards");
}
