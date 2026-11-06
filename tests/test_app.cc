#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "readback.h"
#include <atomic>
#include <cstdlib>

#if defined(TEST_COMPUTE)
#include "compute_fixture.h"
static ComputeFixture fixture;
#elif defined(TEST_STORAGE_IMAGE)
#include "storage_image_fixture.h"
static StorageImageFixture fixture;
#elif defined(TEST_LAYOUT)
#include "layout_fixture.h"
static LayoutFixture fixture;
#else
#if defined(TEST_DEPTH)
#include "depth_fixture.h"
using RasterFixture = DepthFixture;
static constexpr float raster_depths[] = {.25f, .75f};
#else
#include "portable_fixture.h"
using RasterFixture = PortableFixture;
static constexpr float raster_depths[] = {.5f, .2f};
#endif
static RasterFixture fixture;
static size_t draw_index;
#endif

static bool pending, done, finishing;
static int result;
static std::atomic<int> validation_errors;

static void fail_test(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    result = 1;
    done = true;
}

static void log_message(const char *tag, uint32_t level, uint32_t item, const char *message,
                        uint32_t line, const char *file, void *user) {
    if (level <= 1)
        ++validation_errors;
    slog_func(tag, level, item, message, line, file, user);
}

static void cleanup() {
    if (!done)
        fail_test("application closed before checks finished");
    if (sg_isvalid())
        sg_shutdown();
    cleanup_readbacks();
#if defined(__EMSCRIPTEN__)
    EM_ASM({ Module['reportTestResult']($0); }, result);
#else
    if (result)
        std::exit(result);
#endif
}

static void finish() {
    if (validation_errors)
        fail_test("Sokol or backend validation errors");
    if (!result)
        std::puts("PASS: Sokol shader execution and GPU readback");
    sapp_quit();
}

static void finish_validation() {
    finishing = true;
#if defined(SOKOL_WGPU)
    WGPUPopErrorScopeCallbackInfo callback = {};
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type,
                           WGPUStringView message, void *, void *) {
        if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError)
            fail_test(("WebGPU validation: " + readback_message(message)).c_str());
        finish();
    };
    wgpuDevicePopErrorScope(readback_device(), callback);
#else
    finish();
#endif
}

static void init() {
    try {
        sg_desc setup = {};
        setup.environment = sglue_environment();
        setup.logger.func = log_message;
        sg_setup(&setup);
        require(sg_isvalid(), "Sokol setup failed");
#if defined(SOKOL_WGPU)
        wgpuDevicePushErrorScope(readback_device(), WGPUErrorFilter_Validation);
#endif
#if defined(TEST_COMPUTE)
        fixture.init();
#elif defined(TEST_STORAGE_IMAGE)
        fixture.init(readback_target(fixture.width, fixture.height, true));
#elif !defined(TEST_LAYOUT)
        fixture.init(readback_target(fixture.width, fixture.height));
#endif
    } catch (const std::exception &error) {
        fail_test(error.what());
    }
}

static void submit_frame() {
    sg_pass pass = {};
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    sg_end_pass();
    sg_commit();
}

static void frame() {
    if (finishing)
        return;
    if (done) {
        if (!result)
            submit_frame();
        finish_validation();
        return;
    }
    if (pending) {
        submit_frame();
        return;
    }
    pending = true;
    try {
#if defined(TEST_COMPUTE)
        fixture.dispatch();
        submit_frame();
        read_buffer(fixture.result, fixture.count * sizeof(ComputeRecord),
                    [](const uint8_t *bytes, size_t) {
                        fixture.check(reinterpret_cast<const ComputeRecord *>(bytes));
                        done = true;
                    });
#elif defined(TEST_STORAGE_IMAGE)
        fixture.dispatch();
        submit_frame();
        read_buffer(fixture.result, (fixture.count + fixture.guards) * sizeof(StorageImageRecord),
                    [](const uint8_t *bytes, size_t) {
                        fixture.check_records(reinterpret_cast<const StorageImageRecord *>(bytes));
                        read_image(fixture.colors, fixture.width, fixture.height,
                                   SG_PIXELFORMAT_RGBA8,
                                   [](const uint8_t *pixels, size_t row_bytes) {
                                       fixture.check_colors(pixels, row_bytes);
                                       done = true;
                                   });
                    });
#elif defined(TEST_LAYOUT)
        fixture.draw();
        submit_frame();
        read_image(fixture.color, fixture.width, fixture.height, SG_PIXELFORMAT_RGBA32F,
                   [](const uint8_t *bytes, size_t) {
                       fixture.check(reinterpret_cast<const float *>(bytes));
                       done = true;
                   });
#else
        const float depth = raster_depths[draw_index];
        fixture.begin_draw(depth);
#if defined(TEST_DEPTH)
        std::vector<float> depths;
#else
        auto depths = read_depth(fixture.width, fixture.height);
#endif
        sg_end_pass();
        submit_frame();
        const auto image = sg_query_view_desc(fixture.color_view).color_attachment.image;
        read_image(image, fixture.width, fixture.height, SG_PIXELFORMAT_RGBA8,
                   [depth, depths = std::move(depths)](const uint8_t *pixels, size_t) {
                       fixture.check_color(pixels, depth, readback_top_left());
#if !defined(TEST_DEPTH)
                       if (!depths.empty())
                           fixture.check_depth(depths.data(), depth);
#endif
                       done = ++draw_index == sizeof(raster_depths) / sizeof(raster_depths[0]);
                       pending = false;
                   });
#endif
    } catch (const std::exception &error) {
        fail_test(error.what());
    }
}

sapp_desc sokol_main(int, char **) {
    sapp_desc app = {};
    app.init_cb = init;
    app.frame_cb = frame;
    app.cleanup_cb = cleanup;
    app.width = app.height = 64;
    app.sample_count = 1;
    app.window_title = "sokol-slang tests";
    app.logger.func = log_message;
    app.win32.console_attach = true;
#if defined(SOKOL_GLES3) && !defined(__EMSCRIPTEN__)
    app.gl.major_version = 3;
    app.gl.minor_version = 1;
#endif
    return app;
}
