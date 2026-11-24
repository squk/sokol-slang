// Render into an image, then show it with nearest and linear filtering.
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "render_to_texture.h"

enum { IMAGE_SIZE = 40, GAP = 16 };

static struct {
    sg_pipeline pattern_pipeline;
    sg_pipeline display_pipeline;
    sg_view attachment;
    sg_bindings pattern_bindings;
    sg_bindings display_bindings;
    sg_sampler nearest_sampler;
    sg_sampler linear_sampler;
    float time;
} state;

static void init(void) {
    sg_setup(&(sg_desc){.environment = sglue_environment(), .logger.func = slog_func});

    const float triangle[][2] = {{0, 0}, {2, 0}, {0, 2}};
    const sg_buffer vertices = sg_make_buffer(&(sg_buffer_desc){.data = SG_RANGE(triangle)});
    state.pattern_bindings.vertex_buffers[0] = vertices;
    state.display_bindings.vertex_buffers[0] = vertices;

    const sg_image image = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = IMAGE_SIZE,
        .height = IMAGE_SIZE,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .sample_count = 1,
    });
    // The same image needs a view for each role.
    state.attachment = sg_make_view(&(sg_view_desc){.color_attachment.image = image});
    state.display_bindings.views[VIEW_render_to_texture_display_fragment_pattern] =
        sg_make_view(&(sg_view_desc){.texture.image = image});
    state.nearest_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    state.linear_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

    const sg_backend backend = sg_query_backend();
    state.pattern_pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(render_to_texture_pattern_shader_desc(backend)),
        .layout.attrs[ATTR_render_to_texture_pattern_corner].format = SG_VERTEXFORMAT_FLOAT2,
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
    });
    state.display_pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(render_to_texture_display_shader_desc(backend)),
        .layout.attrs[ATTR_render_to_texture_display_corner].format = SG_VERTEXFORMAT_FLOAT2,
    });
}

static void draw_preview(int x, int y, int size, sg_sampler sampler) {
    sg_apply_viewport(x, y, size, size, true);
    state.display_bindings.samplers[SMP_render_to_texture_display_fragment_filterSampler] = sampler;
    sg_apply_bindings(&state.display_bindings);
    sg_draw(0, 3, 1);
}

static void frame(void) {
    const int width = sapp_width();
    const int height = sapp_height();
    const int half_width = (width - 3 * GAP) / 2;
    const int inner_height = height - 2 * GAP;
    const int size = half_width < inner_height ? half_width : inner_height;
    if (size <= 0) {
        return;
    }
    state.time += (float)sapp_frame_duration() * 0.65f;
    const render_to_texture_pattern_fragment_params_t params = {.time = state.time};

    // Store the offscreen pixels so the swapchain pass can sample them.
    sg_begin_pass(&(sg_pass){
        .attachments.colors[0] = state.attachment,
        .action.colors[0] = {.load_action = SG_LOADACTION_DONTCARE,
                             .store_action = SG_STOREACTION_STORE},
    });
    sg_apply_pipeline(state.pattern_pipeline);
    sg_apply_bindings(&state.pattern_bindings);
    sg_apply_uniforms(UB_render_to_texture_pattern_fragment_params, &SG_RANGE(params));
    sg_draw(0, 3, 1);
    sg_end_pass();

    sg_begin_pass(&(sg_pass){
        .action.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                             .clear_value = {0.04f, 0.05f, 0.09f, 1.0f}},
        .swapchain = sglue_swapchain(),
    });
    sg_apply_pipeline(state.display_pipeline);
    const int x = (width - (2 * size + GAP)) / 2;
    const int y = (height - size) / 2;
    draw_preview(x, y, size, state.nearest_sampler);
    draw_preview(x + size + GAP, y, size, state.linear_sampler);
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
        .width = 720,
        .height = 420,
        .window_title = "Render to Texture (Sokol + Slang)",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
