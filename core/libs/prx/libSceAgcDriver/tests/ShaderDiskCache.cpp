#include "ShaderDiskCache.hpp"
#include "ShaderCacheDirectory.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unsetEnvironment(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

bool sameBinding(const DescriptorBinding& left, const DescriptorBinding& right) {
    return left.kind == right.kind && left.role == right.role && left.descriptorSet == right.descriptorSet && left.binding == right.binding && left.count == right.count && left.guestDescriptor == right.guestDescriptor && left.readOnly == right.readOnly && left.imageShape == right.imageShape && left.samplerDepthCompare == right.samplerDepthCompare && left.imageWritten == right.imageWritten && left.imageDepthCompare == right.imageDepthCompare && left.bufferAtomic == right.bufferAtomic && left.bufferWritten == right.bufferWritten && left.bufferRead == right.bufferRead;
}

bool sameBindings(const std::vector<DescriptorBinding>& left, const std::vector<DescriptorBinding>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!sameBinding(left[i], right[i])) return false;
    }
    return true;
}

void requireSameResult(const RecompileResult& left, const RecompileResult& right, const char* what) {
    const std::string prefix = std::string(what) + ": ";
    require(left.spirv.Words() == right.spirv.Words(), prefix + "SPIR-V differs");
    require(sameBindings(left.bindings, right.bindings), prefix + "bindings differ");
    require(left.pushConstants == right.pushConstants, prefix + "push constants differ");
    require(left.bdaAbiVersion == right.bdaAbiVersion, prefix + "BDA ABI version differs");
    require(left.vertexAttributes.size() == right.vertexAttributes.size(), prefix + "vertex attribute count differs");
    for (std::size_t i = 0; i < left.vertexAttributes.size(); ++i) {
        const auto& a = left.vertexAttributes[i];
        const auto& b = right.vertexAttributes[i];
        require(a.location == b.location && a.components == b.components && a.resource.fields == b.resource.fields && a.fetchIndex == b.fetchIndex, prefix + "vertex attribute differs");
    }
    require(left.vertexOffsetSgpr == right.vertexOffsetSgpr && left.instanceOffsetSgpr == right.instanceOffsetSgpr, prefix + "offset SGPRs differ");
    require(left.vertexOffsetShared == right.vertexOffsetShared && left.instanceOffsetShared == right.instanceOffsetShared && left.vertexOffsetConflict == right.vertexOffsetConflict && left.instanceOffsetConflict == right.instanceOffsetConflict, prefix + "offset flags differ");
    require(left.parameterExports == right.parameterExports, prefix + "parameter exports differ");
    require(left.fragmentParameters.size() == right.fragmentParameters.size(), prefix + "fragment parameter count differs");
    for (std::size_t i = 0; i < left.fragmentParameters.size(); ++i) {
        const auto& a = left.fragmentParameters[i];
        const auto& b = right.fragmentParameters[i];
        require(a.location == b.location && a.sourceLocation == b.sourceLocation && a.flat == b.flat && a.perVertex == b.perVertex, prefix + "fragment parameter differs");
    }
}

void requireSameVariant(const CompiledVariant& left, const CompiledVariant& right, const char* what) {
    requireSameResult(left.result, right.result, what);
    const std::string prefix = std::string(what) + ": ";
    const auto& a = left.info;
    const auto& b = right.info;
    require(a.stage == b.stage && a.shaderHash == b.shaderHash && a.waveSize == b.waveSize && a.userDataBase == b.userDataBase && a.userDataCount == b.userDataCount && a.scratchDwords == b.scratchDwords && a.paramExportMask == b.paramExportMask, prefix + "compiled info differs");
    require(a.info == b.info, prefix + "shader info differs");
    require(a.bindings == b.bindings, prefix + "info binding layout differs");
    require(sameBindings(left.bindings.bindings, right.bindings.bindings), prefix + "allocation bindings differ");
    require(left.bindings.layout == right.bindings.layout, prefix + "allocation layout differs");
    require(left.bindings.pushConstantOffsetBytes == right.bindings.pushConstantOffsetBytes && left.bindings.pushConstantSizeBytes == right.bindings.pushConstantSizeBytes, prefix + "push constant range differs");
    require(left.bindings.pushConstants == right.bindings.pushConstants, prefix + "allocation push constants differ");
}

DescriptorBinding sampleBinding(std::uint32_t seed) {
    DescriptorBinding binding{};
    binding.kind = DescriptorKind::StorageImage;
    binding.role = DescriptorRole::GuestImages;
    binding.descriptorSet = 0;
    binding.binding = 29 + seed;
    binding.count = 3;
    binding.guestDescriptor = {0x11111111u * seed, 0xdeadbeefu, 0x80000000u, 7u, 0u, 0xffffffffu, 42u, seed};
    binding.readOnly = seed % 2 == 0;
    binding.imageShape = DescriptorImageShape::Image2DArray;
    binding.samplerDepthCompare = {true, false, true};
    binding.imageDepthCompare = {false, true, false};
    binding.imageWritten = {false, true, true};
    binding.bufferAtomic = {true};
    binding.bufferWritten = {false, false, true, true, false};
    binding.bufferRead = {true, false, false, true, true, false};
    return binding;
}

RecompileResult sampleResult() {
    RecompileResult result;
    std::vector<std::uint32_t> words(1000);
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = static_cast<std::uint32_t>(i * 2654435761u);
    words[0] = 0x07230203u;
    result.spirv = std::move(words);
    result.bindings = {sampleBinding(1), sampleBinding(2)};
    result.bindings[1].kind = DescriptorKind::StorageBuffer;
    result.bindings[1].role = DescriptorRole::GuestBuffers;
    result.bindings[1].imageShape.reset();
    result.pushConstants = {std::byte{1}, std::byte{0xff}, std::byte{0}, std::byte{0x80}, std::byte{7}};
    result.bdaAbiVersion = 3;
    result.vertexAttributes = {{1, 4, {{0x1000u, 0x20000u, 0x30u, 0x4u}}, 2}, {5, 2, {{9u, 8u, 7u, 6u}}, 0}};
    result.vertexOffsetSgpr = 12;
    result.instanceOffsetSgpr = -1;
    result.vertexOffsetShared = true;
    result.instanceOffsetShared = false;
    result.vertexOffsetConflict = false;
    result.instanceOffsetConflict = true;
    result.parameterExports = {0, 3, 7};
    result.fragmentParameters = {{0, 1, true, false}, {2, 3, false, true}};
    result.variantId = 99;
    return result;
}

CompiledVariant sampleVariant() {
    CompiledVariant variant;
    variant.result = sampleResult();
    variant.result.bindings.clear();
    variant.result.pushConstants.clear();
    variant.layout = {0, 0, 0, 128};
    auto& info = variant.info;
    info.stage = IrShaderStage::Compute;
    info.shaderHash = 0x123456789abcdefull;
    info.waveSize = 32;
    info.userDataBase = 4;
    info.userDataCount = 16;
    info.scratchDwords = 8;
    info.paramExportMask = 0x5;
    info.info.scratchDwords = 8;
    info.info.sharedMemoryBytes = 4096;
    BufferResource buffer{};
    buffer.source = 3;
    buffer.firstUsePc = 0x40;
    buffer.maxByteExtent = 256;
    buffer.packedStride = 16;
    buffer.descriptorSwizzle = 0x123u;
    buffer.imageAlias = 1;
    buffer.read = true;
    buffer.atomic = true;
    buffer.scalar = true;
    info.info.buffers = {buffer, BufferResource{}};
    ImageResource image{};
    image.source = 5;
    image.firstUsePc = 0x80;
    image.resourceClass = ImageResourceClass::Storage;
    image.numericClass = IrTextureNumericClass::Uint;
    image.mipMode = ImageMipMode::DynamicStorage;
    image.mipCount = 4;
    image.shaderSwizzle = 0x321u;
    image.written = true;
    image.cube = true;
    image.r128 = true;
    image.indirectRoot = 0;
    image.indirectMappingOffset = 12;
    image.indirectSearchIterations = 3;
    image.indirectResources = {1, 2, 3};
    info.info.images = {image};
    info.info.samplers = {{7, 0x10, true, false}};
    info.info.sampledPairs = {{0, 0, 0x10}};
    StageInput input{};
    input.kind = StageInputKind::GlobalInvocationId;
    input.location = 2;
    input.componentCount = 3;
    input.debugName = "gid";
    input.perVertex = true;
    info.info.inputs = {input};
    StageOutput output{};
    output.kind = StageOutputKind::Mrt;
    output.index = 1;
    output.location = 4;
    output.debugName = "mrt1";
    info.info.outputs = {output};
    info.info.vertexFetchComponents[3] = 4;
    info.info.vertexOffsetSgpr = 6;
    info.info.hasBitwiseXor = true;
    info.info.usesDma = true;
    info.bindings.pushDataStartDword = 2;
    info.bindings.memoryOffsetDword = 1;
    info.bindings.memoryOffsetCount = 5;
    info.bindings.userDataRegisters = {4, 5, 9};
    info.bindings.descriptors = {{DescriptorBindingKind::Buffers, {0, 1}}, {DescriptorBindingKind::FlattenedSrt, {}}};
    variant.bindings.layout = info.bindings;
    variant.bindings.pushConstantOffsetBytes = 16;
    variant.bindings.pushConstantSizeBytes = 112;
    variant.bindings.bindings = {sampleBinding(3)};
    variant.bindings.pushConstants = {std::byte{9}, std::byte{8}};
    return variant;
}

struct SampleRequest {
    std::vector<std::uint32_t> code{0xe0700000u, 0x80000000u, 0xbf810000u, 0x12345678u};
    std::vector<std::uint32_t> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu};
    std::array<std::uint32_t, 2> capabilities{1u, 61u};
    std::array<std::string_view, 1> extensions{"SPV_KHR_storage_buffer_storage_class"};
    std::array<std::byte, 16> header{};
    RecompileRequest request{};
    ResourceSpecialization specialization;
    ShaderDiskCache::SettledLayout settled;
    std::uint32_t hostSubgroupSize = 32;

    SampleRequest() {
        request.shader = {ShaderStage::Compute, 0x20000u, code, 0x1f000u, header};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00403000u;
        request.target.spirvVersion = 0x00010600u;
        request.target.subgroupSize = 32;
        request.target.bdaAbiVersion = 1;
        request.target.supportedCapabilities = capabilities;
        request.target.supportedExtensions = extensions;
        request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
        request.target.maxWorkgroupInvocations = 1024;
        request.target.maxWorkgroupSharedMemoryBytes = 49152;
        request.layout = {0, 0, 0, 128};
        specialization.buffers = {{16u, IrBufferFormat::Invalid, 0xfacu}};
        specialization.images = {ResourceSpecialization::Image{}};
        specialization.boundDescriptors = {0, 0};
    }

    std::vector<std::byte> Key() {
        request.shader.code = code;
        request.context.userData = userData;
        request.target.supportedCapabilities = capabilities;
        std::vector<std::byte> key;
        ShaderDiskCache::BuildKey(request, hostSubgroupSize, specialization, settled, key);
        return key;
    }
};

void verifyResultRoundTrip() {
    const auto result = sampleResult();
    std::vector<std::byte> bytes;
    ShaderDiskCache::EncodeResult(result, bytes);
    RecompileResult decoded;
    require(ShaderDiskCache::DecodeResult(bytes, decoded), "an encoded result does not decode");
    requireSameResult(result, decoded, "result round trip");
    require(decoded.variantId == 0 && !decoded.cacheHit, "the per-process fields were stored");
    for (std::size_t size = 0; size < bytes.size(); size += size < 256 ? 1 : 97) {
        RecompileResult partial;
        require(!ShaderDiskCache::DecodeResult(std::span(bytes).first(size), partial), "a result truncated to " + std::to_string(size) + " bytes decodes");
    }
    auto longer = bytes;
    longer.push_back(std::byte{0});
    require(!ShaderDiskCache::DecodeResult(longer, decoded), "a result with a trailing byte decodes");
    bytes.clear();
    ShaderDiskCache::EncodeResult(RecompileResult{}, bytes);
    require(ShaderDiskCache::DecodeResult(bytes, decoded), "an empty result does not decode");
    requireSameResult(RecompileResult{}, decoded, "empty result round trip");
}

void verifyEntryRoundTrip() {
    SampleRequest sample;
    const auto key = sample.Key();
    const auto variant = sampleVariant();
    const auto file = ShaderDiskCache::EncodeEntry(key, variant);
    CompiledVariant decoded;
    require(ShaderDiskCache::DecodeEntry(file, key, decoded) == ShaderDiskCache::LoadStatus::Loaded, "an encoded entry does not decode");
    requireSameVariant(variant, decoded, "entry round trip");

    for (std::size_t size = 0; size < file.size(); size += size < 512 ? 1 : 131) {
        CompiledVariant partial;
        require(ShaderDiskCache::DecodeEntry(std::span(file).first(size), key, partial) == ShaderDiskCache::LoadStatus::Rejected, "an entry truncated to " + std::to_string(size) + " bytes is not rejected");
    }
    for (std::size_t offset = 0; offset < file.size(); offset += offset < 512 ? 1 : 61) {
        auto damaged = file;
        damaged[offset] ^= std::byte{0x10};
        CompiledVariant partial;
        const auto status = ShaderDiskCache::DecodeEntry(damaged, key, partial);
        require(status == ShaderDiskCache::LoadStatus::Rejected, "an entry with byte " + std::to_string(offset) + " damaged is not rejected");
    }
    auto longer = file;
    longer.push_back(std::byte{0});
    require(ShaderDiskCache::DecodeEntry(longer, key, decoded) == ShaderDiskCache::LoadStatus::Rejected, "an entry with a trailing byte is not rejected");
    auto otherKey = key;
    otherKey.back() ^= std::byte{1};
    require(ShaderDiskCache::DecodeEntry(file, otherKey, decoded) == ShaderDiskCache::LoadStatus::KeyMismatch, "an entry for another key loads");
}

void verifyKeySensitivity() {
    SampleRequest base;
    const auto key = base.Key();
    require(base.Key() == key, "the key is not deterministic");
    require(ShaderDiskCache::EntryName(key).size() == 36, "unexpected entry name length");

    const auto changes = [&](const std::string& what, const std::function<void(SampleRequest&)>& change) {
        SampleRequest sample;
        change(sample);
        const auto changed = sample.Key();
        require(changed != key, "the key ignores " + what);
        require(ShaderDiskCache::EntryName(changed) != ShaderDiskCache::EntryName(key), "the entry name ignores " + what);
    };
    for (std::size_t word = 0; word < base.code.size(); ++word) {
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            changes("code word " + std::to_string(word) + " bit " + std::to_string(bit), [&](SampleRequest& sample) { sample.code[word] ^= 1u << bit; });
        }
    }
    changes("a code word appended", [](SampleRequest& sample) { sample.code.push_back(0); });
    changes("the stage", [](SampleRequest& sample) { sample.request.shader.stage = ShaderStage::Fragment; sample.request.context.compute.reset(); });
    changes("the wave size", [](SampleRequest& sample) { sample.request.context.waveSize = 32; });
    changes("the user data base", [](SampleRequest& sample) { sample.request.context.userDataBaseRegister = 2; });
    changes("the user data count", [](SampleRequest& sample) { sample.userData.push_back(0); });
    for (std::size_t axis = 0; axis < 3; ++axis) {
        changes("thread count " + std::to_string(axis), [&](SampleRequest& sample) { sample.request.context.compute->numThreads[axis] += 1; });
        changes("group id enable " + std::to_string(axis), [&](SampleRequest& sample) { sample.request.context.compute->groupIdEnable[axis] = true; });
    }
    changes("the LDS size", [](SampleRequest& sample) { sample.request.context.compute->ldsSizeDwords = 64; });
    changes("the thread group size enable", [](SampleRequest& sample) { sample.request.context.compute->tgSizeEnable = true; });
    changes("the thread id component count", [](SampleRequest& sample) { sample.request.context.compute->threadIdComponentCount = 3; });
    changes("the Vulkan version", [](SampleRequest& sample) { sample.request.target.vulkanVersion = 0x00402000u; });
    changes("the SPIR-V version", [](SampleRequest& sample) { sample.request.target.spirvVersion = 0x00010500u; });
    changes("the target subgroup size", [](SampleRequest& sample) { sample.request.target.subgroupSize = 64; });
    changes("the BDA ABI version", [](SampleRequest& sample) { sample.request.target.bdaAbiVersion = 2; });
    changes("a capability", [](SampleRequest& sample) { sample.capabilities[1] = 62u; });
    changes("an extension", [](SampleRequest& sample) { sample.extensions[0] = "SPV_KHR_storage_buffer_storage_clasS"; });
    changes("barycentrics", [](SampleRequest& sample) { sample.request.target.fragmentShaderBarycentricEnabled = true; });
    changes("non-constant texel offsets", [](SampleRequest& sample) { sample.request.target.nonConstantImageOffsets = true; });
    changes("the workgroup size limit", [](SampleRequest& sample) { sample.request.target.maxWorkgroupSize[2] = 128; });
    changes("the invocation limit", [](SampleRequest& sample) { sample.request.target.maxWorkgroupInvocations = 512; });
    changes("the shared memory limit", [](SampleRequest& sample) { sample.request.target.maxWorkgroupSharedMemoryBytes = 32768; });
    changes("the host subgroup size", [](SampleRequest& sample) { sample.hostSubgroupSize = 64; });
    changes("the descriptor set", [](SampleRequest& sample) { sample.request.layout.descriptorSet = 1; });
    changes("the first binding", [](SampleRequest& sample) { sample.request.layout.firstBinding = 1; });
    changes("the push constant offset", [](SampleRequest& sample) { sample.request.layout.pushConstantOffsetBytes = 16; });
    changes("the push constant size", [](SampleRequest& sample) { sample.request.layout.pushConstantSizeBytes = 64; });
    changes("a buffer stride", [](SampleRequest& sample) { sample.specialization.buffers[0].packedStride = 32; });
    changes("a buffer format", [](SampleRequest& sample) { sample.specialization.buffers[0].descriptorFormat = static_cast<IrBufferFormat>(1); });
    changes("a buffer swizzle", [](SampleRequest& sample) { sample.specialization.buffers[0].descriptorSwizzle = 0xfadu; });
    changes("the buffer count", [](SampleRequest& sample) { sample.specialization.buffers.emplace_back(); });
    changes("an image class", [](SampleRequest& sample) { sample.specialization.images[0].numericClass = IrTextureNumericClass::Float; });
    changes("an image dimension", [](SampleRequest& sample) { sample.specialization.images[0].dimension = static_cast<RdnaImageDimension>(1); });
    changes("an image mip count", [](SampleRequest& sample) { sample.specialization.images[0].mipCount = 2; });
    changes("an image conversion", [](SampleRequest& sample) { sample.specialization.images[0].conversionFormat = static_cast<IrBufferFormat>(1); });
    changes("an image swizzle", [](SampleRequest& sample) { sample.specialization.images[0].shaderSwizzle = 0; });
    changes("an image indirect root", [](SampleRequest& sample) { sample.specialization.images[0].indirectRoot = 0; });
    changes("an image mapping offset", [](SampleRequest& sample) { sample.specialization.images[0].indirectMappingOffset = 4; });
    changes("an image search depth", [](SampleRequest& sample) { sample.specialization.images[0].indirectSearchIterations = 2; });
    changes("an image cube flag", [](SampleRequest& sample) { sample.specialization.images[0].cube = true; });
    changes("an image FMASK flag", [](SampleRequest& sample) { sample.specialization.images[0].fmask = true; });
    changes("the image count", [](SampleRequest& sample) { sample.specialization.images.emplace_back(); });
    const auto fragment = [](SampleRequest& sample) {
        sample.request.shader.stage = ShaderStage::Fragment;
        sample.request.context.compute.reset();
        ShaderPixelStageInfo pixel{};
        pixel.inputAddr = 0x302u;
        pixel.hasPerspectiveCenterVgpr = pixel.posX = pixel.posY = true;
        pixel.targetOutputMode[0] = 9;
        sample.request.context.pixel = pixel;
    };
    SampleRequest pixelBase;
    fragment(pixelBase);
    const auto pixelKey = pixelBase.Key();
    const auto pixelChanges = [&](const std::string& what, const std::function<void(ShaderPixelStageInfo&)>& change) {
        SampleRequest sample;
        fragment(sample);
        change(*sample.request.context.pixel);
        require(sample.Key() != pixelKey, "the key ignores " + what);
    };
    pixelChanges("SPI_PS_INPUT_ADDR", [](ShaderPixelStageInfo& pixel) { pixel.inputAddr |= 0x4u; });
    pixelChanges("PERSP_CENTROID", [](ShaderPixelStageInfo& pixel) { pixel.inputAddr |= 0x4u; pixel.perspectiveCentroid = true; });
    pixelChanges("LINEAR_CENTROID", [](ShaderPixelStageInfo& pixel) { pixel.inputAddr |= 0x40u; pixel.linearCentroid = true; });
    pixelChanges("LINEAR_CENTROID alone", [](ShaderPixelStageInfo& pixel) { pixel.linearCentroid = true; });
    pixelChanges("PERSP_CENTROID alone", [](ShaderPixelStageInfo& pixel) { pixel.perspectiveCentroid = true; });
    changes("the bound descriptors", [](SampleRequest& sample) { sample.specialization.boundDescriptors.push_back(1); });

    SampleRequest moved;
    moved.userData[0] ^= 0x10000u;
    moved.request.shader.codeAddress += 0x1000000u;
    moved.request.shader.headerAddress += 0x1000000u;
    moved.header[3] = std::byte{1};
    require(moved.Key() == key, "the key depends on the user data values or the addresses");
}

std::optional<std::uint32_t> noLocalMemory(void*, std::span<const std::uint32_t>, std::span<const DescriptorBinding>, std::uint64_t) {
    return std::nullopt;
}

void withProbe(SampleRequest& sample) {
    sample.request.target.localMemoryProbe = &noLocalMemory;
    sample.request.target.localMemoryProbeDevice = {0x10deu, 0x2684u, 0x99c00000u, {}};
    sample.request.target.localMemoryProbeDevice.pipelineCacheUuid[0] = 0x5au;
}

void verifyLayoutKey() {
    SampleRequest base;
    const auto key = base.Key();
    const auto keyWith = [](const std::function<void(SampleRequest&)>& change) {
        SampleRequest sample;
        change(sample);
        return sample.Key();
    };
    require(LayoutChosenByProbe(base.request) == false, "a request without a probe has its layout chosen by one");
    require(keyWith([](SampleRequest& sample) { sample.settled.layout = WaveLayout::TwoLane; }) != key, "the key ignores the settled two-lane layout");
    require(keyWith([](SampleRequest& sample) { sample.settled.layout = WaveLayout::SingleLane; }) != key, "the key ignores the settled one-lane layout");
    require(keyWith([](SampleRequest& sample) { sample.settled.layout = WaveLayout::SingleLane; }) != keyWith([](SampleRequest& sample) { sample.settled.layout = WaveLayout::TwoLane; }), "the settled layouts share a key");
    require(keyWith([](SampleRequest& sample) { sample.settled = {WaveLayout::TwoLane, 1024u}; }) != keyWith([](SampleRequest& sample) { sample.settled.layout = WaveLayout::TwoLane; }), "the key ignores the settled workgroup reserve");

    SampleRequest probed;
    withProbe(probed);
    require(LayoutChosenByProbe(probed.request), "a split wave64 Auto program with a probe is not chosen by it");
    const auto probedKey = probed.Key();
    require(probedKey != key, "the key does not tell a probed layout choice from an unprobed one");
    const auto device = [&](const std::string& what, const std::function<void(LocalMemoryProbeDevice&)>& change) {
        SampleRequest sample;
        withProbe(sample);
        change(sample.request.target.localMemoryProbeDevice);
        require(sample.Key() != probedKey, "the key of a probed layout choice ignores the " + what);
        SampleRequest settledSample;
        withProbe(settledSample);
        settledSample.settled.layout = WaveLayout::SingleLane;
        const auto settledKey = settledSample.Key();
        change(settledSample.request.target.localMemoryProbeDevice);
        require(settledSample.Key() == settledKey, "the key of a settled layout depends on the " + what);
        SampleRequest unprobed;
        const auto unprobedKey = unprobed.Key();
        change(unprobed.request.target.localMemoryProbeDevice);
        require(unprobed.Key() == unprobedKey, "the key of an unprobed request depends on the " + what);
    };
    device("vendor", [](LocalMemoryProbeDevice& value) { value.vendorId = 0x1002u; });
    device("device", [](LocalMemoryProbeDevice& value) { value.deviceId += 1u; });
    device("driver version", [](LocalMemoryProbeDevice& value) { value.driverVersion += 1u; });
    for (std::size_t index = 0; index < 16; ++index) {
        device("pipeline cache UUID byte " + std::to_string(index), [&](LocalMemoryProbeDevice& value) { value.pipelineCacheUuid[index] ^= 0x01u; });
    }

    const auto unsplit = [](SampleRequest& sample) {
        withProbe(sample);
        sample.request.context.compute->numThreads = {32u, 1u, 1u};
    };
    SampleRequest small;
    unsplit(small);
    require(!LayoutChosenByProbe(small.request), "a one-subgroup workgroup has its layout chosen by the probe");
    const auto smallKey = small.Key();
    SampleRequest smallOther;
    unsplit(smallOther);
    smallOther.request.target.localMemoryProbeDevice.driverVersion += 1u;
    require(smallOther.Key() == smallKey, "the key of a program the probe never sees depends on the device");

    SampleRequest fixed;
    withProbe(fixed);
    fixed.request.waveLayout = WaveLayout::TwoLane;
    require(!LayoutChosenByProbe(fixed.request), "a program with a fixed layout has it chosen by the probe");
}

void verifySwitchList() {
    const auto keyed = ShaderDiskCache::KeyedSwitches();
    const auto has = [&](std::string_view name) { return std::find(keyed.begin(), keyed.end(), name) != keyed.end(); };
    require(has("APS5_INEXACT_SINGLE_LANE") && has("APS5_LOOP_GUARD") && has("APS5_TWO_LANE"), "a recompiler switch is not keyed");
    require(!has("APS5_TRACE_LOCAL_MEMORY_PROBE") && !has("APS5_PROFILE_DRAW") && !has("APS5_DUMP_IR"), "a diagnostic switch is keyed");
}

void verifyFormatDigest() {
#if defined(__x86_64__) || defined(_M_X64)
    std::vector<std::byte> result;
    ShaderDiskCache::EncodeResult(sampleResult(), result);
    SampleRequest sample;
    const auto key = sample.Key();
    const auto file = ShaderDiskCache::EncodeEntry(key, sampleVariant());
    const auto payload = std::span(file).subspan(48 + key.size());
    const auto resultDigest = HashBytes(result);
    const auto payloadDigest = HashBytes(payload);
    constexpr std::uint32_t DigestFormat = 9;
    constexpr std::uint64_t ResultDigest = 0xcbdb49376ef9eaa8ull;
    constexpr std::uint64_t PayloadDigest = 0x7af0e1bec2770d11ull;
    char text[160];
    std::snprintf(text, sizeof(text), "format %u: result digest 0x%016llx, payload digest 0x%016llx", ShaderDiskCache::FormatVersion, static_cast<unsigned long long>(resultDigest), static_cast<unsigned long long>(payloadDigest));
    require(ShaderDiskCache::FormatVersion == DigestFormat && resultDigest == ResultDigest && payloadDigest == PayloadDigest, std::string("the entry encoding changed (") + text + "): bump ShaderDiskCache::FormatVersion and record the new digests here");
#endif
}

void verifyStore() {
    require(ShaderDiskCache::Enabled(), "the disk cache is not enabled");
    SampleRequest sample;
    const auto key = sample.Key();
    const auto variant = std::make_shared<const CompiledVariant>(sampleVariant());
    const auto before = ShaderDiskCache::Totals();
    CompiledVariant loaded;
    require(!ShaderDiskCache::Load(key, loaded), "an entry loads before it was stored");
    ShaderDiskCache::Store(key, variant);
    ShaderDiskCache::Flush();
    require(ShaderDiskCache::Totals().writes == before.writes + 1, "the store did not write the entry");
    const auto path = ShaderDiskCache::EntryDirectory() / ShaderDiskCache::EntryName(key);
    require(std::filesystem::exists(path), "no entry file at " + path.string());
    require(ShaderDiskCache::Load(key, loaded), "a stored entry does not load");
    requireSameVariant(*variant, loaded, "store round trip");
    require(ShaderDiskCache::Totals().hits == before.hits + 1, "the load was not counted as a hit");

    std::vector<std::byte> file;
    require(ReadWholeFile(path, file), "cannot read the entry back");
    require(WriteFileAtomically(path, std::span(file).first(file.size() / 2)), "cannot truncate the entry");
    require(!ShaderDiskCache::Load(key, loaded), "a truncated entry loads");
    auto corrupt = file;
    corrupt[corrupt.size() - 5] ^= std::byte{0x40};
    require(WriteFileAtomically(path, corrupt), "cannot damage the entry");
    require(!ShaderDiskCache::Load(key, loaded), "a corrupt entry loads");
    require(ShaderDiskCache::Totals().loadFailures == before.loadFailures + 2, "the rejected entries were not counted");
    ShaderDiskCache::Store(key, variant);
    ShaderDiskCache::Flush();
    require(ShaderDiskCache::Load(key, loaded), "a rewritten entry does not load");
}

struct ComputeRequest {
    std::vector<std::uint32_t> code{0xe0700000u, 0x80000000u, 0xbf810000u};
    std::array<std::uint32_t, 4> userData{0x10000000u, 0x00000000u, 0x40u, 0x00027facu};
    std::array<std::uint32_t, 1> capabilities{1u};
    RecompileRequest request{};

    explicit ComputeRequest(bool useCache) {
        request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.supportedCapabilities = capabilities;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = useCache;
    }
};

struct ProbedRequest {
    std::vector<std::uint32_t> code{0xe0700000u, 0x80000000u, 0xbf800000u, 0xbf810000u};
    std::array<std::uint32_t, 4> userData{0x10000000u, 0x00000000u, 0x40u, 0x00027facu};
    std::array<std::uint32_t, 1> capabilities{1u};
    RecompileRequest request{};

    explicit ProbedRequest(std::uint32_t pushConstantBytes) {
        request.shader = {ShaderStage::Compute, 0x30000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.supportedCapabilities = capabilities;
        request.target.localMemoryProbe = &noLocalMemory;
        request.layout.pushConstantSizeBytes = pushConstantBytes;
    }
};

int runOrderProcess(bool reversed) {
    const std::array<std::uint32_t, 2> order = reversed ? std::array<std::uint32_t, 2>{64u, 128u} : std::array<std::uint32_t, 2>{128u, 64u};
    for (const auto bytes : order) {
        ProbedRequest probed(bytes);
        static_cast<void>(Recompile(probed.request));
    }
    ShaderDiskCache::Flush();
    const auto totals = ShaderDiskCache::Totals();
    std::cout << (reversed ? "reversed" : "same") << " order: " << totals.hits << " hits, " << totals.misses << " misses\n";
    if (reversed) require(totals.hits == 0 && totals.misses == 2, "variants compiled in another order loaded entries keyed to the first order (hits " + std::to_string(totals.hits) + ")");
    else require(totals.hits == 2 && totals.misses == 0, "variants compiled in the stored order did not load (hits " + std::to_string(totals.hits) + ", misses " + std::to_string(totals.misses) + ")");
    return 0;
}

void verifyRunOrder(const char* self) {
    const auto before = ShaderDiskCache::Totals();
    for (const auto bytes : {128u, 64u}) {
        ProbedRequest probed(bytes);
        const auto result = Recompile(probed.request);
        require(result.waveLayout == WaveLayout::TwoLane, "a probed program without local memory did not keep two lanes per invocation");
    }
    ShaderDiskCache::Flush();
    const auto after = ShaderDiskCache::Totals();
    require(after.misses == before.misses + 2 && after.writes == before.writes + 2, "the probed variants were not looked up and stored");
    require(std::system(("\"" + std::string(self) + "\" --order-reversed").c_str()) == 0, "the reversed-order process failed");
    require(std::system(("\"" + std::string(self) + "\" --order-same").c_str()) == 0, "the same-order process failed");
}

int runLoadingProcess() {
    ComputeRequest cached(true);
    const auto loaded = Recompile(cached.request);
    const auto totals = ShaderDiskCache::Totals();
    require(totals.hits == 1 && totals.misses == 0 && totals.loadFailures == 0, "the second process did not load the stored variant (hits " + std::to_string(totals.hits) + ", misses " + std::to_string(totals.misses) + ")");
    ComputeRequest fresh(false);
    const auto compiled = Recompile(fresh.request);
    requireSameResult(compiled, loaded, "loaded against compiled");
    require(!loaded.spirv.empty(), "the loaded SPIR-V is empty");
    std::cout << "second process loaded " << loaded.spirv.size() << " SPIR-V words from the disk cache\n";
    return 0;
}

void verifyAcrossProcesses(const char* self) {
    ComputeRequest request(true);
    const auto before = ShaderDiskCache::Totals();
    const auto compiled = Recompile(request.request);
    ShaderDiskCache::Flush();
    const auto after = ShaderDiskCache::Totals();
    require(after.misses == before.misses + 1 && after.writes == before.writes + 1, "the first compile was not looked up and stored");
    require(!compiled.spirv.empty(), "the compile produced no SPIR-V");
    const std::string command = "\"" + std::string(self) + "\" --load";
    require(std::system(command.c_str()) == 0, "the loading process failed");
}

void verifyDefaultDirectory(const char* self) {
    setEnvironment("ANYPS5_SHADER_CACHE_DIR", "");
    setEnvironment("ANYPS5_NO_SHADER_CACHE", "1");
    require(ShaderRecompiler::ShaderCacheDirectory().empty(), "ANYPS5_NO_SHADER_CACHE=1 did not disable the cache");
    setEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    require(directory.filename() == "shader_cache", "the default cache directory is not named shader_cache");
    std::error_code error;
    require(std::filesystem::equivalent(directory.parent_path(), std::filesystem::absolute(self).parent_path(), error) && !error, "the default cache directory is not beside the executable");
}

}

int main(int argc, char** argv) {
    try {
        for (const auto name : ShaderDiskCache::KeyedSwitches()) unsetEnvironment(std::string(name).c_str());
        if (argc == 2 && std::string_view(argv[1]) == "--load") return runLoadingProcess();
        if (argc == 2 && std::string_view(argv[1]) == "--order-reversed") return runOrderProcess(true);
        if (argc == 2 && std::string_view(argv[1]) == "--order-same") return runOrderProcess(false);
        const auto directory = std::filesystem::temp_directory_path() / ("aps5-shader-disk-cache-test-" + std::to_string(std::random_device{}()));
        std::filesystem::remove_all(directory);
        verifyDefaultDirectory(argv[0]);
        setEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
        setEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.string());
        verifyResultRoundTrip();
        verifyEntryRoundTrip();
        verifyKeySensitivity();
        verifyLayoutKey();
        verifySwitchList();
        verifyFormatDigest();
        verifyStore();
        verifyAcrossProcesses(argv[0]);
        verifyRunOrder(argv[0]);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::cout << "shader disk cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
