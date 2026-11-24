// Dispatch a compute shader into an image, then sample the image in a render pass.
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "compute_image.h"

// Use a size that requires a partial workgroup at each edge.
enum { IMAGE_SIZE = 250, GROUP_SIZE = 8, GROUP_COUNT = (IMAGE_SIZE + GROUP_SIZE - 1) / GROUP_SIZE };

static struct {
    sg_pipeline compute_pipeline;
    sg_pipeline display_pipeline;
    sg_bindings compute_bindings;
    sg_bindings display_bindings;
    float time;
} state;

static void init(void) {
    sg_setup(&(sg_desc){.environment = sglue_environment(), .logger.func = slog_func});

    const float triangle[][2] = {{0, 0}, {2, 0}, {0, 2}};
    state.display_bindings.vertex_buffers[0] =
        sg_make_buffer(&(sg_buffer_desc){.data = SG_RANGE(triangle)});

    const sg_image image = sg_make_image(&(sg_image_desc){
        .usage.storage_image = true,
        .width = IMAGE_SIZE,
        .height = IMAGE_SIZE,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
    });
    state.compute_bindings.views[VIEW_compute_image_generate_compute_outputImage] =
        sg_make_view(&(sg_view_desc){.storage_image.image = image});
    state.display_bindings.views[VIEW_compute_image_display_fragment_displayImage] =
        sg_make_view(&(sg_view_desc){.texture.image = image});
    state.display_bindings.samplers[SMP_compute_image_display_fragment_displaySampler] =
        sg_make_sampler(&(sg_sampler_desc){
            .min_filter = SG_FILTER_LINEAR,
            .mag_filter = SG_FILTER_LINEAR,
            .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
            .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        });

    const sg_backend backend = sg_query_backend();
    state.compute_pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .compute = true,
        .shader = sg_make_shader(compute_image_generate_shader_desc(backend)),
    });
    state.display_pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(compute_image_display_shader_desc(backend)),
        .layout.attrs[ATTR_compute_image_display_corner].format = SG_VERTEXFORMAT_FLOAT2,
    });
}

static void frame(void) {
    const int width = sapp_width();
    const int height = sapp_height();
    const int size = width < height ? width : height;
    if (size <= 0) {
        return;
    }
    state.time += (float)sapp_frame_duration();
    const compute_image_generate_compute_params_t params = {.time = state.time};

    sg_begin_pass(&(sg_pass){.compute = true});
    sg_apply_pipeline(state.compute_pipeline);
    sg_apply_bindings(&state.compute_bindings);
    sg_apply_uniforms(UB_compute_image_generate_compute_params, &SG_RANGE(params));
    sg_dispatch(GROUP_COUNT, GROUP_COUNT, 1);
    sg_end_pass();

    sg_begin_pass(&(sg_pass){
        .action.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                             .clear_value = {0.04f, 0.05f, 0.09f, 1.0f}},
        .swapchain = sglue_swapchain(),
    });
    sg_apply_viewport((width - size) / 2, (height - size) / 2, size, size, true);
    sg_apply_pipeline(state.display_pipeline);
    sg_apply_bindings(&state.display_bindings);
    sg_draw(0, 3, 1);
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
        .window_title = "Compute Image (Sokol + Slang)",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
