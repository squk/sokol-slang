// A complete Sokol application rendering a colored triangle with a Slang shader.
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "triangle.h"
#include <stddef.h>

static struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action pass_action;
} state;

typedef struct {
    float x, y;
    float r, g, b, a;
} vertex_t;

static void init(void) {
    sg_desc setup = {0};
    setup.environment = sglue_environment();
    setup.logger.func = slog_func;
    sg_setup(&setup);

    // 2D position (x, y) and RGBA color for 3 triangle vertices
    const vertex_t vertices[] = {
        {  0.0f,  0.5f,  1.0f, 0.0f, 0.0f, 1.0f }, // Top (red)
        {  0.5f, -0.5f,  0.0f, 1.0f, 0.0f, 1.0f }, // Bottom right (green)
        { -0.5f, -0.5f,  0.0f, 0.0f, 1.0f, 1.0f }, // Bottom left (blue)
    };

    sg_buffer_desc vbuf_desc = {0};
    vbuf_desc.data = SG_RANGE(vertices);
    vbuf_desc.label = "triangle-vertices";
    state.bind.vertex_buffers[0] = sg_make_buffer(&vbuf_desc);

    // Create shader from the generated sokol-slang descriptor function
    const sg_shader_desc* shd_desc = triangle_draw_shader_desc(sg_query_backend());
    sg_shader shd = sg_make_shader(shd_desc);

    // Create pipeline with attribute layout matching the Slang vertex inputs
    sg_pipeline_desc pip_desc = {0};
    pip_desc.shader = shd;
    pip_desc.layout.buffers[0].stride = sizeof(vertex_t);
    pip_desc.layout.attrs[ATTR_triangle_draw_input_position].format = SG_VERTEXFORMAT_FLOAT2;
    pip_desc.layout.attrs[ATTR_triangle_draw_input_position].offset = offsetof(vertex_t, x);
    pip_desc.layout.attrs[ATTR_triangle_draw_input_color].format = SG_VERTEXFORMAT_FLOAT4;
    pip_desc.layout.attrs[ATTR_triangle_draw_input_color].offset = offsetof(vertex_t, r);
    pip_desc.label = "triangle-pipeline";
    state.pip = sg_make_pipeline(&pip_desc);

    // Clear color pass action (dark blue-gray background)
    state.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    state.pass_action.colors[0].clear_value.r = 0.1f;
    state.pass_action.colors[0].clear_value.g = 0.1f;
    state.pass_action.colors[0].clear_value.b = 0.15f;
    state.pass_action.colors[0].clear_value.a = 1.0f;
}

static void frame(void) {
    sg_pass pass = {0};
    pass.action = state.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    sg_apply_pipeline(state.pip);
    sg_apply_bindings(&state.bind);
    sg_draw(0, 3, 1);
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
    desc.window_title = "Triangle (Sokol + Slang)";
    desc.icon.sokol_default = true;
    desc.logger.func = slog_func;
    return desc;
}
