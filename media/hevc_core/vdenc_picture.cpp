// SPDX-License-Identifier: MIT
#include "vdenc_picture.hpp"

#include <algorithm>

#include "hw/mhw_vdbox_vdenc_hwcmd_g12_X.h"

namespace HevcCore {
namespace VdEnc {
namespace {

using Cmd1Command = mhw_vdbox_vdenc_g12_X::VDENC_CMD1_CMD;
using Cmd2Command = mhw_vdbox_vdenc_g12_X::VDENC_CMD2_CMD;

int clampInt(int value, int low, int high) {
    return std::max(low, std::min(high, value));
}

// HEVC mode-cost table, indexed by picture type and QP (iHD DW26 table1).
constexpr uint32_t kQpLambda[3][42] = {
    {0x30002,0x30002,0x30002,0x30003,0x40004,0x40005,0x50006,0x60008,0x6000a,0x7000c,0x8000f,
        0x90013,0xa0018,0xb001e,0xc0026,0xe0030,0x10003d,0x12004d,0x140061,0x16007a,0x19009a,
        0x1c00c2,0x1f00f4,0x230133,0x270183,0x2c01e8,0x320266,0x380306,0x3e03cf,0x4604cd,
        0x4f060c,0x58079f,0x63099a,0x6f0c18,0x7d0f3d,0x8c1333,0x9d1831,0xb11e7a,0xc62666,
        0xdf3062,0xfa3cf5,0x1184ccd},
    {0x30003,0x30003,0x30003,0x40003,0x40004,0x50005,0x50007,0x60008,0x6000a,0x7000d,0x80011,
        0x90015,0xa001a,0xb0021,0xd002a,0xe0034,0x100042,0x120053,0x140069,0x170084,0x1a00a6,
        0x1d00d2,0x210108,0x24014d,0x2901a3,0x2e0210,0x34029a,0x3a0347,0x410421,0x490533,
        0x52068d,0x5c0841,0x670a66,0x740d1a,0x821082,0x9214cd,0xa41a35,0xb82105,0xce299a,
        0xe8346a,0x1044209,0x1245333},
    {0x30003,0x30003,0x30003,0x40003,0x40004,0x50005,0x50007,0x60008,0x6000a,0x7000d,0x80011,
        0x90015,0xa001a,0xb0021,0xd002a,0xe0034,0x100042,0x120053,0x140069,0x170084,0x1a00a6,
        0x1d00d2,0x210108,0x24014d,0x2901a3,0x2e0210,0x34029a,0x3a0347,0x410421,0x490533,
        0x52068d,0x5c0841,0x670a66,0x740d1a,0x821082,0x9214cd,0xa41a35,0xb82105,0xce299a,
        0xe8346a,0x1044209,0x1245333},
};

// iHD DW26 SadQpLambda table for the visual quality path.
constexpr uint32_t kSadQpLambda[3][30] = {
    {0xa,0xb,0xd,0xf,0x11,0x14,0x17,0x1a,0x1e,0x22,0x27,0x2d,0x33,0x3b,0x43,0x4d,0x57,0x64,
        0x72,0x82,0x95,0xa7,0xbb,0xd2,0xec,0x109,0x129,0x14e,0x177,0x1a5},
    {0xa,0xc,0xe,0x10,0x12,0x15,0x18,0x1b,0x1f,0x23,0x29,0x2f,0x35,0x3d,0x46,0x50,0x5b,
        0x68,0x77,0x88,0x9b,0xae,0xc3,0xdb,0xf6,0x114,0x136,0x15c,0x186,0x1b6},
    {0xa,0xc,0xe,0x10,0x12,0x15,0x18,0x1b,0x1f,0x23,0x29,0x2f,0x35,0x3d,0x46,0x50,0x5b,
        0x68,0x77,0x88,0x9b,0xae,0xc3,0xdb,0xf6,0x114,0x136,0x15c,0x186,0x1b6},
};

// CMD1 lambda table for the visual quality path.
constexpr uint32_t kCmd1Lambda[30] = {
    0x0,0x60000,0xc0000,0x120000,0x190000,0x1f0000,0x250000,0x2c0000,0x320000,0x380000,
    0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,
    0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,0x3f0000,
    0x3f0000,0x3f0000,
};

unsigned pictureIndex(bool intra, bool lowDelayB) {
    if (intra)
        return 0;
    return lowDelayB ? 2 : 1;
}

}

Packet cmd1(const Cmd1Params &params) {
    Cmd1Command cmd {};
    cmd.DW22.Value = cmd.DW23.Value = cmd.DW24.Value = cmd.DW25.Value = 0x10101010;
    cmd.DW26.Value = cmd.DW27.Value = cmd.DW28.Value = cmd.DW29.Value = 0x10101010;

    // HEVC baseline values from iHD AddVdencCmd1Cmd.
    cmd.DW1.Value = 0x05030200;
    cmd.DW2.Value = 0x0B090806;
    cmd.DW3.Value = 0x1C140C04;
    cmd.DW4.Value = 0x3C342C24;
    cmd.DW5.Value = 0x5C544C44;
    cmd.DW6.Value = 0x1C140C04;
    cmd.DW7.Value = 0x3C342C24;
    cmd.DW8.Value = 0x5C544C44;
    cmd.DW14.Value = 0;
    cmd.DW15.Value = 0;
    cmd.DW16.Value &= 0xffff0000;
    cmd.DW19.Value = (cmd.DW19.Value & 0xff0000ff) | 0x140400;
    cmd.DW20.Value = 0x14141414;
    cmd.DW21.Value = 0x14141414;

    const int qpPrime = clampInt(static_cast<int>(params.qpY) + params.sliceQpDelta, 10, 51);
    if (params.visualQuality && qpPrime >= 22)
        cmd.DW14.Value = (cmd.DW14.Value & 0xff00ffff) | kCmd1Lambda[qpPrime - 22];

    if (params.intra) {
        cmd.DW10.Value = 0x23131f0f;
        cmd.DW11.Value = (cmd.DW11.Value & 0xffff0000) | 0x2313;
        cmd.DW12.Value = 0x3e5c445c;
        cmd.DW13.Value = (cmd.DW13.Value & 0xff00) | 0x1e040044;
        cmd.DW16.Value = (cmd.DW16.Value & 0xffff) | 0x70000;
        cmd.DW17.Value = 0x0d0e1007;
        cmd.DW18.Value = (cmd.DW18.Value & 0xffffff00) | 0x32;
        cmd.DW18.Value = (cmd.DW18.Value & 0xffff00ff) | (params.roi ? 0 : 0x1e00);
        cmd.DW30.Value = (cmd.DW30.Value & 0xff000000) | 0x101010;
    }
    return packetFrom(cmd);
}

Packet cmd2(const Cmd2Params &params) {
    Cmd2Command cmd {};

    // Shared baseline words, applied before the codec specific values.
    cmd.DW2.Value = (cmd.DW2.Value & 0xdff00000) | 0x2005aff3;
    cmd.DW3.Value = 0xfe02ff01;
    cmd.DW4.Value = 0xfc04fd03;
    cmd.DW5.Value = (cmd.DW5.Value & 0xff7e03ff) | 0x0080ac00;
    cmd.DW7.Value = (cmd.DW7.Value & 0xffd90ff0) | 0x00062003;
    cmd.DW9.Value = (cmd.DW9.Value & 0xffff) | 0x43840000;
    cmd.DW12.Value = 0xffffffff;
    cmd.DW15.Value = 0x4e201f40;
    cmd.DW16.Value = (cmd.DW16.Value & 0xf0ff0000) | 0x0f003300;
    cmd.DW17.Value = (cmd.DW17.Value & 0xfff00000) | 0x2710;
    cmd.DW19.Value = (cmd.DW19.Value & 0x80ffffff) | 0x18000000;
    cmd.DW21.Value &= 0x0fffffff;
    cmd.DW22.Value = 0x1f001102;
    cmd.DW23.Value = 0xaaaa1f00;
    cmd.DW27.Value = (cmd.DW27.Value & 0xffff0000) | 0x1a1a;

    cmd.DW1.FrameWidthInPixelsMinusOne = params.frameWidth - 1;
    cmd.DW1.FrameHeightInPixelsMinusOne = params.frameHeight - 1;

    // PICTURE_TYPE: 0 = I, 1 = P. The reference driver's ternary maps every
    // non-I HEVC picture to 2 (or 3 for a low-delay B), which is correct for a
    // B slice; a P slice must carry P or the VDEnc predicts with a list the
    // slice syntax does not have.
    cmd.DW2.PictureType = params.intra ? 0 : (params.lowDelayB ? 3 : params.pictureType);
    cmd.DW2.TemporalMvpEnableFlag = (!params.intra && params.temporalMvp) ? 1 : 0;
    cmd.DW2.TransformSkip = params.transformSkip ? 1 : 0;

    cmd.DW5.NumRefIdxL0Minus1 = params.numRefIdxL0;
    cmd.DW5.NumRefIdxL1Minus1 = params.numRefIdxL1;
    cmd.DW5.Value = (cmd.DW5.Value & 0xff83ffff) | 0x00400000;
    cmd.DW14.Value = (cmd.DW14.Value & 0xffff) | 0x07d00000;
    cmd.DW17.Value = (cmd.DW17.Value & 0xfff00000) | 0x2710;
    cmd.DW18.Value = (cmd.DW18.Value & 0xffff) | 0x00600000;
    cmd.DW19.Value = (cmd.DW19.Value & 0xffff0000) | 0xc0;
    cmd.DW20.Value &= 0xfffeffff;
    cmd.DW7.TilingEnable = params.tilesEnabled ? 1 : 0;
    cmd.DW37.TileReplayEnable = params.tileReplay ? 1 : 0;

    if (!params.intra) {
        // Each reference slot carries currPoc - refPoc clipped to -16..16.
        // Unused slots describe POC 0, matching the reference driver, which
        // reads an invalid frame index as POC 0. The long-term flags stay
        // clear because this path only uses short-term references.
        const auto pocDelta = [&](uint32_t refPoc) {
            return static_cast<uint32_t>(clampInt(
                static_cast<int>(params.currPoc) - static_cast<int>(refPoc),
                -16, 16)) & 0xFF;
        };
        cmd.DW3.PocNumberForRefid0InL0 = pocDelta(params.refPocL0[0]);
        cmd.DW3.PocNumberForRefid1InL0 = pocDelta(params.refPocL0[1]);
        cmd.DW4.PocNumberForRefid2InL0 = pocDelta(params.refPocL0[2]);
        cmd.DW3.PocNumberForRefid0InL1 = pocDelta(params.refPocL1[0]);
        cmd.DW3.PocNumberForRefid1InL1 = cmd.DW3.PocNumberForRefid1InL0;
        cmd.DW4.PocNumberForRefid2InL1 = cmd.DW4.PocNumberForRefid2InL0;
        cmd.DW5.SubPelMode = 3;
    } else {
        cmd.DW3.Value = 0;
        cmd.DW4.Value = 0;
    }

    switch (params.targetUsage) {
    case 1:
        cmd.DW2.Value &= 0xdfffffff;
        cmd.DW2.Value = (cmd.DW2.Value & 0xfffffffc) | (params.intra ? 2 : 3);
        cmd.DW7.Value &= 0xfffffeff;
        cmd.DW7.Value = (cmd.DW7.Value & 0xfffff7ff) | (params.intra ? 0x800 : 0);
        cmd.DW9.Value = (cmd.DW9.Value & 0xfffff) | 0x43800000;
        cmd.DW12.Value = 0xffffffff;
        cmd.DW34.Value = (cmd.DW34.Value & 0xffffff) | 0x21000000;
        break;
    case 4:
        cmd.DW2.Value &= 0xdfffffff;
        cmd.DW7.Value &= 0xfffffeff;
        cmd.DW9.Value = (cmd.DW9.Value & 0xfffff) | 0x43800000;
        cmd.DW12.Value = 0xce4014a0;
        cmd.DW34.Value = (cmd.DW34.Value & 0xffffff) | 0x21000000;
        break;
    case 7:
        cmd.DW2.Value = (cmd.DW2.Value & 0xdfffffff) | 0x20000000;
        cmd.DW7.Value = (cmd.DW7.Value & 0xfff7feff) | 0x00080100;
        cmd.DW9.Value = (cmd.DW9.Value & 0xffff) | 0x22420000;
        cmd.DW12.Value = 0x89800dc0;
        cmd.DW34.Value = (cmd.DW34.Value & 0xffffff) | 0x20000000;
        break;
    default:
        break;  // unsupported target usage keeps the baseline words
    }

    if (params.numRefIdxL0 == 0)
        cmd.DW7.Value = (cmd.DW7.Value & 0xfff7ffff) | 0x00080000;

    if (!params.lowDelayB)
        cmd.DW7.Value &= 0xfff7feff;

    cmd.DW7.VdencStreamInEnable = params.streamIn ? 1 : 0;
    cmd.DW7.PakOnlyMultiPassEnable = params.pakOnlyMultipass ? 1 : 0;

    if (params.lowDelayB) {
        cmd.DW8.Value = 0;
        cmd.DW9.Value &= 0xffff0000;
    } else {
        cmd.DW8.Value = 0x54555555;
        cmd.DW9.Value = (cmd.DW9.Value & 0xffff0000) | 0x5555;
    }

    cmd.DW16.MinQp = params.brcMinQp < 0x0a ? 10 : params.brcMinQp;
    cmd.DW16.MaxQp = params.brcMaxQp < 0x0a ? 51 : (params.brcMaxQp > 0x33 ? 0x33 : params.brcMaxQp);
    cmd.DW17.TemporalMVEnableForIntegerSearch = (params.lowDelayB && cmd.DW2.TemporalMvpEnableFlag) ? 1 : 0;

    const int qpPrime = clampInt(static_cast<int>(params.qpY) + params.sliceQpDelta, 10, 51);
    cmd.DW26.Value = (cmd.DW26.Value & 0xfe000000)
        | kQpLambda[pictureIndex(params.intra, params.lowDelayB)][qpPrime - 10];

    if (params.visualQuality) {
        if (params.intra && qpPrime >= 22)
            cmd.DW26.SadQpLambda = kSadQpLambda[0][qpPrime - 22];
        if (params.qpY >= 22 && params.qpY <= 51)
            cmd.DW6.Value = (cmd.DW6.Value & 0xc00fffff) | 0x1fb00000;
    }

    cmd.DW27.QpPrimeYAc = static_cast<uint32_t>(qpPrime);
    cmd.DW27.Value &= 0xffffff00;
    cmd.DW35.Value = (cmd.DW35.Value & 0xfffff0ff) | 0x700;

    if (params.useDefaultQpDeltas) {
        cmd.DW13.Value = (cmd.DW13.Value & 0xffff) | 0xf0120000;
        if (params.intra) {
            cmd.DW14.Value = (cmd.DW14.Value & 0xffff0000) | 0x21db;
            cmd.DW16.Value = (cmd.DW16.Value & 0xf00ffff) | 0x10000;
            cmd.DW18.Value = 0x600000;
            cmd.DW19.Value = (cmd.DW19.Value & 0xffff0000) | 0xc0;
        } else {
            cmd.DW14.Value = (cmd.DW14.Value & 0xffff0000) | 0x21ed;
            cmd.DW16.Value = (cmd.DW16.Value & 0xf00ffff) | 0xd0010000;
            cmd.DW18.Value = 0x60010f;
            cmd.DW19.Value = (cmd.DW19.Value & 0xffff0000) | 0xc0;
        }
    }

    if (params.rounding) {
        cmd.DW28.Value = 0x07d00fa0;
        cmd.DW29.Value = 0x02bc0bb8;
        cmd.DW30.Value = 0x032003e8;
        cmd.DW31.Value = 0x01f4012c;
        cmd.DW32.Value = (cmd.DW32.Value & 0xffff0000) | 0x190;

        const uint32_t interIndex = params.roundInter - 2;
        const uint32_t intraIndex = params.roundIntra - 8;
        if (interIndex < 3 && intraIndex < 3) {
            const uint32_t table3[3][3] = {{0x88220000, 0x99220000, 0xaa220000},
                {0x88330000, 0x99330000, 0xaa330000},
                {0x88440000, 0x99440000, 0xaa440000}};
            cmd.DW32.Value = (cmd.DW32.Value & 0xffff) | table3[interIndex][intraIndex];

            const uint32_t table4[3][3] = {{0x22882222, 0x22992222, 0x22aa2222},
                {0x33883333, 0x33993333, 0x33aa3333},
                {0x44884444, 0x44994444, 0x44aa4444}};
            cmd.DW33.Value = table4[interIndex][intraIndex];

            const uint32_t table5[3][3] = {{0x228822, 0x229922, 0x22aa22},
                {0x338833, 0x339933, 0x33aa33},
                {0x448844, 0x449944, 0x44aa44}};
            cmd.DW34.Value = (cmd.DW34.Value & 0xff000000) | table5[interIndex][intraIndex];
        }
    }

    // Reference index mapping: one byte per VDEnc reference slot. The
    // reference initializes the three L0 slots to 0x7 and only overwrites the
    // slots that hold a real reference. A low-delay B slice has no separate L1
    // slot either, so the fourth byte stays zero and bit 31 marks the picture
    // as low-delay B; a picture with a real L1 list stores that list's frame
    // index there instead.
    for (uint32_t i = 0; i < 4; ++i)
        reinterpret_cast<uint8_t *>(&cmd.DW11.Value)[i] = 0x7;
    for (uint32_t i = 0; i <= params.numRefIdxL0 && i < 3; ++i)
        if (params.refL0[i] < 8)
            reinterpret_cast<uint8_t *>(&cmd.DW11.Value)[i] = params.refIdxMapping[params.refL0[i]];
    if (params.lowDelayB)
        reinterpret_cast<uint8_t *>(&cmd.DW11.Value)[3] = 0;
    else if (params.refL1[0] < 8)
        reinterpret_cast<uint8_t *>(&cmd.DW11.Value)[3] = params.refIdxMapping[params.refL1[0]];
    cmd.DW11.Value = (cmd.DW11.Value & 0x7fffffff) | 0x80000000;

    return packetFrom(cmd);
}

}
}
