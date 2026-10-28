#pragma once
#include "vulkan_context.h"
#include "vulkan_readback_spirv.h"

// Read an RGBA8 image through a native compute pipeline. This helper bypasses
// the adapter and Sokol shader generator so it cannot mask their binding bugs.
struct VulkanImageReadback {
    VulkanReadbackContext& context;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    explicit VulkanImageReadback(VulkanReadbackContext& value) : context(value) {}
    ~VulkanImageReadback() {
        if (view) vkDestroyImageView(context.device, view, nullptr);
        if (pool) vkDestroyDescriptorPool(context.device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(context.device, pipeline, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
        if (layout) vkDestroyDescriptorSetLayout(context.device, layout, nullptr);
        if (shader) vkDestroyShaderModule(context.device, shader, nullptr);
    }

    void read(sg_image image, void* destination, uint32_t width, uint32_t height) {
        auto* resource = _sg_lookup_image(image.id);
        require(resource != nullptr, "color image missing");
        auto commands = context.begin_readback(width * height * 4);
        if (!pipeline) {
            VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize = sizeof(vulkan_readback_spirv);
            module.pCode = vulkan_readback_spirv;
            check_vk(vkCreateShaderModule(context.device, &module, nullptr, &shader), "create readback shader");
            const VkDescriptorSetLayoutBinding bindings[] = {
                {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            };
            VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            set_info.bindingCount = 2;
            set_info.pBindings = bindings;
            check_vk(vkCreateDescriptorSetLayout(context.device, &set_info, nullptr, &layout), "create readback descriptor layout");
            VkPipelineLayoutCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipeline_info.setLayoutCount = 1;
            pipeline_info.pSetLayouts = &layout;
            check_vk(vkCreatePipelineLayout(context.device, &pipeline_info, nullptr, &pipeline_layout), "create readback pipeline layout");
            VkComputePipelineCreateInfo compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            compute.layout = pipeline_layout;
            compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            compute.stage.module = shader;
            compute.stage.pName = "main";
            check_vk(vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline), "create readback pipeline");
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
            VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool_info.maxSets = 1;
            pool_info.poolSizeCount = 2;
            pool_info.pPoolSizes = sizes;
            check_vk(vkCreateDescriptorPool(context.device, &pool_info, nullptr, &pool), "create readback descriptor pool");
            VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = resource->vk.img;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check_vk(vkCreateImageView(context.device, &view_info, nullptr, &view), "create readback image view");
        }
        check_vk(vkResetDescriptorPool(context.device, pool, 0), "reset readback descriptor pool");
        VkDescriptorSetAllocateInfo allocation = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &layout;
        VkDescriptorSet set;
        check_vk(vkAllocateDescriptorSets(context.device, &allocation, &set), "allocate readback descriptor set");
        const VkDescriptorImageInfo image_info = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo buffer_info = {context.staging, 0, width * height * 4};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &image_info;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = _sg_vk_image_layout(resource->vk.cur_access);
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = resource->vk.img;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
        barrier.newLayout = barrier.oldLayout;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        context.end_readback(commands, destination, width * height * 4);
    }
};
