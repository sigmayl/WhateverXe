// SPDX-License-Identifier: MIT
#include "../media/hevc_core/guc_ct.cpp"
#include "../media/hevc_core/uc_firmware.cpp"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    using namespace HevcCore::Uc;
    alignas(64) std::array<uint8_t, kCtBlobBytes + 64> bytes;
    bytes.fill(0xa5);
    CtChannels channels;
    assert(initializeCtChannels(bytes.data(), kCtBlobBytes, 0x400000, channels));
    assert(channels.sendDescriptorAddress == 0x400000 && channels.receiveDescriptorAddress == 0x400800);
    assert(channels.sendAddress == 0x401000 && channels.receiveAddress == 0x402000);
    for (uint32_t i = 0; i < kCtBlobBytes; ++i) assert(bytes[i] == 0);
    for (uint32_t i = kCtBlobBytes; i < bytes.size(); ++i) assert(bytes[i] == 0xa5);
    const auto savedChannels = channels;
    assert(!initializeCtChannels(bytes.data(), kCtBlobBytes - 1, 0x400000, channels));
    assert(!initializeCtChannels(bytes.data() + 1, kCtBlobBytes, 0x400000, channels));
    assert(!initializeCtChannels(bytes.data(), kCtBlobBytes, 0x100000, channels));
    assert(!memcmp(&channels, &savedChannels, sizeof(channels)));
    uint32_t config[4];
    assert(buildCtSelfConfig(0x0903, channels.sendDescriptorAddress, config));
    assert(config[0] == 0x0508 && config[1] == 0x09030002 && config[2] == 0x400000 && !config[3]);
    assert(buildCtSelfConfig(0x0906, channels.receiveDescriptorAddress, config));
    assert(config[1] == 0x09060002 && config[2] == 0x400800);
    assert(buildCtSelfConfig(0x0904, kCtSendBytes, config));
    assert(config[1] == 0x09040001 && config[2] == kCtSendBytes && !config[3]);
    assert(!buildCtSelfConfig(0x0907, kCtSendBytes, config));
    assert(!buildCtSelfConfig(0x0903, 0x100000000ULL, config));
    assert(!buildCtSelfConfig(0x0905, kGucGgttTop - 4096, config));
    assert(!buildCtSelfConfig(0x0908, 0x400000, config));
    uint32_t control[2];
    buildCtControl(true, control);
    assert(control[0] == 0x4509 && control[1] == 1);
    buildCtControl(false, control);
    assert(control[0] == 0x4509 && control[1] == 0);

    const uint32_t authenticate[] {0x4000, 0x500000};
    assert(writeCtRequest(channels.send, authenticate, 2, 1) == CtResult::Ok);
    assert(channels.send.words[0] == 0x00010002 && channels.send.words[1] == 0x4000);
    assert(channels.send.words[2] == 0x500000 && channels.send.descriptor->tail == 3);
    auto &desc = *channels.send.descriptor;
    desc.head = desc.tail = channels.send.capacityDwords - 1;
    assert(writeCtRequest(channels.send, authenticate, 2, 0xffff) == CtResult::Ok);
    assert(channels.send.words[1023] == 0xffff0002 && channels.send.words[0] == 0x4000);
    assert(channels.send.words[1] == 0x500000 && desc.tail == 2);
    desc.head = 3; // leave one empty dword so full and empty remain distinct
    const auto savedBytes = bytes;
    assert(writeCtRequest(channels.send, authenticate, 2, 2) == CtResult::NoSpace);
    assert(!memcmp(bytes.data(), savedBytes.data(), bytes.size()));
    assert(writeCtRequest(channels.send, authenticate, 2, 0x10000) == CtResult::Invalid);
    desc.status = 1;
    assert(writeCtRequest(channels.send, authenticate, 2, 2) == CtResult::Broken);

    auto &recv = *channels.receive.descriptor;
    recv.head = 4095; recv.tail = 1;
    channels.receive.words[4095] = 0x00010001;
    channels.receive.words[0] = 0xf0000000;
    uint32_t payload[256] {};
    CtMessage message;
    assert(readCtMessage(channels.receive, payload, 256, message) == CtResult::Ok);
    assert(message.fence == 1 && message.dwords == 1 && recv.head == 1);
    uint32_t data = 0x123;
    assert(classifyHxgReply(message, payload, 1, data) == HxgReply::Success && data == 0);
    assert(classifyHxgReply(message, payload, 2, data) == HxgReply::Other);
    payload[0] = 0xe0000007;
    assert(classifyHxgReply(message, payload, 1, data) == HxgReply::Failure && data == 7);
    payload[0] = 0xb0000000;
    assert(classifyHxgReply(message, payload, 1, data) == HxgReply::Busy);
    payload[0] = 0xd0000000;
    assert(classifyHxgReply(message, payload, 1, data) == HxgReply::Retry);
    payload[0] = 0x70000000;
    assert(classifyHxgReply(message, payload, 1, data) == HxgReply::Invalid);
    assert(readCtMessage(channels.receive, payload, 256, message) == CtResult::Empty);
    recv.head = 2; recv.tail = 4;
    channels.receive.words[2] = 0x00010003; // producer advertised an incomplete message
    assert(readCtMessage(channels.receive, payload, 256, message) == CtResult::Invalid && recv.head == 2);
    channels.receive.words[2] = 0x00011001; // unknown CT format
    assert(readCtMessage(channels.receive, payload, 256, message) == CtResult::Invalid && recv.head == 2);
    channels.receive.words[2] = 0x00010001;
    assert(readCtMessage(channels.receive, payload, 0, message) == CtResult::TooSmall && recv.head == 2);
    recv.status = 4;
    assert(readCtMessage(channels.receive, payload, 256, message) == CtResult::Broken && recv.head == 2);
    std::puts("PASS GuC 70 CTB/HXG registration, wrapped messages, fences and corrupt-buffer rejection");
}
