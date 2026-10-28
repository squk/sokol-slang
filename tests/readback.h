#pragma once
// Sokol has no portable GPU readback API. Only this helper uses native handles.
#include "test_support.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using ReadbackCallback = std::function<void(const uint8_t*, size_t)>;
static void fail_test(const char* message);

#if defined(SOKOL_VULKAN)
#include "vulkan_readback.h"
static std::unique_ptr<VulkanReadbackContext> vulkan_readback;
static std::unique_ptr<VulkanImageReadback> vulkan_image_readback;
#elif defined(SOKOL_D3D11)
#include "d3d11_support.h"
#elif defined(SOKOL_WGPU)
static std::vector<WGPUTexture> readback_textures;
static WGPUQueue readback_queue;
static std::string readback_message(WGPUStringView message) {
    if (!message.data) return {};
    return message.length == WGPU_STRLEN ? std::string(message.data) : std::string(message.data, message.length);
}
static WGPUDevice readback_device() { return (WGPUDevice)sglue_environment().wgpu.device; }

struct WgpuReadback {
    WGPUBuffer buffer;
    size_t size, row_bytes;
    ReadbackCallback callback;
};

static void submit_readback(WGPUCommandEncoder encoder, WGPUBuffer buffer,
                            size_t size, size_t row_bytes, ReadbackCallback callback) {
    WGPUCommandBufferDescriptor descriptor = {};
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, &descriptor);
    if (!readback_queue) readback_queue = wgpuDeviceGetQueue(readback_device());
    wgpuQueueSubmit(readback_queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    auto* job = new WgpuReadback{buffer, size, row_bytes, std::move(callback)};
    WGPUBufferMapCallbackInfo mapped = {};
    mapped.mode = WGPUCallbackMode_AllowSpontaneous;
    mapped.userdata1 = job;
    mapped.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* user, void*) {
        std::unique_ptr<WgpuReadback> job(static_cast<WgpuReadback*>(user));
        try {
            if (status != WGPUMapAsyncStatus_Success)
                throw std::runtime_error("WebGPU readback mapping: " + readback_message(message));
            auto* bytes = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(job->buffer, 0, job->size));
            require(bytes != nullptr, "mapped readback unavailable");
            job->callback(bytes, job->row_bytes);
            wgpuBufferUnmap(job->buffer);
        } catch (const std::exception& error) { fail_test(error.what()); }
        wgpuBufferRelease(job->buffer);
    };
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size, mapped);
}

static WGPUBuffer make_readback_buffer(size_t size) {
    WGPUBufferDescriptor descriptor = {};
    descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    descriptor.size = size;
    return wgpuDeviceCreateBuffer(readback_device(), &descriptor);
}
#endif

static sg_image_desc readback_target(int width, int height, bool storage = false) {
    sg_image_desc image = {};
#if defined(SOKOL_WGPU)
    // Sokol's default images omit CopySrc; inject a texture that can be copied.
    WGPUTextureDescriptor descriptor = {};
    descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
        (storage ? WGPUTextureUsage_StorageBinding : WGPUTextureUsage_RenderAttachment);
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = {(uint32_t)width, (uint32_t)height, 1};
    descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    descriptor.mipLevelCount = descriptor.sampleCount = 1;
    auto texture = wgpuDeviceCreateTexture(readback_device(), &descriptor);
    readback_textures.push_back(texture);
    image.wgpu_texture = texture;
#endif
    return image;
}

static bool readback_top_left() {
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    return false;
#else
    return true;
#endif
}

static std::vector<float> read_depth(int width, int height) {
    std::vector<float> depths;
#if defined(SOKOL_GLCORE)
    depths.resize(width * height);
    glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depths.data());
    require(glGetError() == GL_NO_ERROR, "depth readback failed");
#endif
    return depths;
}

static void read_buffer(sg_buffer source, size_t size, ReadbackCallback callback) {
#if defined(SOKOL_WGPU)
    auto buffer = make_readback_buffer(size);
    WGPUCommandEncoderDescriptor descriptor = {};
    auto encoder = wgpuDeviceCreateCommandEncoder(readback_device(), &descriptor);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, (WGPUBuffer)sg_wgpu_query_buffer_info(source).buf,
                                        0, buffer, 0, size);
    submit_readback(encoder, buffer, size, 0, std::move(callback));
#else
    std::vector<uint8_t> bytes(size);
#if defined(SOKOL_METAL)
    auto device = (__bridge id<MTLDevice>)sglue_environment().metal.device;
    const auto native = sg_mtl_query_buffer_info(source);
    auto buffer = (__bridge id<MTLBuffer>)native.buf[native.active_slot];
    id<MTLBuffer> staging = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    auto queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
    auto commands = [queue commandBuffer];
    auto blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:buffer sourceOffset:0 toBuffer:staging destinationOffset:0 size:size];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    require(commands.status == MTLCommandBufferStatusCompleted, "buffer readback failed");
    std::memcpy(bytes.data(), staging.contents, size);
    [staging release];
#elif defined(SOKOL_D3D11)
    auto device = (ID3D11Device*)sglue_environment().d3d11.device;
    auto context = (ID3D11DeviceContext*)sglue_environment().d3d11.device_context;
    auto buffer = (ID3D11Buffer*)sg_d3d11_query_buffer_info(source).buf;
    D3D11_BUFFER_DESC descriptor = {};
    buffer->GetDesc(&descriptor);
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.BindFlags = descriptor.MiscFlags = descriptor.StructureByteStride = 0;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComOwner<ID3D11Buffer> staging;
    check_hr(device->CreateBuffer(&descriptor, nullptr, &staging.value), "create readback buffer");
    context->CopyResource(staging.value, buffer);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    check_hr(context->Map(staging.value, 0, D3D11_MAP_READ, 0, &mapped), "map readback buffer");
    std::memcpy(bytes.data(), mapped.pData, size);
    context->Unmap(staging.value, 0);
#elif defined(SOKOL_VULKAN)
    if (!vulkan_readback) vulkan_readback = std::make_unique<VulkanReadbackContext>();
    vulkan_readback->read_buffer(source, bytes.data(), size);
#elif !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    const auto buffer = sg_gl_query_buffer_info(source);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer.buf[buffer.active_slot]);
    const void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, size, GL_MAP_READ_BIT);
    require(mapped != nullptr, "buffer readback failed");
    std::memcpy(bytes.data(), mapped, size);
    require(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER), "readback data invalidated");
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    require(glGetError() == GL_NO_ERROR, "buffer readback GL error");
    sg_reset_state_cache();
#else
    throw std::runtime_error("compute readback unavailable on this backend");
#endif
    callback(bytes.data(), 0);
#endif
}

static void read_image(sg_image source, int width, int height, sg_pixel_format format, ReadbackCallback callback) {
    const size_t row_bytes = width * (format == SG_PIXELFORMAT_RGBA32F ? 16 : 4);
#if defined(SOKOL_WGPU)
    const size_t stride = (row_bytes + 255) & ~size_t(255);
    auto buffer = make_readback_buffer(stride * height);
    WGPUCommandEncoderDescriptor descriptor = {};
    auto encoder = wgpuDeviceCreateCommandEncoder(readback_device(), &descriptor);
    WGPUTexelCopyTextureInfo input = {};
    input.texture = (WGPUTexture)sg_wgpu_query_image_info(source).tex;
    input.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo output = {};
    output.buffer = buffer;
    output.layout.bytesPerRow = stride;
    output.layout.rowsPerImage = height;
    WGPUExtent3D extent = {(uint32_t)width, (uint32_t)height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &input, &output, &extent);
    submit_readback(encoder, buffer, stride * height, stride, std::move(callback));
#else
    std::vector<uint8_t> bytes(row_bytes * height);
#if defined(SOKOL_METAL)
    auto device = (__bridge id<MTLDevice>)sglue_environment().metal.device;
    const auto native = sg_mtl_query_image_info(source);
    auto image = (__bridge id<MTLTexture>)native.tex[native.active_slot];
    const size_t stride = (row_bytes + 255) & ~size_t(255);
    id<MTLBuffer> staging = [device newBufferWithLength:stride * height options:MTLResourceStorageModeShared];
    auto queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
    auto commands = [queue commandBuffer];
    auto blit = [commands blitCommandEncoder];
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toBuffer:staging destinationOffset:0
        destinationBytesPerRow:stride destinationBytesPerImage:stride * height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    require(commands.status == MTLCommandBufferStatusCompleted, "image readback failed");
    for (int row = 0; row < height; ++row)
        std::memcpy(bytes.data() + row * row_bytes, (const uint8_t*)staging.contents + row * stride, row_bytes);
    [staging release];
#elif defined(SOKOL_D3D11)
    auto device = (ID3D11Device*)sglue_environment().d3d11.device;
    auto context = (ID3D11DeviceContext*)sglue_environment().d3d11.device_context;
    auto image = (ID3D11Texture2D*)sg_d3d11_query_image_info(source).tex2d;
    D3D11_TEXTURE2D_DESC descriptor = {};
    image->GetDesc(&descriptor);
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.BindFlags = descriptor.MiscFlags = 0;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComOwner<ID3D11Texture2D> staging;
    check_hr(device->CreateTexture2D(&descriptor, nullptr, &staging.value), "create readback image");
    context->CopyResource(staging.value, image);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    check_hr(context->Map(staging.value, 0, D3D11_MAP_READ, 0, &mapped), "map readback image");
    for (int row = 0; row < height; ++row)
        std::memcpy(bytes.data() + row * row_bytes, (const uint8_t*)mapped.pData + row * mapped.RowPitch, row_bytes);
    context->Unmap(staging.value, 0);
#elif defined(SOKOL_VULKAN)
    require(format == SG_PIXELFORMAT_RGBA8, "Vulkan image readback requires RGBA8");
    if (!vulkan_readback) vulkan_readback = std::make_unique<VulkanReadbackContext>();
    if (!vulkan_image_readback) vulkan_image_readback = std::make_unique<VulkanImageReadback>(*vulkan_readback);
    vulkan_image_readback->read(source, bytes.data(), width, height);
#else
    const auto image = sg_gl_query_image_info(source);
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image.tex[image.active_slot], 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "readback framebuffer incomplete");
    glReadPixels(0, 0, width, height, GL_RGBA, format == SG_PIXELFORMAT_RGBA32F ? GL_FLOAT : GL_UNSIGNED_BYTE, bytes.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    require(glGetError() == GL_NO_ERROR, "image readback GL error");
    sg_reset_state_cache();
#endif
    callback(bytes.data(), row_bytes);
#endif
}

static void cleanup_readbacks() {
#if defined(SOKOL_WGPU)
    for (auto texture : readback_textures) wgpuTextureRelease(texture);
    readback_textures.clear();
    if (readback_queue) wgpuQueueRelease(readback_queue);
    readback_queue = nullptr;
#elif defined(SOKOL_VULKAN)
    vulkan_image_readback.reset();
    vulkan_readback.reset();
#endif
}
