// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <boost/range/algorithm/fill.hpp>
#include "common/bit_field.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/pica.h"
#include "video_core/pica_state.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_interpreter.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/shader/shader_jit_x64.h"
#endif // ARCHITECTURE_x86_64
#include "video_core/video_core.h"

namespace Pica {

namespace Shader {

OutputVertex OutputRegisters::ToVertex(const Regs::ShaderConfig& config) {
    // Setup output data
    OutputVertex ret;
    // TODO(neobrain): Under some circumstances, up to 16 attributes may be output. We need to
    // figure out what those circumstances are and enable the remaining outputs then.
    unsigned index = 0;
    for (unsigned i = 0; i < 7; ++i) {

        if (index >= g_state.regs.vs_output_total)
            break;

        if ((config.output_mask & (1 << i)) == 0)
            continue;

        const auto& output_register_map = g_state.regs.vs_output_attributes[index];

        u32 semantics[4] = {output_register_map.map_x, output_register_map.map_y,
                            output_register_map.map_z, output_register_map.map_w};

        for (unsigned comp = 0; comp < 4; ++comp) {
            float24* out = ((float24*)&ret) + semantics[comp];
            if (semantics[comp] != Regs::VSOutputAttributes::INVALID) {
                *out = value[i][comp];
            } else {
                // Zero output so that attributes which aren't output won't have denormals in them,
                // which would slow us down later.
                memset(out, 0, sizeof(*out));
            }
        }

        index++;
    }

    // The hardware takes the absolute and saturates vertex colors like this, *before* doing
    // interpolation
    for (unsigned i = 0; i < 4; ++i) {
        ret.color[i] = float24::FromFloat32(std::fmin(std::fabs(ret.color[i].ToFloat32()), 1.0f));
    }

    LOG_TRACE(HW_GPU, "Output vertex: pos(%.2f, %.2f, %.2f, %.2f), quat(%.2f, %.2f, %.2f, %.2f), "
                      "col(%.2f, %.2f, %.2f, %.2f), tc0(%.2f, %.2f), view(%.2f, %.2f, %.2f)",
              ret.pos.x.ToFloat32(), ret.pos.y.ToFloat32(), ret.pos.z.ToFloat32(),
              ret.pos.w.ToFloat32(), ret.quat.x.ToFloat32(), ret.quat.y.ToFloat32(),
              ret.quat.z.ToFloat32(), ret.quat.w.ToFloat32(), ret.color.x.ToFloat32(),
              ret.color.y.ToFloat32(), ret.color.z.ToFloat32(), ret.color.w.ToFloat32(),
              ret.tc0.u().ToFloat32(), ret.tc0.v().ToFloat32(), ret.view.x.ToFloat32(),
              ret.view.y.ToFloat32(), ret.view.z.ToFloat32());

    return ret;
}

#ifdef ARCHITECTURE_x86_64
static std::unordered_map<u64, std::shared_ptr<JitShader>> shader_map;
#endif // ARCHITECTURE_x86_64

void ClearCache() {
#ifdef ARCHITECTURE_x86_64
    shader_map.clear();
#endif // ARCHITECTURE_x86_64
}

void ShaderSetup::Setup() {
#ifdef ARCHITECTURE_x86_64
    if (VideoCore::g_shader_jit_enabled) {
        u64 cache_key = (Common::ComputeHash64(&program_code, sizeof(program_code)) ^
                         Common::ComputeHash64(&swizzle_data, sizeof(swizzle_data)));

        auto iter = shader_map.find(cache_key);
        if (iter != shader_map.end()) {
            jit_shader = iter->second;
        } else {
            auto shader = std::make_shared<JitShader>();
            shader->Compile(*this);
            jit_shader = shader;
            shader_map[cache_key] = std::move(shader);
        }
    } else {
        jit_shader.reset();
    }
#endif // ARCHITECTURE_x86_64
}

MICROPROFILE_DEFINE(GPU_Shader, "GPU", "Shader", MP_RGB(50, 50, 240));

void ShaderSetup::Run(UnitState<false>& state, const InputVertex& input, int num_attributes,
                      const Regs::ShaderConfig& config) {

    MICROPROFILE_SCOPE(GPU_Shader);

    state.debug.max_offset = 0;
    state.debug.max_opdesc_id = 0;

    // Setup input register table
    const auto& attribute_register_map = config.input_register_map;

    for (unsigned i = 0; i < num_attributes; i++)
        state.registers.input[attribute_register_map.GetRegisterForAttribute(i)] = input.attr[i];

    state.conditional_code[0] = false;
    state.conditional_code[1] = false;

#ifdef ARCHITECTURE_x86_64
    if (auto shader = jit_shader.lock())
        shader.get()->Run(*this, state, config.main_offset);
    else
        RunInterpreter(*this, state, config.main_offset);
#else
    RunInterpreter(*this, state, config.main_offset);
#endif // ARCHITECTURE_x86_64
}

DebugData<true> ShaderSetup::ProduceDebugInfo(const InputVertex& input, int num_attributes,
                                              const Regs::ShaderConfig& config) {
    UnitState<true> state;

    state.debug.max_offset = 0;
    state.debug.max_opdesc_id = 0;

    // Setup input register table
    boost::fill(state.registers.input, Math::Vec4<float24>::AssignToAll(float24::Zero()));
    const auto& attribute_register_map = config.input_register_map;
    for (unsigned i = 0; i < num_attributes; i++)
        state.registers.input[attribute_register_map.GetRegisterForAttribute(i)] = input.attr[i];

    state.conditional_code[0] = false;
    state.conditional_code[1] = false;

    RunInterpreter(*this, state, config.main_offset);
    return state.debug;
}

bool SharedGS() {
    return g_state.regs.vs_com_mode == Pica::Regs::VSComMode::Shared;
}

bool UseGS() {
    // TODO(ds84182): This would be more accurate if it looked at induvidual
    // shader units for the geoshader bit
    // gs_regs.input_buffer_config.use_geometry_shader == 0x08
    ASSERT((g_state.regs.using_geometry_shader == 0) || (g_state.regs.using_geometry_shader == 2));
    return g_state.regs.using_geometry_shader == 2;
}

UnitState<false>& GetShaderUnit(bool gs) {

    // GS are always run on shader unit 3
    if (gs) {
        return g_state.shader_units[3];
    }

    // The worst scheduler you'll ever see!
    // TODO: How does PICA shader scheduling work?
    static unsigned shader_unit_scheduler = 0;
    shader_unit_scheduler++;
    shader_unit_scheduler %= 3; // TODO: When does it also allow use of unit 3?!
    return g_state.shader_units[shader_unit_scheduler];
}

void WriteUniformBoolReg(bool gs, u32 value) {
    auto& setup = gs ? g_state.gs : g_state.vs;

    ASSERT(setup.uniforms.b.size() == 16);
    for (unsigned i = 0; i < 16; ++i)
        setup.uniforms.b[i] = (value & (1 << i)) != 0;

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteUniformBoolReg(true, value);
    }
}

void WriteUniformIntReg(bool gs, unsigned index, const Math::Vec4<u8>& values) {
    const char* shader_type = gs ? "GS" : "VS";
    auto& setup = gs ? g_state.gs : g_state.vs;

    ASSERT(index < setup.uniforms.i.size());
    setup.uniforms.i[index] = values;
    LOG_TRACE(HW_GPU, "Set %s integer uniform %d to %02x %02x %02x %02x", shader_type, index,
              values.x.Value(), values.y.Value(), values.z.Value(), values.w.Value());

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteUniformIntReg(true, index, values);
    }
}

void WriteUniformFloatSetupReg(bool gs, u32 value) {
    auto& config = gs ? g_state.regs.gs : g_state.regs.vs;

    config.uniform_setup.setup = value;

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteUniformFloatSetupReg(true, value);
    }
}

void WriteUniformFloatReg(bool gs, u32 value) {
    const char* shader_type = gs ? "GS" : "VS";
    auto& config = gs ? g_state.regs.gs : g_state.regs.vs;
    auto& setup = gs ? g_state.gs : g_state.vs;

    auto& uniform_setup = config.uniform_setup;
    auto& uniform_write_buffer = setup.uniform_write_buffer;
    auto& float_regs_counter = setup.float_regs_counter;

    // TODO: Does actual hardware indeed keep an intermediate buffer or does
    //       it directly write the values?
    uniform_write_buffer[float_regs_counter++] = value;

    // Uniforms are written in a packed format such that four float24 values are
    // encoded in
    // three 32-bit numbers. We write to internal memory once a full such vector
    // is
    // written.
    if ((float_regs_counter >= 4 && uniform_setup.IsFloat32()) ||
        (float_regs_counter >= 3 && !uniform_setup.IsFloat32())) {
        float_regs_counter = 0;

        auto& uniform = setup.uniforms.f[uniform_setup.index];

        if (uniform_setup.index >= 96) {
            LOG_ERROR(HW_GPU, "Invalid %s float uniform index %d", shader_type,
                      (int)uniform_setup.index);
        } else {

            // NOTE: The destination component order indeed is "backwards"
            if (uniform_setup.IsFloat32()) {
                for (auto i : {0, 1, 2, 3})
                    uniform[3 - i] = float24::FromFloat32(*(float*)(&uniform_write_buffer[i]));
            } else {
                // TODO: Untested
                uniform.w = float24::FromRaw(uniform_write_buffer[0] >> 8);
                uniform.z = float24::FromRaw(((uniform_write_buffer[0] & 0xFF) << 16) |
                                             ((uniform_write_buffer[1] >> 16) & 0xFFFF));
                uniform.y = float24::FromRaw(((uniform_write_buffer[1] & 0xFFFF) << 8) |
                                             ((uniform_write_buffer[2] >> 24) & 0xFF));
                uniform.x = float24::FromRaw(uniform_write_buffer[2] & 0xFFFFFF);
            }

            LOG_TRACE(HW_GPU, "Set %s float uniform %x to (%f %f %f %f)", shader_type,
                      (int)uniform_setup.index, uniform.x.ToFloat32(), uniform.y.ToFloat32(),
                      uniform.z.ToFloat32(), uniform.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            uniform_setup.index.Assign(uniform_setup.index + 1);
        }
    }

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteUniformFloatReg(true, value);
    }
}

void WriteProgramCodeOffset(bool gs, u32 value) {
    auto& config = gs ? g_state.regs.gs : g_state.regs.vs;
    config.program.offset = value;

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteProgramCodeOffset(true, value);
    }
}

void WriteProgramCode(bool gs, u32 value) {
    const char* shader_type = gs ? "GS" : "VS";
    auto& config = gs ? g_state.regs.gs : g_state.regs.vs;
    auto& setup = gs ? g_state.gs : g_state.vs;

    if (config.program.offset >= setup.program_code.size()) {
        LOG_ERROR(HW_GPU, "Invalid %s program offset %d", shader_type, (int)config.program.offset);
    } else {
        setup.program_code[config.program.offset] = value;
        config.program.offset++;
    }

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteProgramCode(true, value);
    }
}

void WriteSwizzlePatternsOffset(bool gs, u32 value) {
    auto& config = gs ? g_state.regs.gs : g_state.regs.vs;
    config.swizzle_patterns.offset = value;

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteSwizzlePatternsOffset(true, value);
    }
}

void WriteSwizzlePatterns(bool gs, u32 value) {
    const char* shader_type = gs ? "GS" : "VS";
    auto& config = gs ? g_state.regs.gs : g_state.regs.vs;
    auto& setup = gs ? g_state.gs : g_state.vs;

    if (config.swizzle_patterns.offset >= setup.swizzle_data.size()) {
        LOG_ERROR(HW_GPU, "Invalid %s swizzle pattern offset %d", shader_type,
                  (int)config.swizzle_patterns.offset);
    } else {
        setup.swizzle_data[config.swizzle_patterns.offset] = value;
        config.swizzle_patterns.offset++;
    }

    // Copy for GS in shared mode
    if (!gs && SharedGS()) {
        WriteSwizzlePatterns(true, value);
    }
}

template <bool Debug>
void HandleEMIT(UnitState<Debug>& state) {
    auto& config = g_state.regs.gs;
    auto& emit_params = state.emit_params;
    auto& emit_buffers = state.emit_buffers;

    ASSERT(emit_params.vertex_id < 3);

    emit_buffers[emit_params.vertex_id] = state.output_registers;

    if (emit_params.primitive_emit) {
        ASSERT_MSG(state.emit_triangle_callback, "EMIT invoked but no handler set!");
        OutputVertex v0 = emit_buffers[0].ToVertex(config);
        OutputVertex v1 = emit_buffers[1].ToVertex(config);
        OutputVertex v2 = emit_buffers[2].ToVertex(config);
        if (emit_params.winding) {
            state.emit_triangle_callback(v2, v1, v0);
        } else {
            state.emit_triangle_callback(v0, v1, v2);
        }
    }
}

// Explicit instantiation
template void HandleEMIT(UnitState<false>& state);
template void HandleEMIT(UnitState<true>& state);

} // namespace Shader

} // namespace Pica
