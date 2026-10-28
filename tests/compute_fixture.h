#pragma once
#include "test_support.h"
#include SHADER_HEADER
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

struct ComputeRecord { uint32_t value, marker; };
struct alignas(16) ComputeParams { int32_t control[4]; };
static_assert(sizeof(ComputeParams) == sizeof(compute_compute_params_t));

struct ComputeFixture {
    static constexpr int count = 137, written = 113;
    static constexpr ComputeRecord sentinel = {0xdeadc0de, 0xa5a5a5a5};
    sg_buffer result = {};
    sg_pipeline pipeline = {};
    sg_bindings bindings = {};

    void init() {
        require(sg_query_features().compute, "backend does not support compute");
        auto descriptor = compute_run_shader_desc(sg_query_backend());
        require(descriptor != nullptr, "compute descriptor missing");
        sg_shader shader = sg_make_shader(descriptor);
        require(sg_query_shader_state(shader) == SG_RESOURCESTATE_VALID, "compute shader creation failed");
        sg_pipeline_desc pipeline_desc = {};
        pipeline_desc.compute = true;
        pipeline_desc.shader = shader;
        pipeline = sg_make_pipeline(&pipeline_desc);
        require(sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID, "compute pipeline creation failed");

        std::array<ComputeRecord, count> input, output;
        for (int i = 0; i < count; ++i) {
            input[i] = {uint32_t(i + 7), 0xf00d0000u + uint32_t(i)};
            output[i] = sentinel;
        }
        sg_buffer_desc buffer_desc = {};
        buffer_desc.usage.storage_buffer = true;
        buffer_desc.data = SG_RANGE(input);
        sg_buffer source = sg_make_buffer(&buffer_desc);
        buffer_desc.usage.copy_src = true;
        buffer_desc.data = SG_RANGE(output);
        result = sg_make_buffer(&buffer_desc);
        require(sg_query_buffer_state(source) == SG_RESOURCESTATE_VALID &&
                sg_query_buffer_state(result) == SG_RESOURCESTATE_VALID, "compute buffer creation failed");
        sg_view_desc view = {};
        view.storage_buffer.buffer = source;
        sg_view source_view = sg_make_view(&view);
        view.storage_buffer.buffer = result;
        sg_view result_view = sg_make_view(&view);
        require(sg_query_view_state(source_view) == SG_RESOURCESTATE_VALID &&
                sg_query_view_state(result_view) == SG_RESOURCESTATE_VALID, "storage buffer view creation failed");
        bindings.views[VIEW_compute_compute_sourceRecords] = source_view;
        bindings.views[VIEW_compute_compute_resultRecords] = result_view;
    }

    void dispatch() {
        sg_pass pass = {};
        pass.compute = true;
        sg_begin_pass(&pass);
        sg_apply_pipeline(pipeline);
        sg_apply_bindings(&bindings);
        const ComputeParams params = {{written, 3, 17, 0}};
        const sg_range uniforms = SG_RANGE(params);
        sg_apply_uniforms(UB_compute_compute_params, &uniforms);
        sg_dispatch(2, 1, 1);
        sg_end_pass();
    }

    void check(const ComputeRecord* records) const {
        for (int i = 0; i < count; ++i) {
            const uint32_t value = i < written ? uint32_t((i + 7) * 3 + 17) : sentinel.value;
            const uint32_t marker = i < written ? (0xf00d0000u + uint32_t(i)) ^ uint32_t(i % 64 + ((i / 64) << 16)) : sentinel.marker;
            if (records[i].value != value || records[i].marker != marker) {
                std::fprintf(stderr, "record %d: got %u/%u expected %u/%u\n", i, records[i].value, records[i].marker, value, marker);
                throw std::runtime_error("compute result or guard record mismatch");
            }
        }
    }
};
