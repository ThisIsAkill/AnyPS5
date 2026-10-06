#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURERESIDENCY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURERESIDENCY_HPP

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

namespace AgcDriver::Graphics {

struct Context;

struct ResidencyUsage {
    std::uint64_t device = 0;
    std::uint64_t host = 0;
};

struct ResidencyLimits {
    std::uint64_t deviceSoft = 0;
    std::uint64_t deviceHard = 0;
    std::uint64_t hostSoft = 0;
    std::uint64_t hostHard = 0;
};

enum class ResidencyPressure : std::uint8_t { None, Over, Critical };

struct ResidencyWindow {
    std::uint64_t agedBefore = 0;
    std::uint64_t frameStart = 0;
};

struct ResidencyPolicy {
    std::size_t evictionsPerPass = 16;
    std::size_t criticalEvictionsPerPass = 64;
    std::size_t scansPerPass = 256;
    bool strictLru = false;
};

struct ResidencyEntryState {
    std::uint64_t listedUse = 0;
    std::uint64_t lastUse = 0;
    std::uint64_t device = 0;
    std::uint64_t host = 0;
};

struct EvictionOutcome {
    std::size_t evicted = 0;
    std::size_t rescued = 0;
    std::uint64_t deviceBytes = 0;
    std::uint64_t hostBytes = 0;
};

struct DeviceMemoryBudget {
    std::uint64_t budget = 0;
    std::uint64_t usage = 0;
    bool reported = false;
};

class ResidencyClock {
public:
    static void NoteSubmission();
    static void NoteFrame();
    static std::uint64_t Now();
    static std::uint64_t Frame();
    static ResidencyWindow Window(std::uint64_t minIdleTicks, std::uint32_t minIdleFrames);
};

ResidencyPressure PressureOf(const ResidencyUsage& usage, const ResidencyLimits& limits);
ResidencyWindow MakeResidencyWindow(std::uint64_t now, std::uint64_t idleFrameStart, std::uint64_t currentFrameStart, std::uint64_t minIdleTicks);
std::uint64_t DeviceHardLimit(const DeviceMemoryBudget& budget, std::uint64_t heapBytes, std::uint64_t cacheDeviceBytes);
std::uint64_t DeviceSoftLimit(const DeviceMemoryBudget& budget, std::uint64_t heapBytes, std::uint64_t numerator, std::uint64_t denominator, std::optional<std::uint64_t> overrideBytes);
struct TopMipFacts {
    bool depthCompare = false;
    bool twoDimensional = false;
    std::uint32_t mipCount = 0;
    std::uint32_t lastLevel = 0;
    std::uint64_t guestBytes = 0;
};
std::optional<std::uint64_t> ParseTopMipSkip(const char* text);
std::optional<std::uint64_t> TopMipSkipBytes();
std::uint32_t TopMipsToSkip(const TopMipFacts& facts, std::optional<std::uint64_t> thresholdBytes);

std::uint64_t ChargedTextureBytes(std::uint64_t guestBytes, std::uint64_t allocationBytes);
std::uint64_t HostSoftLimit(std::uint64_t physicalBytes, std::optional<std::uint64_t> overrideBytes);
DeviceMemoryBudget QueryDeviceMemoryBudget(const Context& context);
std::uint64_t DeviceLocalHeapBytes(const Context& context);
std::uint64_t PhysicalMemoryBytes();
std::optional<std::uint64_t> MebibytesFromEnvironment(const char* name);

template<typename List, typename Describe, typename Rescue, typename Evict>
EvictionOutcome RunEvictionPass(List& entries, ResidencyUsage& usage, const ResidencyLimits& limits, const ResidencyWindow& window, const ResidencyPolicy& policy, Describe describe, Rescue rescue, Evict evict) {
    EvictionOutcome outcome;
    std::size_t scanned = 0;
    auto cursor = entries.end();
    while (cursor != entries.begin()) {
        const auto pressure = PressureOf(usage, limits);
        if (pressure == ResidencyPressure::None) break;
        const bool critical = pressure == ResidencyPressure::Critical;
        if (!policy.strictLru && (outcome.evicted >= (critical ? policy.criticalEvictionsPerPass : policy.evictionsPerPass) || scanned >= policy.scansPerPass)) break;
        const auto victim = std::prev(cursor);
        ++scanned;
        const auto state = describe(*victim);
        if (!policy.strictLru) {
            const auto bound = critical ? window.frameStart : window.agedBefore;
            if (state.listedUse >= bound) break;
            if (state.lastUse >= bound) {
                rescue(victim);
                ++outcome.rescued;
                continue;
            }
        }
        usage.device -= state.device < usage.device ? state.device : usage.device;
        usage.host -= state.host < usage.host ? state.host : usage.host;
        outcome.deviceBytes += state.device;
        outcome.hostBytes += state.host;
        ++outcome.evicted;
        evict(victim);
    }
    return outcome;
}

}

#endif
