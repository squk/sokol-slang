#pragma once
#include "test_support.h"
// Shared offscreen draw and independent pixel expectations for backend tests.
#include SHADER_HEADER
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

// Independently authored host layout, checked against both generated interfaces.
struct alignas(16) HostParams { float transform[16]; float tint[4]; };
struct Vertex { float position[2]; float uv[2]; };
static_assert(sizeof(HostParams) == sizeof(portable_vertex_params_t));
static_assert(sizeof(HostParams) == sizeof(portable_fragment_params_t));
static_assert(offsetof(HostParams, tint) == offsetof(portable_fragment_params_t, tint));

struct PortableFixture {
    static constexpr int width = 64, height = 64;
    // Distinct colors and alpha detect texture binding and UV mistakes.
    static constexpr uint8_t texels[] = {255,0,0,255, 0,255,0,128, 0,0,255,64, 255,255,255,255};
    sg_view color_view = {}, depth_view = {};
    sg_pipeline pipeline = {};
    sg_bindings bindings = {};
    HostParams vertex_params = {}, fragment_params = {};

    // Optional native color image enables backend-specific readback usage flags.
    void init(sg_image_desc color_image = {}) {
        const auto* descriptor = portable_draw_shader_desc(sg_query_backend());
        require(descriptor != nullptr, "generated backend descriptor missing");
        sg_shader shader = sg_make_shader(descriptor);
        require(sg_query_shader_state(shader) == SG_RESOURCESTATE_VALID, "shader creation failed");

        const Vertex vertices[] = {{{-1,-1},{0,0}}, {{1,-1},{1,0}}, {{1,1},{1,1}}, {{-1,1},{0,1}}};
        const uint16_t indices[] = {0,1,2, 0,2,3};
        sg_buffer_desc buffer = {};
        buffer.data = SG_RANGE(vertices);
        sg_buffer vb = sg_make_buffer(&buffer);
        buffer = {};
        buffer.usage.index_buffer = true;
        buffer.data = SG_RANGE(indices);
        sg_buffer ib = sg_make_buffer(&buffer);
        require(sg_query_buffer_state(vb) == SG_RESOURCESTATE_VALID &&
                sg_query_buffer_state(ib) == SG_RESOURCESTATE_VALID, "vertex/index buffer creation failed");

        sg_image_desc image = {};
        image.width = image.height = 2;
        image.pixel_format = SG_PIXELFORMAT_RGBA8;
        image.data.mip_levels[0] = SG_RANGE(texels);
        sg_image texture = sg_make_image(&image);
        require(sg_query_image_state(texture) == SG_RESOURCESTATE_VALID, "texture creation failed");
        sg_view_desc view = {};
        view.texture.image = texture;
        sg_view texture_view = sg_make_view(&view);
        sg_sampler_desc sampler_desc = {};
        sampler_desc.min_filter = sampler_desc.mag_filter = SG_FILTER_NEAREST;
        sampler_desc.wrap_u = sampler_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        sg_sampler sampler = sg_make_sampler(&sampler_desc);
        require(sg_query_sampler_state(sampler) == SG_RESOURCESTATE_VALID, "sampler creation failed");

        image = color_image;
        image.width = width; image.height = height;
        image.pixel_format = SG_PIXELFORMAT_RGBA8;
        image.usage.color_attachment = true;
        sg_image color = sg_make_image(&image);
        image = {};
        image.width = width; image.height = height;
        image.pixel_format = SG_PIXELFORMAT_DEPTH;
        image.usage = {};
        image.usage.depth_stencil_attachment = true;
        sg_image depth = sg_make_image(&image);
        require(sg_query_image_state(color) == SG_RESOURCESTATE_VALID &&
                sg_query_image_state(depth) == SG_RESOURCESTATE_VALID, "render target creation failed");
        view = {};
        view.color_attachment.image = color;
        color_view = sg_make_view(&view);
        view = {};
        view.depth_stencil_attachment.image = depth;
        depth_view = sg_make_view(&view);
        require(sg_query_view_state(texture_view) == SG_RESOURCESTATE_VALID &&
                sg_query_view_state(color_view) == SG_RESOURCESTATE_VALID &&
                sg_query_view_state(depth_view) == SG_RESOURCESTATE_VALID, "view creation failed");

        sg_pipeline_desc pipeline_desc = {};
        pipeline_desc.shader = shader;
        pipeline_desc.index_type = SG_INDEXTYPE_UINT16;
        pipeline_desc.layout.buffers[0].stride = sizeof(Vertex);
        pipeline_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
        pipeline_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
        pipeline_desc.layout.attrs[1].offset = offsetof(Vertex, uv);
        pipeline_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        pipeline_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        pipeline_desc.depth.compare = SG_COMPAREFUNC_LESS;
        pipeline_desc.depth.write_enabled = true;
        pipeline = sg_make_pipeline(&pipeline_desc);
        require(sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID, "pipeline creation failed");

        vertex_params.transform[0] = .5f;
        vertex_params.transform[5] = .25f;
        vertex_params.transform[10] = vertex_params.transform[15] = 1;
        vertex_params.transform[12] = -.25f;
        vertex_params.transform[13] = .25f;
        fragment_params.tint[0] = .5f; fragment_params.tint[1] = .25f;
        fragment_params.tint[2] = .75f; fragment_params.tint[3] = .8f;
        // Unused fields stay zero: sharing the wrong stage's data breaks the draw.
        bindings.vertex_buffers[0] = vb;
        bindings.index_buffer = ib;
        bindings.views[VIEW_portable_fragment_tex] = texture_view;
        bindings.samplers[SMP_portable_fragment_smp] = sampler;
    }

    void begin_draw(float clear_depth) {
        sg_pass pass = {};
        pass.attachments.colors[0] = color_view;
        pass.attachments.depth_stencil = depth_view;
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = {0,0,0,0};
        pass.action.depth.load_action = SG_LOADACTION_CLEAR;
        pass.action.depth.store_action = SG_STOREACTION_STORE;
        pass.action.depth.clear_value = clear_depth;
        sg_begin_pass(&pass);
        sg_apply_pipeline(pipeline);
        sg_apply_bindings(&bindings);
        const auto vertex_uniforms = SG_RANGE(vertex_params);
        const auto fragment_uniforms = SG_RANGE(fragment_params);
        sg_apply_uniforms(UB_portable_vertex_params, &vertex_uniforms);
        sg_apply_uniforms(UB_portable_fragment_params, &fragment_uniforms);
        sg_draw(0, 6, 1);
    }

    void check_depth(const float* depths, float clear_depth) const {
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const bool covered = clear_depth > .25f && x >= 8 && x < 40 && y >= 32 && y < 48;
            const float expected = covered ? .25f : clear_depth;
            const float actual = depths[y * width + x];
            if (!std::isfinite(actual) || std::abs(actual - expected) > 0.00001f)
                throw std::runtime_error("clip depth/depth rejection mismatch");
        }
    }

    void check_color(const uint8_t* pixels, float clear_depth, bool origin_top_left) const {
        // Compare in bottom-left coordinates; APIs with a top-left framebuffer
        // origin reverse readback rows. Texture UVs are checked independently.
        for (int row = 0; row < height; ++row) for (int x = 0; x < width; ++x) {
            const int y = origin_top_left ? height - 1 - row : row;
            const bool covered = clear_depth > .25f && x >= 8 && x < 40 && y >= 32 && y < 48;
            const int texel = (y >= 40 ? 2 : 0) + (x >= 24 ? 1 : 0);
            for (int c = 0; c < 4; ++c) {
                const int expected = covered ? int(std::lround(texels[texel * 4 + c] * fragment_params.tint[c])) : 0;
                const int actual = pixels[(row * width + x) * 4 + c];
                if (std::abs(actual - expected) > 1) {
                    std::fprintf(stderr, "depth clear %g, pixel (%d,%d), channel %d: got %d expected %d\n",
                                 clear_depth, x, y, c, actual, expected);
                    throw std::runtime_error("color/coverage mismatch");
                }
            }
        }
    }
};
