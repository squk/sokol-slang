#pragma once
#include "test_support.h"
#include SHADER_HEADER
#include <cstdint>
#include <cstdio>
#include <initializer_list>

struct DepthFixture {
    static constexpr int width = 64, height = 64;
    sg_image color = {};
    sg_view color_view = {}, depth_view = {};
    sg_pipeline pipeline = {};
    sg_bindings bindings = {};

    void init(sg_image_desc color_image = {}) {
        const auto* descriptor = depth_draw_shader_desc(sg_query_backend());
        require(descriptor != nullptr, "depth shader descriptor missing");
        sg_shader shader = sg_make_shader(descriptor);
        require(sg_query_shader_state(shader) == SG_RESOURCESTATE_VALID, "depth shader creation failed");
        sg_image_desc image = {};
        image.width = image.height = 4;
        image.pixel_format = SG_PIXELFORMAT_DEPTH;
        image.usage.depth_stencil_attachment = true;
        sg_image depth = sg_make_image(&image);
        require(sg_query_image_state(depth) == SG_RESOURCESTATE_VALID, "sampled depth image creation failed");
        sg_view_desc view = {};
        view.depth_stencil_attachment.image = depth;
        depth_view = sg_make_view(&view);
        view = {};
        view.texture.image = depth;
        sg_view texture_view = sg_make_view(&view);
        require(sg_query_view_state(depth_view) == SG_RESOURCESTATE_VALID &&
                sg_query_view_state(texture_view) == SG_RESOURCESTATE_VALID, "depth views failed");

        sg_sampler_desc sampler = {};
        sampler.min_filter = sampler.mag_filter = SG_FILTER_NEAREST;
        sampler.wrap_u = sampler.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        sampler.compare = SG_COMPAREFUNC_LESS;
        sg_sampler less = sg_make_sampler(&sampler);
        sampler.compare = SG_COMPAREFUNC_GREATER;
        sg_sampler greater = sg_make_sampler(&sampler);
        require(sg_query_sampler_state(less) == SG_RESOURCESTATE_VALID &&
                sg_query_sampler_state(greater) == SG_RESOURCESTATE_VALID, "comparison samplers failed");
        bindings.views[VIEW_depth_fragment_shadow] = texture_view;
        bindings.samplers[SMP_depth_fragment_lessSmp] = less;
        bindings.samplers[SMP_depth_fragment_greaterSmp] = greater;

        color_image.width = width; color_image.height = height;
        color_image.pixel_format = SG_PIXELFORMAT_RGBA8;
        color_image.usage.color_attachment = true;
        color = sg_make_image(&color_image);
        view = {};
        view.color_attachment.image = color;
        color_view = sg_make_view(&view);
        require(sg_query_image_state(color) == SG_RESOURCESTATE_VALID &&
                sg_query_view_state(color_view) == SG_RESOURCESTATE_VALID, "color target failed");
        sg_pipeline_desc pipeline_desc = {};
        pipeline_desc.shader = shader;
        pipeline_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        pipeline_desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
        pipeline = sg_make_pipeline(&pipeline_desc);
        require(sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID, "depth sampling pipeline failed");
    }

    void begin_draw(float clear_depth) {
        // Clear and sample in the same submission, exercising attachment-to-texture transitions.
        sg_pass pass = {};
        pass.attachments.depth_stencil = depth_view;
        pass.action.depth.load_action = SG_LOADACTION_CLEAR;
        pass.action.depth.store_action = SG_STOREACTION_STORE;
        pass.action.depth.clear_value = clear_depth;
        sg_begin_pass(&pass);
        sg_end_pass();
        pass = {};
        pass.attachments.colors[0] = color_view;
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = {0,0,1,0};
        sg_begin_pass(&pass);
        sg_apply_pipeline(pipeline);
        sg_apply_bindings(&bindings);
        sg_draw(0, 3, 1);
    }

    void check_color(const uint8_t* pixels, float clear_depth, bool origin_top_left) const {
        for (int row = 0; row < height; ++row) for (int x = 0; x < width; ++x) {
            const int y = origin_top_left ? height - 1 - row : row;
            const uint8_t expected[] = {uint8_t((x + .5f) / width < clear_depth ? 255 : 0),
                                       uint8_t((y + .5f) / height > clear_depth ? 255 : 0), 0, 255};
            for (int channel = 0; channel < 4; ++channel) {
                const auto actual = pixels[(row * width + x) * 4 + channel];
                if (actual != expected[channel]) {
                    std::fprintf(stderr, "depth %g, pixel (%d,%d), channel %d: got %u expected %u\n",
                                 clear_depth, x, y, channel, actual, expected[channel]);
                    throw std::runtime_error("depth comparison mismatch");
                }
            }
        }
    }
};
