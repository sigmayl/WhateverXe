// SPDX-License-Identifier: MIT
// Mock transport verifies firmware ownership, reset scope and failure recovery.
#include "../media/hevc_core/uc_firmware.cpp"
#include "../media/hevc_core/guc_boot.cpp"
#include "../media/hevc_core/uc_runtime.cpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

using namespace HevcCore::Uc;
struct Mock {
    std::map<uint32_t, uint32_t> registers {{0xc000, 1}};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::vector<uint32_t> dmaSizes;
    bool owner {true}, mappings {true}, retain {true}, retained {}, flushed {}, ct {};
    bool resetWorks {true}, hucDma {true}, gucDma {true}, gucBoot {true};
    bool registration {true}, authentication {true}, hucReady {true};
    unsigned sleeps {}, retainCalls {}, ctCalls {}, authCalls {};
};
static RuntimeOps operations(Mock &m) {
    RuntimeOps ops;
    ops.context = &m;
    ops.ownershipValid = [](void *p) { return static_cast<Mock *>(p)->owner; };
    ops.mappingsValid = [](void *p) { return static_cast<Mock *>(p)->mappings; };
    ops.flushMappings = [](void *p) { static_cast<Mock *>(p)->flushed = true; };
    ops.retainUntilReboot = [](void *p) {
        auto &m = *static_cast<Mock *>(p);
        ++m.retainCalls;
        assert(m.owner && m.mappings && m.flushed);
        if (!m.retain || m.retained) return false;
        m.retained = true;
        return true;
    };
    ops.read = [](void *p, uint32_t reg) { return static_cast<Mock *>(p)->registers[reg]; };
    ops.write = [](void *p, uint32_t reg, uint32_t value) {
        auto &m = *static_cast<Mock *>(p);
        assert(m.retained && m.owner && m.mappings && m.flushed);
        m.writes.emplace_back(reg, value);
        if (reg == 0x941c) {
            assert(value == 8); // no full-GT, render, media or display reset
            m.registers[reg] = m.resetWorks ? 0 : value;
            m.registers[0xc000] = 1;
        } else if (reg == 0xc050 || reg == 0xc340) {
            m.registers[reg] = value | 1; // hardware locks/validates WOPCM
        } else if (reg == 0xc314) {
            if (value & 1) {
                const bool huc = value & (1 << 9);
                m.dmaSizes.push_back(m.registers[0xc310]);
                assert(m.dmaSizes.size() == (huc ? 1u : 2u));
                assert(m.registers[0xc308] == (huc ? 0u : 0x2000u));
                assert(m.registers[0xc30c] == (7u << 16));
                m.registers[reg] = (huc ? m.hucDma : m.gucDma) ? 0 : 1;
                if (!huc && m.gucDma) m.registers[0xc000] = m.gucBoot ? 0x8002f076 : 1;
            } else {
                m.registers[reg] = 0;
            }
        } else {
            m.registers[reg] = value;
        }
    };
    ops.sleep = [](void *p, uint32_t milliseconds) {
        assert(milliseconds == 1);
        ++static_cast<Mock *>(p)->sleeps;
    };
    ops.registerCt = [](void *p) {
        auto &m = *static_cast<Mock *>(p);
        ++m.ctCalls;
        assert(gen12FirmwareReady(m.registers[0xc000], 1));
        m.ct = m.registration;
        return m.ct;
    };
    ops.authenticateHuC = [](void *p) {
        auto &m = *static_cast<Mock *>(p);
        ++m.authCalls;
        assert(m.ct);
        if (m.authentication && m.hucReady) m.registers[0xc1dc] = 0x90001;
        return m.authentication;
    };
    return ops;
}
static RuntimeConfig configuration(const std::array<uint32_t, 64> &rsa) {
    RuntimeConfig c;
    c.guc = {{70,49,4},335104,335104,256,0x801000};
    c.huc = {{7,9,3},589632,589632,256,0};
    c.gucAddress = 0x400000; c.hucAddress = 0x500000;
    c.adsAddress = 0x800000; c.logAddress = 0x1200000;
    c.gucRsa = rsa.data(); c.gucRsaBytes = 256;
    assert(buildGen12Guc70AuthenticationParameters(c.guc,0xa780,4,
        c.adsAddress,c.logAddress,c.parameters));
    return c;
}
int main() {
    std::array<uint32_t, 64> rsa;
    for (uint32_t i = 0; i < rsa.size(); ++i) rsa[i] = 0x12340000 + i;
    const auto config = configuration(rsa);
    assert(gen12FirmwareReady(0x8002f076,0x90001));
    assert(!gen12FirmwareReady(0x8002f076,0));
    assert(!gen12FirmwareReady(0x4000f000,1));
    assert(!gen12FirmwareReady(0x8000ef00,1));
    assert(!gen12FirmwareReady(0xffffffff,1));
    assert(!gen12FirmwareReady(0x8002f076,0xffffffff));
    for (bool locked : {false,true}) {
        Mock m;
        if (locked) {m.registers[0xc050] = 0x163001; m.registers[0xc340] = 0x94003;}
        auto ops = operations(m);
        RuntimeState state;
        assert(initializeGen12Runtime(config,ops,state));
        assert(state.phase == RuntimePhase::Authenticated && state.retained && m.retained);
        assert(m.retainCalls == 1 && m.ctCalls == 1 && m.authCalls == 1);
        assert((m.dmaSizes == std::vector<uint32_t>{589632,335104}));
        assert((m.writes.front() == std::make_pair(0x941cu,8u)));
        for (uint32_t i = 0; i < rsa.size(); ++i) assert(m.registers[0xc200 + 4*i] == rsa[i]);
        assert(m.registers[0xc18c] == (1u << 14)); // host scheduler retained
        if (locked) for (const auto &write : m.writes)
            assert(write.first != 0xc050 && write.first != 0xc340);
        const auto writes = m.writes;
        assert(initializeGen12Runtime(config,ops,state));
        assert(m.writes == writes && m.retainCalls == 1);
        // Losing authentication invalidates this entire lifetime. A stale
        // registry result cannot reauthorize HuC or a second upload.
        m.registers[0xc1dc] = 0;
        assert(!refreshGen12Runtime(ops,state));
        assert(state.phase == RuntimePhase::Unavailable && state.retained);
        m.registers[0xc1dc] = 0x90001;
        assert(!refreshGen12Runtime(ops,state));
        assert(!initializeGen12Runtime(config,ops,state) && m.writes == writes);
    }
    for (unsigned fault = 0; fault < 9; ++fault) {
        Mock m;
        auto c = config;
        RuntimeState state;
        if (fault == 0) m.owner = false;
        if (fault == 1) m.mappings = false;
        if (fault == 2) m.retain = false;
        if (fault == 3) m.registers[0xc000] = 0x8002f076; // another firmware owner
        if (fault == 4) m.registers[0xc1dc] = 1;
        if (fault == 5) m.registers[0xc314] = 1;
        if (fault == 6) m.registers[0xc000] = 0xffffffff;
        if (fault == 7) m.registers[0xc050] = 0x163001; // partially locked layout
        if (fault == 8) c.parameters.words[2] = 0; // changing scheduling is outside scope
        assert(!initializeGen12Runtime(c,operations(m),state));
        assert(m.writes.empty() && !m.retained && !state.retained);
    }
    for (RuntimeFailure fault : {RuntimeFailure::Reset,RuntimeFailure::HuCDma,
        RuntimeFailure::GuCDma,RuntimeFailure::GuCBoot,RuntimeFailure::CtRegister,
        RuntimeFailure::CtAuthenticate,RuntimeFailure::HuCAuthenticate}) {
        Mock m;
        if (fault == RuntimeFailure::Reset) m.resetWorks = false;
        if (fault == RuntimeFailure::HuCDma) m.hucDma = false;
        if (fault == RuntimeFailure::GuCDma) m.gucDma = false;
        if (fault == RuntimeFailure::GuCBoot) m.gucBoot = false;
        if (fault == RuntimeFailure::CtRegister) m.registration = false;
        if (fault == RuntimeFailure::CtAuthenticate) m.authentication = false;
        if (fault == RuntimeFailure::HuCAuthenticate) m.hucReady = false;
        RuntimeState state;
        auto ops = operations(m);
        assert(!initializeGen12Runtime(config,ops,state));
        assert(state.failure == fault && state.phase == RuntimePhase::Unavailable);
        assert(state.retained && m.retained && !m.writes.empty());
        const auto writes = m.writes;
        assert(!initializeGen12Runtime(config,ops,state));
        assert(m.writes == writes && m.retainCalls == 1 && state.failure == fault);
    }
    std::puts("PASS guarded Gen12 firmware ownership, transport, authentication and recovery");
}
