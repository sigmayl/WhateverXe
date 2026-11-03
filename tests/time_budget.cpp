// SPDX-License-Identifier: BSD-3-Clause
#include "../media/hevc_core/brc_window.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace HevcCore;
constexpr uint64_t ns = FrameBudget::second;
static FrameBudgetConfig config(uint32_t bits = 1000000, uint64_t duration = ns) {
    return {1000000, 2000000, 1500000, 30, 1, 1, {{30,bits,duration},{}},true};
}
int main() {
    FrameBudget b{}; auto c = config();
    assert(b.advance(0) && b.configure(c));
    assert(b.availableBits() == 1000000 && b.commit(600000));
    assert(b.advance(ns/100));
    c.fpsNumerator = c.windows[0].frames = 150;
    assert(b.configure(c) && b.availableBits() == 400000);
    c.fpsNumerator = 1; c.fpsDenominator = 4; c.windows[0].frames = 1;
    assert(b.configure(c) && b.availableBits() == 400000);
    assert(b.commit(400000) && !b.availableBits());
    assert(b.nextTimeFor(1000) == ns);
    assert(b.advance(ns-1) && !b.availableBits());
    assert(b.advance(ns) && b.availableBits() == 600000 && b.commit(1000));
    const auto before = b.availableBits();
    assert(!b.advance(ns-1) && b.availableBits() == before);
    assert(b.nextTimeFor(1000001) == UINT64_MAX);

    // Low activity and true rate/limit changes retain output until it expires.
    assert(b.advance(ns+ns/2));
    c.windows[0].durationNs = 2*ns;
    assert(b.configure(c) && !b.availableBits());
    assert(b.nextTimeFor(1000) == 2*ns+ns/100);
    assert(b.advance(2*ns+ns/100) && b.availableBits() >= 1000);
    c.windows[0].bits = 500;
    assert(b.configure(c) && !b.availableBits());
    assert(b.nextTimeFor(500) == 3*ns);
    assert(b.advance(3*ns) && b.availableBits() == 500);

    // Bounded timestamp storage must pace, never discard unexpired packets.
    FrameBudget dense{}; c=config();
    assert(dense.advance(0) && dense.configure(c));
    for (unsigned i=0;i<256;++i) assert(dense.commit(1));
    assert(!dense.availableBits() && dense.nextTimeFor(1)==ns);
    assert(dense.advance(ns) && dense.commit(1));

    // Check independent rolling byte sums across irregular capture intervals.
    FrameBudget stream{}; c=config();c.windowCount=2;
    c.windows[1]={60,1300000,2*ns};
    assert(stream.advance(0) && stream.configure(c));
    struct Packet { uint64_t time; uint32_t bits; };
    std::vector<Packet> packets;uint64_t now=0;unsigned waits=0;
    const uint64_t intervals[]{1000000,17000000,50000000,250000000};
    const uint32_t rates[]{500,60,120,1};
    for (unsigned i=0;i<1200;++i) {
        now+=intervals[i%4];
        assert(stream.advance(now));
        c.fpsNumerator=rates[i%4];c.fpsDenominator=i%4==3?4:1;
        c.windows[0].frames=rates[i%4];
        assert(stream.configure(c));
        const uint32_t bits=i%13==0?600000:30000;
        if(stream.availableBits()<bits) {
            now=stream.nextTimeFor(bits);assert(now!=UINT64_MAX);
            assert(stream.advance(now));++waits;
        }
        assert(stream.commit(bits));packets.push_back({now,bits});
        for(const auto &w:c.windows) {
            uint64_t used=0;
            for(const auto &p:packets)if(now-p.time<w.durationNs)used+=p.bits;
            assert(used<=w.bits);
        }
    }
    assert(waits);
    c.windows[0].durationNs=257*ns;assert(!stream.configure(c));
    FrameBudget large{};
    FrameBudgetConfig edge{UINT32_MAX,UINT32_MAX,UINT32_MAX,1,1,0,{},true};
    assert(large.advance(0) && large.configure(edge) && large.commit(UINT32_MAX));
    assert(large.advance(UINT64_MAX) && large.availableBits()==UINT32_MAX);
    puts("PASS monotonic bitrate windows, FPS changes, pacing and retained history");
}
