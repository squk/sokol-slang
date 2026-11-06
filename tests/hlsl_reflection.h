#pragma once
#include "d3d11_support.h"
#define SOKOL_D3D11
#include "sokol_gfx.h"
#include SHADER_HEADER
#include <cstring>
#include <d3d11shader.h>
#include <d3dcompiler.h>

struct HlslReflection {
    ComOwner<ID3DBlob> bytecode;
    ComOwner<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC shader = {};

    explicit HlslReflection(const sg_shader_function &function) {
        if (function.bytecode.ptr) {
            require(function.source == nullptr, "DXBC descriptor also contains source");
            check_hr(D3DReflect(function.bytecode.ptr, function.bytecode.size,
                                IID_ID3D11ShaderReflection,
                                reinterpret_cast<void **>(&reflection.value)),
                     "D3DReflect");
            check_hr(reflection.value->GetDesc(&shader), "shader reflection");
            return;
        }
        ComOwner<ID3DBlob> diagnostics;
        const HRESULT compiled =
            D3DCompile(function.source, std::strlen(function.source), SHADER_HEADER, nullptr,
                       nullptr, function.entry, function.d3d11_target,
                       D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                       &bytecode.value, &diagnostics.value);
        if (diagnostics.value)
            std::fprintf(stderr, "%s\n",
                         static_cast<const char *>(diagnostics.value->GetBufferPointer()));
        check_hr(compiled, "D3DCompile");
        require(bytecode.value != nullptr, "DXBC bytecode missing");
        check_hr(D3DReflect(bytecode.value->GetBufferPointer(), bytecode.value->GetBufferSize(),
                            IID_ID3D11ShaderReflection,
                            reinterpret_cast<void **>(&reflection.value)),
                 "D3DReflect");
        check_hr(reflection.value->GetDesc(&shader), "shader reflection");
    }
};
