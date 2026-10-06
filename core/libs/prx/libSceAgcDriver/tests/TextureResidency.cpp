#include "prx/libSceAgcDriver/Graphics/include/TextureResidency.hpp"
#include <algorithm>
#include <cstdio>
#include <list>
#include <unordered_map>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

constexpr std::uint64_t MiB = 1ull << 20u;
constexpr std::uint64_t GiB = 1ull << 30u;
int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

struct Entry {
    int id = 0;
    std::uint64_t listedUse = 0;
    std::uint64_t objectUse = 0;
    std::uint64_t device = 0;
    std::uint64_t host = 0;
};

struct Cache {
    std::list<Entry> entries;
    ResidencyUsage usage;
    std::vector<int> evicted;

    void Add(int id, std::uint64_t use, std::uint64_t device, std::uint64_t host = 0) {
        entries.push_front({id, use, use, device, host});
        usage.device += device;
        usage.host += host;
    }

    EvictionOutcome Pass(const ResidencyLimits& limits, const ResidencyWindow& window, const ResidencyPolicy& policy = {}) {
        auto usageCopy = usage;
        const auto outcome = RunEvictionPass(
                entries, usageCopy, limits, window, policy,
                [](const Entry& entry) { return ResidencyEntryState{entry.listedUse, std::max(entry.listedUse, entry.objectUse), entry.device, entry.host}; },
                [&](std::list<Entry>::iterator it) {
                    it->listedUse = std::max(it->listedUse, it->objectUse);
                    entries.splice(entries.begin(), entries, it);
                },
                [&](std::list<Entry>::iterator it) {
                    usage.device -= it->device;
                    usage.host -= it->host;
                    evicted.push_back(it->id);
                    entries.erase(it);
                });
        Expect(usageCopy.device == usage.device && usageCopy.host == usage.host, "the pass's usage follows the evictions");
        return outcome;
    }
};

ResidencyLimits Limits(std::uint64_t deviceSoft, std::uint64_t deviceHard, std::uint64_t hostSoft = ~0ull, std::uint64_t hostHard = ~0ull) {
    return {deviceSoft, deviceHard, hostSoft, hostHard};
}

void NeverEvictsEntriesUsedThisFrame() {
    Cache cache;
    for (int i = 0; i < 40; ++i) cache.Add(i, 100 + i, 64 * MiB);
    const ResidencyWindow window{90, 100};
    const auto outcome = cache.Pass(Limits(0, 0), window);
    Expect(outcome.evicted == 0, "a critical pass leaves every entry used in the current frame");
    Expect(cache.entries.size() == 40, "no entry used this frame is erased");

    cache.Add(99, 99, 64 * MiB);
    cache.entries.splice(cache.entries.end(), cache.entries, cache.entries.begin());
    const auto second = cache.Pass(Limits(0, 0), window);
    Expect(second.evicted == 1 && cache.evicted.back() == 99, "a critical pass evicts only the entry from the previous frame");
}

void EvictsOnlyAgedEntriesUnderSoftPressure() {
    Cache cache;
    for (int i = 0; i < 8; ++i) cache.Add(i, 10 + i * 10, 256 * MiB);
    const ResidencyWindow window{45, 70};
    const auto outcome = cache.Pass(Limits(512 * MiB, 64 * GiB), window);
    Expect(outcome.evicted == 4, "entries last used before the aged bound are evicted");
    for (const auto& entry : cache.entries) Expect(entry.listedUse >= 45, "entries used since the aged bound stay");
    Expect(cache.evicted.size() == 4 && cache.evicted.front() == 0 && cache.evicted.back() == 3, "eviction takes the oldest first");
}

void StopsOnceUnderBudget() {
    Cache cache;
    for (int i = 0; i < 10; ++i) cache.Add(i, i + 1, 100 * MiB);
    const auto outcome = cache.Pass(Limits(750 * MiB, 64 * GiB), {1000, 1000});
    Expect(outcome.evicted == 3, "the pass stops as soon as usage is under the soft limit");
    Expect(outcome.deviceBytes == 300 * MiB, "the outcome counts the evicted bytes");
    Expect(cache.usage.device == 700 * MiB, "usage drops by the evicted bytes");
}

void BoundsDeletionsPerPass() {
    Cache cache;
    for (int i = 0; i < 200; ++i) cache.Add(i, i + 1, 1 * MiB);
    ResidencyPolicy policy;
    const auto over = cache.Pass(Limits(0, 64 * GiB), {100000, 100000}, policy);
    Expect(over.evicted == policy.evictionsPerPass, "a soft pass deletes at most evictionsPerPass entries");
    const auto critical = cache.Pass(Limits(0, 0), {100000, 100000}, policy);
    Expect(critical.evicted == policy.criticalEvictionsPerPass, "a critical pass deletes at most criticalEvictionsPerPass entries");
    policy.scansPerPass = 3;
    for (auto& entry : cache.entries) entry.device = 0;
    const auto scans = cache.Pass(Limits(0, 64 * GiB), {100000, 100000}, policy);
    Expect(scans.evicted == 3, "a pass scans at most scansPerPass entries");
}

void RescuesEntriesTheirObjectsStillUse() {
    Cache cache;
    for (int i = 0; i < 4; ++i) cache.Add(i, 10, 256 * MiB);
    cache.entries.back().objectUse = 500;
    const auto outcome = cache.Pass(Limits(0, 64 * GiB), {100, 400});
    Expect(outcome.rescued == 1, "an entry whose object was used recently is rescued");
    Expect(cache.entries.size() == 1 && cache.entries.front().id == 0, "the rescued entry stays and moves to the front");
    Expect(cache.entries.front().listedUse == 500, "the rescued entry takes its object's stamp");
}

void EvictsAgedEntriesThatHoldNothing() {
    Cache cache;
    cache.Add(1, 1, 0, 0);
    cache.Add(2, 2, 64 * MiB, 64 * MiB);
    cache.Add(3, 200, 64 * MiB, 64 * MiB);
    const auto outcome = cache.Pass(Limits(64 * GiB, 64 * GiB, 32 * MiB, 64 * GiB), {100, 100});
    Expect(outcome.evicted == 2 && cache.evicted.front() == 1 && cache.evicted.back() == 2, "an aged view at the back is evicted rather than blocking the snapshots behind it");
    Expect(cache.entries.size() == 1 && cache.entries.front().id == 3, "the recently used snapshot stays although host bytes are still over");
}

void StrictLruEvictsRegardlessOfAge() {
    Cache cache;
    for (int i = 0; i < 6; ++i) cache.Add(i, 100, 512 * MiB);
    ResidencyPolicy policy;
    policy.strictLru = true;
    const auto outcome = cache.Pass(Limits(1 * GiB, ~0ull), {0, 0}, policy);
    Expect(outcome.evicted == 4, "strict LRU evicts entries used this frame down to the budget");
}

struct Simulation {
    std::list<Entry> entries;
    std::unordered_map<int, std::list<Entry>::iterator> index;
    ResidencyUsage usage;
    std::uint64_t created = 0;

    std::uint64_t maintainedFrame = ~0ull;

    void Evict(ResidencyUsage& pending, const ResidencyLimits& limits, const ResidencyPolicy& policy) {
        RunEvictionPass(
                entries, pending, limits, ResidencyClock::Window(16, 2), policy,
                [](const Entry& entry) { return ResidencyEntryState{entry.listedUse, entry.listedUse, entry.device, entry.host}; },
                [](std::list<Entry>::iterator) {},
                [&](std::list<Entry>::iterator it) {
                    usage.device -= it->device;
                    index.erase(it->id);
                    entries.erase(it);
                });
    }

    void Use(int id, std::uint64_t bytes, const ResidencyLimits& limits, const ResidencyPolicy& policy) {
        const auto now = ResidencyClock::Now();
        if (ResidencyClock::Frame() != maintainedFrame) {
            maintainedFrame = ResidencyClock::Frame();
            auto current = usage;
            Evict(current, limits, policy);
        }
        if (const auto found = index.find(id); found != index.end()) {
            found->second->listedUse = now;
            entries.splice(entries.begin(), entries, found->second);
            return;
        }
        ++created;
        auto pending = usage;
        pending.device += bytes;
        Evict(pending, limits, policy);
        entries.push_front({id, now, now, bytes, 0});
        index[id] = entries.begin();
        usage.device += bytes;
    }
};

std::uint64_t RecreationsOverCycles(const ResidencyPolicy& policy) {
    Simulation simulation;
    const auto limits = Limits(2 * GiB, 64 * GiB);
    constexpr int textures = 120;
    constexpr std::uint64_t bytes = 24 * MiB;
    constexpr int submissionsPerFrame = 4;
    std::uint64_t warm = 0;
    for (int frame = 0; frame < 30; ++frame) {
        for (int submission = 0; submission < submissionsPerFrame; ++submission) {
            for (int id = submission; id < textures; id += submissionsPerFrame) simulation.Use(id, bytes, limits, policy);
            ResidencyClock::NoteSubmission();
        }
        ResidencyClock::NoteFrame();
        if (frame == 4) warm = simulation.created;
    }
    return simulation.created - warm;
}

void CyclicWorkingSetAboveBudgetDoesNotThrash() {
    const auto aged = RecreationsOverCycles({});
    Expect(aged == 0, "a per-frame working set larger than the soft budget is never recreated");
    ResidencyPolicy strict;
    strict.strictLru = true;
    const auto lru = RecreationsOverCycles(strict);
    Expect(lru == 25ull * 120ull, "strict LRU recreates every texture every frame (the old thrash)");
}

void AgedWorkingSetIsTrimmedAfterAScene() {
    Simulation simulation;
    const auto limits = Limits(1 * GiB, 64 * GiB);
    for (int frame = 0; frame < 6; ++frame) {
        for (int id = 0; id < 64; ++id) simulation.Use(id, 32 * MiB, limits, {});
        for (int s = 0; s < 20; ++s) ResidencyClock::NoteSubmission();
        ResidencyClock::NoteFrame();
    }
    Expect(simulation.usage.device == 2 * GiB, "the first scene's set stays resident while in use, above the soft limit");
    for (int frame = 0; frame < 10; ++frame) {
        for (int id = 1000; id < 1016; ++id) simulation.Use(id, 32 * MiB, limits, {});
        for (int s = 0; s < 20; ++s) ResidencyClock::NoteSubmission();
        ResidencyClock::NoteFrame();
    }
    Expect(simulation.usage.device <= 1 * GiB, "once the old scene ages out, usage returns under the soft limit");
}

void ClockWindowFollowsFrames() {
    for (int i = 0; i < 3; ++i) ResidencyClock::NoteFrame();
    const auto before = ResidencyClock::Now();
    ResidencyClock::NoteFrame();
    const auto previousStart = ResidencyClock::Now();
    for (int i = 0; i < 40; ++i) ResidencyClock::NoteSubmission();
    ResidencyClock::NoteFrame();
    const auto currentStart = ResidencyClock::Now();
    const auto window = ResidencyClock::Window(16, 2);
    Expect(window.frameStart == currentStart, "the critical bound is the current frame's start");
    Expect(window.agedBefore == previousStart, "two idle frames put the aged bound at the previous frame's start");
    Expect(before < window.agedBefore, "an entry used before the previous frame is aged");
    const auto ticks = ResidencyClock::Window(20, 1);
    Expect(ticks.agedBefore == currentStart - 20 + 1, "an idle submission count stricter than the frames sets the bound");
}

void LimitsFollowTheDriverBudget() {
    const DeviceMemoryBudget budget{23 * GiB, 10 * GiB, true};
    const auto headroom = 23 * GiB / 16;
    Expect(DeviceHardLimit(budget, 24 * GiB, 4 * GiB) == 4 * GiB + 23 * GiB - 10 * GiB - headroom, "the hard limit is the cache plus the driver's free budget minus headroom");
    Expect(DeviceHardLimit({23 * GiB, 23 * GiB, true}, 24 * GiB, 4 * GiB) == 4 * GiB - headroom, "an exhausted budget pulls the hard limit under the cache");
    Expect(DeviceHardLimit({23 * GiB, 30 * GiB, true}, 24 * GiB, 1 * GiB) == 0, "the hard limit never underflows");
    Expect(DeviceHardLimit({}, 24 * GiB, 4 * GiB) == 12 * GiB, "without a reported budget the hard limit is half the heap");
    Expect(DeviceSoftLimit(budget, 24 * GiB, 3, 8, std::nullopt) == 23 * GiB / 8 * 3, "the soft limit is a share of the driver budget");
    Expect(DeviceSoftLimit({}, 24 * GiB, 1, 4, std::nullopt) == 6 * GiB, "without a reported budget the soft limit is a share of the heap");
    Expect(DeviceSoftLimit(budget, 24 * GiB, 3, 8, 2048 * MiB) == 2048 * MiB, "an override replaces the soft limit");
    Expect(HostSoftLimit(62 * GiB, std::nullopt) == 6 * GiB, "host copies are capped at 6 GiB");
    Expect(HostSoftLimit(16 * GiB, std::nullopt) == 16 * GiB / 10, "host copies take a tenth of a small machine's memory");
    Expect(HostSoftLimit(62 * GiB, 1024 * MiB) == 1024 * MiB, "an override replaces the host limit");
    Expect(MakeResidencyWindow(100, 90, 95, 16).agedBefore == 85, "the aged bound takes the stricter of ticks and frames");
    Expect(MakeResidencyWindow(100, 50, 95, 16).agedBefore == 50, "the aged bound takes the stricter of ticks and frames");
    Expect(PressureOf({3, 0}, Limits(2, 4)) == ResidencyPressure::Over, "over the soft limit is pressure");
    Expect(PressureOf({5, 0}, Limits(2, 4)) == ResidencyPressure::Critical, "over the hard limit is critical");
    Expect(PressureOf({1, 9}, Limits(2, 4, 8, 16)) == ResidencyPressure::Over, "host bytes over their soft limit are pressure");
}

}

void ChargedBytes() {
    Expect(ChargedTextureBytes(64 * MiB, 16 * MiB) == 16 * MiB, "a texture is charged the bytes its image holds, not the guest surface's size");
    Expect(ChargedTextureBytes(64 * MiB, 80 * MiB) == 80 * MiB, "a texture larger on the device than in the guest is charged its allocation");
    Expect(ChargedTextureBytes(64 * MiB, 0) == 64 * MiB, "a texture that does not know its allocation is charged the guest size");
}

void TopMipCap() {
    Expect(!ParseTopMipSkip(nullptr).has_value() && !ParseTopMipSkip("").has_value(), "the cap is off when the setting is unset or empty");
    Expect(!ParseTopMipSkip("0").has_value() && !ParseTopMipSkip("abc").has_value() && !ParseTopMipSkip("4x").has_value() && !ParseTopMipSkip("-4").has_value(), "zero and malformed settings leave the cap off");
    Expect(ParseTopMipSkip("4") == 4 * MiB, "the setting is a threshold in MiB");
    Expect(!ParseTopMipSkip("99999999999999999999").has_value(), "an overflowing setting leaves the cap off");
    const TopMipFacts big{false, true, 12, 11, 8 * MiB};
    Expect(TopMipsToSkip(big, std::nullopt) == 0u, "nothing is skipped while the cap is off");
    Expect(TopMipsToSkip(big, 4 * MiB) == 1u, "a texture larger than the threshold loses its top mip");
    Expect(TopMipsToSkip(big, 8 * MiB) == 0u, "a texture exactly at the threshold keeps it");
    TopMipFacts small = big; small.guestBytes = 2 * MiB;
    Expect(TopMipsToSkip(small, 4 * MiB) == 0u, "a texture under the threshold keeps its top mip");
    TopMipFacts depth = big; depth.depthCompare = true;
    Expect(TopMipsToSkip(depth, 4 * MiB) == 0u, "a depth comparison texture is not reduced");
    TopMipFacts volume = big; volume.twoDimensional = false;
    Expect(TopMipsToSkip(volume, 4 * MiB) == 0u, "3D and cube textures are not reduced");
    TopMipFacts single = big; single.mipCount = 1;
    Expect(TopMipsToSkip(single, 4 * MiB) == 0u, "a texture with one level has nothing to drop");
    TopMipFacts topOnly = big; topOnly.lastLevel = 0;
    Expect(TopMipsToSkip(topOnly, 4 * MiB) == 0u, "a view that reads only the top level keeps it");
}

int main() {
    TopMipCap();
    ChargedBytes();
    NeverEvictsEntriesUsedThisFrame();
    EvictsOnlyAgedEntriesUnderSoftPressure();
    StopsOnceUnderBudget();
    BoundsDeletionsPerPass();
    RescuesEntriesTheirObjectsStillUse();
    EvictsAgedEntriesThatHoldNothing();
    StrictLruEvictsRegardlessOfAge();
    CyclicWorkingSetAboveBudgetDoesNotThrash();
    AgedWorkingSetIsTrimmedAfterAScene();
    ClockWindowFollowsFrames();
    LimitsFollowTheDriverBudget();
    if (failures != 0) return 1;
    std::puts("texture residency tests passed");
    return 0;
}
