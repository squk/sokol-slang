// Draw a grid of rotating tiles with one mesh and a per-instance vertex buffer.
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include <stddef.h>

#include "instancing.h"

enum { COLS = 9, ROWS = 7 };

typedef struct {
    float center[2];
    float color[3];
} instance_t;

static struct {
    sg_pipeline pipeline;
    sg_bindings bindings;
    float time;
} state;

static void init(void) {
    sg_setup(&(sg_desc){.environment = sglue_environment(), .logger.func = slog_func});

    const float quad[][2] = {
        {-0.06f, -0.06f},
        {0.06f, -0.06f},
        {-0.06f, 0.06f},
        {0.06f, 0.06f},
    };
    state.bindings.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){.data = SG_RANGE(quad)});

    instance_t instances[ROWS][COLS];
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            const float x = (float)col / (COLS - 1);
            const float y = (float)row / (ROWS - 1);
            instances[row][col] = (instance_t){
                .center = {(x - 0.5f) * 1.6f, (y - 0.5f) * 1.2f},
                .color = {0.18f + 0.75f * x, 0.35f + 0.6f * y, 0.9f - 0.45f * x},
            };
        }
    }
    state.bindings.vertex_buffers[1] =
        sg_make_buffer(&(sg_buffer_desc){.data = SG_RANGE(instances)});

    state.pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(instancing_draw_shader_desc(sg_query_backend())),
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP,
        .layout.buffers[0].stride = sizeof(quad[0]),
        .layout.buffers[1] = {.stride = sizeof(instance_t),
                              .step_func = SG_VERTEXSTEP_PER_INSTANCE},
        .layout.attrs[ATTR_instancing_draw_input_position].format = SG_VERTEXFORMAT_FLOAT2,
        .layout.attrs[ATTR_instancing_draw_input_center] =
            {
                .buffer_index = 1,
                .format = SG_VERTEXFORMAT_FLOAT2,
                .offset = offsetof(instance_t, center),
            },
        .layout.attrs[ATTR_instancing_draw_input_color] =
            {
                .buffer_index = 1,
                .format = SG_VERTEXFORMAT_FLOAT3,
                .offset = offsetof(instance_t, color),
            },
    });
}

static void frame(void) {
    const float width = sapp_widthf();
    const float height = sapp_heightf();
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    state.time += (float)sapp_frame_duration();
    const instancing_vertex_params_t params = {
        .scale = {width > height ? height / width : 1.0f, height > width ? width / height : 1.0f},
        .time = state.time,
    };

    sg_begin_pass(&(sg_pass){
        .action.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                             .clear_value = {0.04f, 0.05f, 0.09f, 1.0f}},
        .swapchain = sglue_swapchain(),
    });
    sg_apply_pipeline(state.pipeline);
    sg_apply_bindings(&state.bindings);
    sg_apply_uniforms(UB_instancing_vertex_params, &SG_RANGE(params));
    sg_draw(0, 4, ROWS * COLS);
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) { sg_shutdown(); }

sapp_desc sokol_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .width = 640,
        .height = 480,
        .window_title = "Instancing (Sokol + Slang)",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
