// Indexed drawing, a sampled texture, and a generated uniform buffer struct.
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "textured_quad.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float x, y;
    float u, v;
} vertex_t;

static struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action pass_action;
} state;

static void init(void) {
    sg_desc setup = {0};
    setup.environment = sglue_environment();
    setup.logger.func = slog_func;
    sg_setup(&setup);

    const vertex_t vertices[] = {
        {-0.75f,  0.75f, 0.0f, 0.0f},
        { 0.75f,  0.75f, 1.0f, 0.0f},
        { 0.75f, -0.75f, 1.0f, 1.0f},
        {-0.75f, -0.75f, 0.0f, 1.0f},
    };
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    sg_buffer_desc vertices_desc = {0};
    vertices_desc.data = SG_RANGE(vertices);
    vertices_desc.label = "quad-vertices";
    state.bind.vertex_buffers[0] = sg_make_buffer(&vertices_desc);

    sg_buffer_desc indices_desc = {0};
    indices_desc.usage.index_buffer = true;
    indices_desc.data = SG_RANGE(indices);
    indices_desc.label = "quad-indices";
    state.bind.index_buffer = sg_make_buffer(&indices_desc);

    // Generate RGBA bytes directly so the example needs no image loader.
    uint8_t pixels[64][64][4];
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const bool light = ((x / 8) + (y / 8)) % 2 == 0;
            pixels[y][x][0] = light ? 240 : 40;
            pixels[y][x][1] = light ? 190 : 70;
            pixels[y][x][2] = light ? 80 : 110;
            pixels[y][x][3] = 255;
        }
    }
    sg_image_desc image_desc = {0};
    image_desc.width = image_desc.height = 64;
    image_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    image_desc.data.mip_levels[0] = SG_RANGE(pixels);
    image_desc.label = "checkerboard";
    sg_image image = sg_make_image(&image_desc);

    sg_view_desc view_desc = {0};
    view_desc.texture.image = image;
    state.bind.views[VIEW_textured_quad_fragment_tex] = sg_make_view(&view_desc);

    sg_sampler_desc sampler_desc = {0};
    sampler_desc.min_filter = sampler_desc.mag_filter = SG_FILTER_NEAREST;
    sampler_desc.wrap_u = sampler_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    state.bind.samplers[SMP_textured_quad_fragment_smp] = sg_make_sampler(&sampler_desc);

    sg_pipeline_desc pip_desc = {0};
    pip_desc.shader = sg_make_shader(textured_quad_draw_shader_desc(sg_query_backend()));
    pip_desc.index_type = SG_INDEXTYPE_UINT16;
    pip_desc.layout.buffers[0].stride = sizeof(vertex_t);
    pip_desc.layout.attrs[ATTR_textured_quad_draw_input_position].format = SG_VERTEXFORMAT_FLOAT2;
    pip_desc.layout.attrs[ATTR_textured_quad_draw_input_position].offset = offsetof(vertex_t, x);
    pip_desc.layout.attrs[ATTR_textured_quad_draw_input_uv].format = SG_VERTEXFORMAT_FLOAT2;
    pip_desc.layout.attrs[ATTR_textured_quad_draw_input_uv].offset = offsetof(vertex_t, u);
    pip_desc.label = "textured-quad-pipeline";
    state.pip = sg_make_pipeline(&pip_desc);

    state.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    state.pass_action.colors[0].clear_value = (sg_color){0.1f, 0.1f, 0.15f, 1.0f};
}

static void frame(void) {
    const float width = sapp_widthf();
    const float height = sapp_heightf();
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    // Clip space spans the window; scale the longer axis to keep a square.
    textured_quad_vertex_params_t params = {0};
    params.scale[0] = width > height ? height / width : 1.0f;
    params.scale[1] = height > width ? width / height : 1.0f;
    const sg_range uniforms = SG_RANGE(params);

    sg_pass pass = {0};
    pass.action = state.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    sg_apply_pipeline(state.pip);
    sg_apply_bindings(&state.bind);
    sg_apply_uniforms(UB_textured_quad_vertex_params, &uniforms);
    sg_draw(0, 6, 1);
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    sapp_desc desc = {0};
    desc.init_cb = init;
    desc.frame_cb = frame;
    desc.cleanup_cb = cleanup;
    desc.width = 640;
    desc.height = 480;
    desc.window_title = "Textured Quad (Sokol + Slang)";
    desc.icon.sokol_default = true;
    desc.logger.func = slog_func;
    return desc;
}
