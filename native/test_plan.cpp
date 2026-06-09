// Console unit test for the pure pass-planner (qw::Plan). No disk, no admin:
// it only exercises the planning logic that decides what gets overwritten and in
// what order. Build with native\test-plan.ps1; exit code 0 => all checks passed.
#include "core.h"
#include <cstdio>
#include <algorithm>
#include <vector>

using namespace qw;

static int g_pass = 0, g_fail = 0;
static void Check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}

static const long long MB = 1024LL * 1024;

// Verify a Quick/Quick2 plan: map-kill front+tail present, fill chunks are exactly
// `chunk`-sized (last may be partial), every byte of [0,total) is covered at least
// once (=> converges to a full wipe), and the fill is NOT in plain ascending order
// (it's the bit-reversed spread).
static void CheckSpreadPlan(const char* label, long long total, Mode mode, long long chunk) {
    std::printf("- %s (total=%lld MB, chunk=%lld MB)\n", label, total / MB, chunk / MB);
    auto ops = Plan(total, mode);
    Check("plan is non-empty", !ops.empty());

    // Collect fill ops (pass == "Spread fill") and map-kill ops separately.
    std::vector<WriteOp> fill, map;
    for (auto& o : ops) (std::string(o.pass) == "Spread fill" ? fill : map).push_back(o);

    bool mapFront = false, mapTail = false;
    for (auto& o : map) {
        if (o.offset == 0) mapFront = true;
        if (o.offset + o.length == total && o.offset > 0) mapTail = true;
    }
    Check("map-kill hits the front", mapFront);
    Check("map-kill hits the tail (backup GPT)", mapTail || total <= 20 * MB);

    long long nChunks = (total + chunk - 1) / chunk;
    Check("one fill op per chunk", (long long)fill.size() == nChunks);

    // Every fill offset is chunk-aligned; offsets form exactly {0, chunk, 2*chunk, ...}.
    std::vector<long long> offs;
    bool aligned = true, sized = true;
    for (auto& o : fill) {
        offs.push_back(o.offset);
        if (o.offset % chunk != 0) aligned = false;
        long long expat = (o.offset + chunk <= total) ? chunk : (total - o.offset);
        if (o.length != expat) sized = false;
    }
    Check("every fill op is chunk-aligned", aligned);
    Check("every fill op is chunk-sized (last partial ok)", sized);

    std::sort(offs.begin(), offs.end());
    bool cover = ((long long)offs.size() == nChunks);
    for (long long i = 0; cover && i < nChunks; ++i) cover = (offs[i] == i * chunk);
    Check("fill covers [0,total) exactly once (=> converges to Full)", cover);

    // The fill must be spread (bit-reversed), not plain ascending order. (Bit-reversal
    // only reorders when there are >=3 chunks; with 1-2 chunks it is the identity.)
    bool ascending = true;
    for (size_t i = 1; i < fill.size(); ++i) if (fill[i].offset <= fill[i-1].offset) { ascending = false; break; }
    Check("fill is reordered, not plain ascending", !ascending || fill.size() <= 2);

    // Quick2's chunk must be smaller than Quick's => finer coverage.
    if (mode == Mode::Quick2) Check("Quick2 chunk is finer than Quick (8MB < 64MB)", chunk < 64 * MB);
}

int main() {
    std::printf("qw::Plan unit checks\n");

    // Full: a single sequential op over the whole device.
    {
        std::printf("- Full (total=128 GB)\n");
        auto ops = Plan(128LL * 1024 * MB, Mode::Full);
        Check("Full is a single op", ops.size() == 1);
        Check("Full covers the whole device from 0", !ops.empty() && ops[0].offset == 0 && ops[0].length == 128LL * 1024 * MB);
    }

    CheckSpreadPlan("Quick  116GB", 116LL * 1024 * MB, Mode::Quick,  64 * MB);
    CheckSpreadPlan("Quick2 116GB", 116LL * 1024 * MB, Mode::Quick2,  8 * MB);
    // non-power-of-two, non-multiple-of-chunk size:
    CheckSpreadPlan("Quick2 odd size", 30000LL * MB + 12345, Mode::Quick2, 8 * MB);
    // small device (< map front): still must fully cover.
    CheckSpreadPlan("Quick2 tiny 10MB", 10 * MB, Mode::Quick2, 8 * MB);
    // exactly one chunk + a bit:
    CheckSpreadPlan("Quick2 9MB", 9 * MB, Mode::Quick2, 8 * MB);

    // Empty / zero device => empty plan, no crash.
    Check("zero size => empty plan", Plan(0, Mode::Quick2).empty());

    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
