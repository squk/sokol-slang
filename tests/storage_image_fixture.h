#pragma once
#include "test_support.h"
#include SHADER_HEADER
#include <array>
#include <cmath>
#include <cstdint>

struct StorageImageRecord { uint32_t value, marker; };
struct alignas(16) StorageImageParams { int32_t control[4]; };
static_assert(sizeof(StorageImageParams) == sizeof(storage_image_compute_params_t));

struct StorageImageFixture {
    static constexpr int width = 11, height = 9, count = width * height, guards = 17;
    static constexpr StorageImageRecord sentinel = {0xdeadc0de, 0xa5a5a5a5};
    sg_image colors = {}, counters = {};
    sg_buffer result = {};
    sg_pipeline pipeline = {};
    sg_bindings bindings = {};

    void init(sg_image_desc color_image = {}) {
        const auto* descriptor = storage_image_run_shader_desc(sg_query_backend());
        require(descriptor != nullptr, "storage image descriptor missing");
        sg_shader shader = sg_make_shader(descriptor);
        require(sg_query_shader_state(shader) == SG_RESOURCESTATE_VALID, "storage image shader creation failed");
        sg_pipeline_desc pipeline_desc = {};
        pipeline_desc.compute = true;
        pipeline_desc.shader = shader;
        pipeline = sg_make_pipeline(&pipeline_desc);
        require(sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID, "storage image pipeline creation failed");
        sg_image_desc image = color_image;
        image.width = width; image.height = height;
        image.usage.storage_image = true;
        image.pixel_format = SG_PIXELFORMAT_RGBA8;
        colors = sg_make_image(&image);
        image = {};
        image.width = width; image.height = height;
        image.usage.storage_image = true;
        image.pixel_format = SG_PIXELFORMAT_R32UI;
        counters = sg_make_image(&image);
        require(sg_query_image_state(colors) == SG_RESOURCESTATE_VALID &&
                sg_query_image_state(counters) == SG_RESOURCESTATE_VALID, "storage image creation failed");
        sg_view_desc view = {};
        view.storage_image.image = colors;
        bindings.views[VIEW_storage_image_compute_colors] = sg_make_view(&view);
        view.storage_image.image = counters;
        bindings.views[VIEW_storage_image_compute_counters] = sg_make_view(&view);
        std::array<StorageImageRecord, count + guards> records;
        records.fill(sentinel);
        sg_buffer_desc buffer = {};
        buffer.usage.storage_buffer = true;
        buffer.usage.copy_src = true;
        buffer.data = SG_RANGE(records);
        result = sg_make_buffer(&buffer);
        require(sg_query_buffer_state(result) == SG_RESOURCESTATE_VALID, "image result buffer creation failed");
        view = {};
        view.storage_buffer.buffer = result;
        bindings.views[VIEW_storage_image_compute_resultRecords] = sg_make_view(&view);
        for (const int slot : {VIEW_storage_image_compute_colors, VIEW_storage_image_compute_counters,
                               VIEW_storage_image_compute_resultRecords})
            require(sg_query_view_state(bindings.views[slot]) == SG_RESOURCESTATE_VALID, "storage image/buffer view creation failed");
    }

    void dispatch() {
        // Initialize, then read/modify twice. Later passes must observe image writes.
        for (int phase = 0; phase < 3; ++phase) {
            sg_pass pass = {};
            pass.compute = true;
            sg_begin_pass(&pass);
            sg_apply_pipeline(pipeline);
            sg_apply_bindings(&bindings);
            const StorageImageParams params = {{width, height, phase, 17}};
            const auto data = SG_RANGE(params);
            sg_apply_uniforms(UB_storage_image_compute_params, &data);
            sg_dispatch(2, 3, 1);
            sg_end_pass();
        }
    }

    void check_records(const StorageImageRecord* records) const {
        for (int i = 0; i < count + guards; ++i) {
            const uint32_t expected = i < count ? i * 3 + 7 + 17 : sentinel.value;
            const uint32_t marker = i < count ? expected + 17 : sentinel.marker;
            require(records[i].value == expected && records[i].marker == marker, "storage image read/write result or guard mismatch");
        }
    }

    void check_colors(const uint8_t* pixels, size_t row_bytes) const {
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int expected[] = {int(std::lround(x * 255.0 / 16)), int(std::lround(y * 255.0 / 16)), 128, 255};
            for (int c = 0; c < 4; ++c)
                require(std::abs(int(pixels[y * row_bytes + x * 4 + c]) - expected[c]) <= 1, "write-only image color mismatch");
        }
    }
};
