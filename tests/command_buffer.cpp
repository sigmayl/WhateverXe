// CPU check for command-buffer accounting, GPU address patches and the batch
// groups used by the Gen12 HEVC VDEnc path.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_vdenc_hwcmd_g12_X.cpp"
#include "../media/hevc_core/command_buffer.cpp"
#include "../media/hevc_core/hcp_state.cpp"
#include "../media/hevc_core/hcp_picture.cpp"
#include "../media/hevc_core/hcp_slice.cpp"
#include "../media/hevc_core/hcp_pak_insert.cpp"
#include "../media/hevc_core/vdenc_picture.cpp"
#include "../media/hevc_core/vdenc_weights.cpp"

int main() {
    using namespace HevcCore;

    BatchBuffer batch(4096);
    batch.beginGroup(GroupKind::First);
    batch.append(Hcp::pipeModeSelect(Hcp::PipeModeSelect {}));
    batch.appendBatchBufferEnd();
    batch.endGroup();

    batch.beginGroup(GroupKind::Second);
    batch.append(VdEnc::cmd1(VdEnc::Cmd1Params {}));
    batch.append(Hcp::picState(Hcp::PicStateParams {}));
    batch.append(VdEnc::cmd2(VdEnc::Cmd2Params {}));
    batch.appendBatchBufferEnd();
    batch.endGroup();

    batch.beginGroup(GroupKind::Slice);
    batch.append(Hcp::sliceState(Hcp::SliceParams {}));
    const uint8_t header[] = {0x00, 0x00, 0x01, 0x02};
    Hcp::PakInsertParams pak;
    pak.data = header;
    pak.bitSize = 32;
    pak.lastHeader = true;
    auto insert = Hcp::pakInsert(pak);
    batch.append(insert.header);
    batch.append(insert.payload);
    batch.append(VdEnc::weightsOffsetsState(VdEnc::WeightsParams {}));
    batch.appendBatchBufferEnd();
    batch.appendNoop();
    batch.appendNoop();
    batch.endGroup();

    assert(!batch.overflowed());
    assert(batch.size() == 616);

    const GroupRange *first = batch.group(GroupKind::First);
    const GroupRange *second = batch.group(GroupKind::Second);
    const GroupRange *slice = batch.group(GroupKind::Slice);
    assert(first && first->start == 0 && first->size == 32);
    assert(second && second->start == 32 && second->size == 496);
    assert(slice && slice->start == 528 && slice->size == 88);
    assert(batch.group(GroupKind::First, 1) == nullptr);

    const std::vector<uint8_t> &bytes = batch.bytes();
    auto word = [&bytes](uint32_t offset) {
        return static_cast<uint32_t>(bytes[offset])
            | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    };
    assert(word(0) == 0x73800005);       // first group starts with pipe mode select
    assert(word(28) == kMiBatchBufferEnd);
    assert(word(32 + 492) == kMiBatchBufferEnd);
    assert(word(528 + 52 + 12 + 12) == kMiBatchBufferEnd);
    assert(word(612) == kMiNoop && word(608) == kMiNoop);

    batch.patches().add(4, 0x12345678);
    batch.patches().add(32, 0xA5A55A5A);
    assert(batch.patches().size() == 2);
    std::vector<uint8_t> patched = bytes;
    batch.patches().apply(patched);
    assert(patched[4] == 0x78 && patched[5] == 0x56 && patched[6] == 0x34 && patched[7] == 0x12);
    assert(patched[32] == 0x5A && patched[33] == 0x5A && patched[34] == 0xA5 && patched[35] == 0xA5);

    BatchBuffer small(8);
    Packet oversized(3);
    small.append(oversized);
    assert(small.overflowed());
    assert(small.size() == 0);

    puts("PASS Gen12 HEVC command buffer and batch groups");
    return 0;
}
