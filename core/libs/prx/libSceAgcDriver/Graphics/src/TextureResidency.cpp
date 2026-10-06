#include "prx/libSceAgcDriver/Graphics/include/TextureResidency.hpp"
#include <cerrno>
#include <cstdlib>
#include <limits>
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver::Graphics {
namespace {

constexpr std::size_t FrameHistory = 8;
constexpr std::uint64_t MiB = 1ull << 20u;

struct ClockState {
    std::atomic<std::uint64_t> tick{1};
    std::atomic<std::uint64_t> frame{0};
    std::array<std::atomic<std::uint64_t>, FrameHistory> frameStarts{};
};

ClockState& Clock() {
    static ClockState state;
    return state;
}

}

void ResidencyClock::NoteSubmission() {
    Clock().tick.fetch_add(1, std::memory_order_relaxed);
}

void ResidencyClock::NoteFrame() {
    auto& clock = Clock();
    const auto start = clock.tick.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto frame = clock.frame.load(std::memory_order_relaxed) + 1;
    clock.frameStarts[frame % FrameHistory].store(start, std::memory_order_relaxed);
    clock.frame.store(frame, std::memory_order_release);
}

std::uint64_t ResidencyClock::Now() {
    return Clock().tick.load(std::memory_order_relaxed);
}

std::uint64_t ResidencyClock::Frame() {
    return Clock().frame.load(std::memory_order_acquire);
}

ResidencyWindow ResidencyClock::Window(std::uint64_t minIdleTicks, std::uint32_t minIdleFrames) {
    auto& clock = Clock();
    const auto frame = clock.frame.load(std::memory_order_acquire);
    const auto now = clock.tick.load(std::memory_order_relaxed);
    if (frame == 0) return MakeResidencyWindow(now, std::numeric_limits<std::uint64_t>::max(), now >= minIdleTicks ? now - minIdleTicks + 1 : 0, minIdleTicks);
    const auto startOf = [&](std::uint64_t index) { return index > frame || frame - index >= FrameHistory ? 0 : clock.frameStarts[index % FrameHistory].load(std::memory_order_relaxed); };
    const auto back = std::min<std::uint64_t>(std::max<std::uint32_t>(minIdleFrames, 1u) - 1u, FrameHistory - 1u);
    const auto currentStart = startOf(frame);
    const auto idleStart = back > frame ? 0 : startOf(frame - back);
    return MakeResidencyWindow(now, idleStart, currentStart, minIdleTicks);
}

ResidencyPressure PressureOf(const ResidencyUsage& usage, const ResidencyLimits& limits) {
    if (usage.device > limits.deviceHard || usage.host > limits.hostHard) return ResidencyPressure::Critical;
    if (usage.device > limits.deviceSoft || usage.host > limits.hostSoft) return ResidencyPressure::Over;
    return ResidencyPressure::None;
}

ResidencyWindow MakeResidencyWindow(std::uint64_t now, std::uint64_t idleFrameStart, std::uint64_t currentFrameStart, std::uint64_t minIdleTicks) {
    const auto agedByTicks = now >= minIdleTicks ? now - minIdleTicks + 1 : 0;
    return {std::min(agedByTicks, idleFrameStart), currentFrameStart};
}

std::uint64_t DeviceHardLimit(const DeviceMemoryBudget& budget, std::uint64_t heapBytes, std::uint64_t cacheDeviceBytes) {
    if (!budget.reported || budget.budget == 0) return heapBytes / 2;
    const auto headroom = std::max<std::uint64_t>(512 * MiB, budget.budget / 16);
    const auto ceiling = cacheDeviceBytes + budget.budget;
    const auto taken = budget.usage + headroom;
    return ceiling > taken ? ceiling - taken : 0;
}

std::optional<std::uint64_t> ParseTopMipSkip(const char* text) {
    if (text == nullptr || text[0] == '\0') return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const auto mib = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0 || mib == 0 || mib > (std::numeric_limits<std::uint64_t>::max() >> 20u)) return std::nullopt;
    return mib << 20u;
}

std::optional<std::uint64_t> TopMipSkipBytes() {
    static const std::optional<std::uint64_t> bytes = ParseTopMipSkip(std::getenv("APS5_TEXTURE_SKIP_TOP_MIP_MIB"));
    return bytes;
}

std::uint32_t TopMipsToSkip(const TopMipFacts& facts, std::optional<std::uint64_t> thresholdBytes) {
    if (!thresholdBytes.has_value() || facts.depthCompare || !facts.twoDimensional) return 0u;
    if (facts.mipCount < 2u || facts.lastLevel < 1u) return 0u;
    return facts.guestBytes > *thresholdBytes ? 1u : 0u;
}

std::uint64_t ChargedTextureBytes(std::uint64_t guestBytes, std::uint64_t allocationBytes) {
    return allocationBytes != 0 ? allocationBytes : guestBytes;
}

std::uint64_t DeviceSoftLimit(const DeviceMemoryBudget& budget, std::uint64_t heapBytes, std::uint64_t numerator, std::uint64_t denominator, std::optional<std::uint64_t> overrideBytes) {
    if (overrideBytes.has_value()) return *overrideBytes;
    const auto base = budget.reported && budget.budget != 0 ? budget.budget : heapBytes;
    return denominator == 0 ? base : base / denominator * numerator;
}

std::uint64_t HostSoftLimit(std::uint64_t physicalBytes, std::optional<std::uint64_t> overrideBytes) {
    if (overrideBytes.has_value()) return *overrideBytes;
    if (physicalBytes == 0) return 2048 * MiB;
    return std::min<std::uint64_t>(6144 * MiB, physicalBytes / 10);
}

std::uint64_t DeviceLocalHeapBytes(const Context& context) {
    std::uint64_t largest = 0;
    for (std::uint32_t i = 0; i < context.memory.memoryHeapCount; ++i) {
        if ((context.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) largest = std::max<std::uint64_t>(largest, context.memory.memoryHeaps[i].size);
    }
    return largest;
}

DeviceMemoryBudget QueryDeviceMemoryBudget(const Context& context) {
    if (!context.memoryBudget || context.memoryProperties2 == nullptr) return {};
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budgets};
    context.memoryProperties2(context.physical, &properties);
    std::optional<std::uint32_t> heap;
    for (std::uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; ++i) {
        const auto& candidate = properties.memoryProperties.memoryHeaps[i];
        if ((candidate.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        if (!heap.has_value() || candidate.size > properties.memoryProperties.memoryHeaps[*heap].size) heap = i;
    }
    if (!heap.has_value() || budgets.heapBudget[*heap] == 0) return {};
    return {budgets.heapBudget[*heap], budgets.heapUsage[*heap], true};
}

std::uint64_t PhysicalMemoryBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<std::uint64_t>(status.ullTotalPhys) : 0;
#else
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto pageBytes = sysconf(_SC_PAGESIZE);
    return pages > 0 && pageBytes > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageBytes) : 0;
#endif
}

std::optional<std::uint64_t> MebibytesFromEnvironment(const char* name) {
    const char* text = std::getenv(name);
    if (text == nullptr) return std::nullopt;
    const auto mebibytes = std::strtoull(text, nullptr, 10);
    if (mebibytes == 0) return std::nullopt;
    return mebibytes * MiB;
}

}
