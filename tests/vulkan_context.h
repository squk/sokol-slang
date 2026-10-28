#pragma once
#include "test_support.h"
#include <cstring>
#include <string>
#include <vector>

static void check_vk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (VkResult " + std::to_string(result) + ")");
}

struct VulkanReadbackContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    VulkanReadbackContext() {
        const auto environment = sglue_environment().vulkan;
        physical = (VkPhysicalDevice)environment.physical_device;
        device = (VkDevice)environment.device;
        queue = (VkQueue)environment.queue;
        VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = environment.queue_family_index;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check_vk(vkCreateCommandPool(device, &info, nullptr, &pool), "create readback command pool");
    }
    ~VulkanReadbackContext() {
        vkDeviceWaitIdle(device);
        if (staging) vkDestroyBuffer(device, staging, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
    }

    VkCommandBuffer begin_readback(size_t size) {
        check_vk(vkQueueWaitIdle(queue), "wait for Sokol submission");
        if (!staging) {
            VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = size;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            check_vk(vkCreateBuffer(device, &info, nullptr, &staging), "create readback buffer");
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(device, staging, &requirements);
            VkPhysicalDeviceMemoryProperties properties;
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    type = i;
                    break;
                }
            }
            require(type != UINT32_MAX, "host-visible Vulkan memory required");
            VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            check_vk(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate readback memory");
            check_vk(vkBindBufferMemory(device, staging, memory, 0), "bind readback memory");
        }
        check_vk(vkResetCommandPool(device, pool, 0), "reset readback commands");
        VkCommandBufferAllocateInfo allocation = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer commands;
        check_vk(vkAllocateCommandBuffers(device, &allocation, &commands), "allocate readback commands");
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(commands, &begin), "begin readback");
        return commands;
    }

    void end_readback(VkCommandBuffer commands, void* destination, size_t size) {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             1, &barrier, 0, nullptr, 0, nullptr);
        check_vk(vkEndCommandBuffer(commands), "end readback");
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        check_vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit readback");
        check_vk(vkQueueWaitIdle(queue), "wait for readback");
        void* mapped = nullptr;
        check_vk(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map readback");
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.size = VK_WHOLE_SIZE;
        const auto invalidated = vkInvalidateMappedMemoryRanges(device, 1, &range);
        if (invalidated == VK_SUCCESS) std::memcpy(destination, mapped, size);
        vkUnmapMemory(device, memory);
        check_vk(invalidated, "invalidate readback memory");
        vkFreeCommandBuffers(device, pool, 1, &commands);
    }

    // The pinned Sokol has no public Vulkan native-handle query. These lookups
    // are test-only; shader creation, bindings, draws and dispatch use public APIs.
    void read_buffer(sg_buffer buffer, void* destination, size_t size) {
        auto* resource = _sg_lookup_buffer(buffer.id);
        require(resource != nullptr, "compute result buffer missing");
        auto commands = begin_readback(size);
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        const VkBufferCopy copy = {0, 0, size};
        vkCmdCopyBuffer(commands, resource->vk.buf, staging, 1, &copy);
        end_readback(commands, destination, size);
    }

};
