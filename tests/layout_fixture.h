#pragma once
#include "test_support.h"
#include "proof.h"
#include <cmath>
#include <cstddef>
#include <cstdio>

// This host layout is independent of the generated structs. The draw reads its
// fields on the GPU; static checks also compare the generated packing contract.
struct alignas(16) HostParams {
    float scale;
    float pad0[3];
    float bias[3];
    float pad1;
    float transform[16];
    float colors[2][4];
};
struct HostRecord { float offset[3]; float weight; float tint[4]; };
struct Vertex { float position[2]; uint32_t record; };
static_assert(sizeof(HostParams) == sizeof(proof_vertex_params_t));
static_assert(sizeof(HostParams) == sizeof(proof_fragment_params_t));
static_assert(offsetof(HostParams, bias) == offsetof(proof_vertex_params_t, bias));
static_assert(offsetof(HostParams, transform) == offsetof(proof_vertex_params_t, transform));
static_assert(offsetof(HostParams, colors) == offsetof(proof_vertex_params_t, colors));
static_assert(sizeof(HostRecord) == sizeof(proof_Record_std430_t));

struct LayoutFixture {
    static constexpr int width = 64, height = 32;
    sg_image color = {};

    void draw() {
        const auto* descriptor = proof_draw_shader_desc(sg_query_backend());
        require(descriptor != nullptr, "generated Metal platform descriptor missing");
        require(descriptor->uniform_blocks[0].msl_buffer_n == 0, "wrong vertex uniform binding");
        require(descriptor->uniform_blocks[1].msl_buffer_n == 1, "wrong fragment uniform binding");
        require(descriptor->views[0].storage_buffer.msl_buffer_n == 8, "wrong vertex storage binding");
        require(descriptor->views[2].storage_buffer.msl_buffer_n == 8, "wrong fragment storage binding");
        sg_shader shader = sg_make_shader(descriptor);
        require(sg_query_shader_state(shader) == SG_RESOURCESTATE_VALID, "native Metal shader creation failed");
        Vertex vertices[] = {{{99,99},0}, {{-.4f,-.4f},1}, {{.4f,-.4f},1},
                             {{.4f,.4f},1}, {{-.4f,.4f},1}};
        uint16_t indices[] = {99,99,99, 0,1,2, 0,2,3};
        HostRecord records[] = {{{99,99,99},99,{0,0,1,1}},
                                {{-.5f,0,0},.5f,{1,0,0,1}}, {{.5f,0,0},1,{0,1,0,1}}};
        sg_buffer_desc buffer = {};
        buffer.data = SG_RANGE(vertices);
        sg_buffer vb = sg_make_buffer(&buffer);
        buffer = {};
        buffer.usage.index_buffer = true;
        buffer.data = SG_RANGE(indices);
        sg_buffer ib = sg_make_buffer(&buffer);
        buffer = {};
        buffer.usage.storage_buffer = true;
        buffer.data = SG_RANGE(records);
        sg_buffer sb = sg_make_buffer(&buffer);
        sg_view_desc view = {};
        view.storage_buffer.buffer = sb;
        sg_view records_view = sg_make_view(&view);
        uint8_t pixel[] = {128,128,255,255};
        sg_image_desc image = {};
        image.width = image.height = 1;
        image.pixel_format = SG_PIXELFORMAT_RGBA8;
        image.data.mip_levels[0] = SG_RANGE(pixel);
        sg_image texture = sg_make_image(&image);
        view = {};
        view.texture.image = texture;
        sg_view texture_view = sg_make_view(&view);
        sg_sampler_desc sampler_desc = {};
        sampler_desc.min_filter = sampler_desc.mag_filter = SG_FILTER_LINEAR;
        sg_sampler sampler = sg_make_sampler(&sampler_desc);
        image = {};
        image.width = 64; image.height = 32;
        image.pixel_format = SG_PIXELFORMAT_RGBA32F;
        image.usage.color_attachment = true;
        color = sg_make_image(&image);
        view = {};
        view.color_attachment.image = color;
        sg_view color_view = sg_make_view(&view);
        sg_pipeline_desc pipeline_desc = {};
        pipeline_desc.shader = shader;
        pipeline_desc.index_type = SG_INDEXTYPE_UINT16;
        pipeline_desc.layout.buffers[0].stride = sizeof(Vertex);
        pipeline_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
        pipeline_desc.layout.attrs[1].format = SG_VERTEXFORMAT_UINT;
        pipeline_desc.layout.attrs[1].offset = offsetof(Vertex, record);
        pipeline_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA32F;
        pipeline_desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
        sg_pipeline pipeline = sg_make_pipeline(&pipeline_desc);
        require(sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID, "pipeline creation failed");
        HostParams params = {};
        params.scale = .75f;
        params.bias[0] = .03125f;
        params.bias[2] = .25f;
        params.transform[0] = params.transform[5] = params.transform[10] = params.transform[15] = 1;
        params.transform[4] = .125f;
        params.transform[13] = .125f;
        params.colors[0][2] = 99;
        params.colors[1][0] = .5f; params.colors[1][1] = 1;
        params.colors[1][2] = .25f; params.colors[1][3] = 1;
        sg_pass pass = {};
        pass.attachments.colors[0] = color_view;
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = {0,0,0,0};
        sg_begin_pass(&pass);
        sg_apply_pipeline(pipeline);
        sg_bindings draw = {};
        draw.vertex_buffers[0] = vb;
        draw.index_buffer = ib;
        draw.views[VIEW_proof_vertex_records] = records_view;
        draw.views[VIEW_proof_fragment_records] = records_view;
        draw.views[VIEW_proof_fragment_tex] = texture_view;
        draw.samplers[SMP_proof_fragment_smp] = sampler;
        sg_apply_bindings(&draw);
        auto uniforms = SG_RANGE(params);
        sg_apply_uniforms(UB_proof_vertex_params, &uniforms);
        sg_apply_uniforms(UB_proof_fragment_params, &uniforms);
        sg_draw_ex(3, 6, 2, 1, 4);
        sg_end_pass();
    }

    void check(const float* pixels) const {
        auto check_pixel = [&](int x, int y, float r, float g, float b, float a) {
            const float expected[] = {r,g,b,a};
            for (int c = 0; c < 4; ++c) {
                float actual = pixels[(y*64+x)*4+c];
                if (!std::isfinite(actual) || std::abs(actual-expected[c]) > 0.002f) {
                    std::fprintf(stderr, "pixel (%d,%d) channel %d: got %g expected %g\n", x,y,c,actual,expected[c]);
                    throw std::runtime_error("rendered values do not match host inputs");
                }
            }
        };
        check_pixel(16,14,128.f/255*.25f,0,0,.5f);
        check_pixel(48,14,0,128.f/255,0,1);
        check_pixel(32,14,0,0,0,0);
        check_pixel(16,30,0,0,0,0);
    }
};
