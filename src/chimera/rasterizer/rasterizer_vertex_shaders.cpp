// SPDX-License-Identifier: GPL-3.0-only

#include <d3dcompiler.h>

#include <cstddef>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "rasterizer_vertex_shaders.hpp"
#include "../chimera.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shaders/shader_blob.hpp"
#include "../output/output.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"


namespace Chimera {
    DynamicVertex screen_vertices[4] = {
        -1.0f, 1.0, 0.0f, 0xffffffff, -1.0f, 1.0f,
        1.0f, 1.0, 0.0f, 0xffffffff, 1.0f, 1.0f,
        1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, -1.0f,
        -1.0f, -1.0f, 0.0f, 0xffffffff, -1.0f, -1.0f
    };

    // Full-size modern bank. A null slot means "use Halo's stock shader".
    // Known transparent-generic shaders use Chimera's existing hand-compiled
    // VS3 blobs. Stock VS1.1 shaders without relative address-register access
    // are translated instruction-for-instruction to VS3 at runtime. Paths that
    // use a0 remain stock until an exact family-specific conversion is added.
    static IDirect3DVertexShader9 *modern_vertex_shaders_3_0[NUM_OF_VERTEX_SHADERS] = {nullptr};
    static bool vsh_initialized = false;
    static bool vsh_creation_failed = false;

    enum class ModernBuildResult {
        built,
        skipped_relative_addressing,
        failed
    };

    static bool modern_shader_caps_available() noexcept {
        return d3d9_device_caps
            && d3d9_device_caps->PixelShaderVersion >= 0xffff0300
            && d3d9_device_caps->VertexShaderVersion >= 0xfffe0300;
    }

    static void replace_all(std::string &value, const char *from, const char *to) {
        if(!from || !*from || !to) {
            return;
        }
        const std::string needle(from);
        const std::string replacement(to);
        std::size_t position = 0;
        while((position = value.find(needle, position)) != std::string::npos) {
            value.replace(position, needle.length(), replacement);
            position += replacement.length();
        }
    }

    static std::string trim_left(const std::string &line) {
        const auto position = line.find_first_not_of(" \t");
        return position == std::string::npos ? std::string() : line.substr(position);
    }

    static bool legacy_vs11_to_vs30_assembly(
        const std::string &disassembly,
        std::string &output
    ) {
        const auto version_position = disassembly.find("vs_1_1");
        if(version_position == std::string::npos) {
            return false;
        }

        std::string program = disassembly.substr(version_position);
        const auto footer_position = program.find("// approximately");
        if(footer_position != std::string::npos) {
            program.resize(footer_position);
        }

        // Address-register conversion is deliberately not done mechanically.
        // VS1.1 mov a0 and VS3 mova have different integer-conversion behavior;
        // these are exactly the paths that caused model spikes under 9On12.
        if(program.find("a0.") != std::string::npos) {
            return false;
        }

        const bool uses_position = program.find("oPos") != std::string::npos;
        const bool uses_fog = program.find("oFog") != std::string::npos;
        const bool uses_d0 = program.find("oD0") != std::string::npos;
        const bool uses_d1 = program.find("oD1") != std::string::npos;
        bool uses_t[5] = {false, false, false, false, false};
        for(unsigned i = 0; i < 5; i++) {
            const std::string token = std::string("oT") + static_cast<char>('0' + i);
            uses_t[i] = program.find(token) != std::string::npos;
        }

        std::vector<std::string> definitions;
        std::vector<std::string> declarations;
        std::vector<std::string> instructions;
        std::istringstream input(program);
        std::string line;
        bool skipped_version = false;
        while(std::getline(input, line)) {
            if(!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::string trimmed = trim_left(line);
            if(trimmed.empty()) {
                continue;
            }
            if(!skipped_version && trimmed == "vs_1_1") {
                skipped_version = true;
                continue;
            }
            if(trimmed.rfind("def ", 0) == 0) {
                definitions.emplace_back(std::move(trimmed));
                continue;
            }
            if(trimmed.rfind("dcl_", 0) == 0) {
                declarations.emplace_back(std::move(trimmed));
                continue;
            }
            if(trimmed.rfind("//", 0) == 0 || trimmed[0] == '\0') {
                continue;
            }
            instructions.emplace_back(std::move(trimmed));
        }

        if(!skipped_version || !uses_position) {
            return false;
        }

        output.clear();
        output += "vs_3_0\n";
        for(const auto &definition : definitions) {
            output += definition + "\n";
        }
        for(const auto &declaration : declarations) {
            output += declaration + "\n";
        }

        // VS3 outputs are generic registers with explicit semantics. Keep a
        // stable mapping across every mechanically translated Halo shader.
        output += "dcl_position o0\n";
        if(uses_d0) output += "dcl_color o1\n";
        if(uses_d1) output += "dcl_color1 o2\n";
        if(uses_fog) output += "dcl_fog o3.x\n";
        for(unsigned i = 0; i < 5; i++) {
            if(uses_t[i]) {
                output += "dcl_texcoord";
                if(i != 0) output += static_cast<char>('0' + i);
                output += " o" + std::to_string(4 + i) + "\n";
            }
        }

        for(auto instruction : instructions) {
            replace_all(instruction, "oPos", "o0");
            replace_all(instruction, "oD0", "o1");
            replace_all(instruction, "oD1", "o2");
            replace_all(instruction, "oFog", "o3.x");
            replace_all(instruction, "oT0", "o4");
            replace_all(instruction, "oT1", "o5");
            replace_all(instruction, "oT2", "o6");
            replace_all(instruction, "oT3", "o7");
            replace_all(instruction, "oT4", "o8");
            output += instruction + "\n";
        }
        return true;
    }

    static ModernBuildResult transpile_stock_shader_to_vs3(
        IDirect3DDevice9 *device,
        IDirect3DVertexShader9 *stock,
        std::uint16_t index,
        std::FILE *log,
        IDirect3DVertexShader9 **out
    ) noexcept {
        if(!device || !stock || !out) {
            return ModernBuildResult::failed;
        }
        *out = nullptr;

        UINT byte_count = 0;
        HRESULT result = stock->GetFunction(nullptr, &byte_count);
        if(FAILED(result) || byte_count == 0) {
            if(log) std::fprintf(log, "%03u FAIL GetFunction(size) hr=0x%08lX\n", index, static_cast<unsigned long>(result));
            return ModernBuildResult::failed;
        }

        std::vector<unsigned char> bytecode(byte_count);
        result = stock->GetFunction(bytecode.data(), &byte_count);
        if(FAILED(result)) {
            if(log) std::fprintf(log, "%03u FAIL GetFunction(data) hr=0x%08lX\n", index, static_cast<unsigned long>(result));
            return ModernBuildResult::failed;
        }

        ID3DBlob *disassembly = nullptr;
        result = D3DDisassemble(bytecode.data(), byte_count, 0, nullptr, &disassembly);
        if(FAILED(result) || !disassembly) {
            if(log) std::fprintf(log, "%03u FAIL D3DDisassemble hr=0x%08lX\n", index, static_cast<unsigned long>(result));
            if(disassembly) disassembly->Release();
            return ModernBuildResult::failed;
        }

        std::string stock_assembly(
            static_cast<const char *>(disassembly->GetBufferPointer()),
            disassembly->GetBufferSize()
        );
        disassembly->Release();

        std::string modern_assembly;
        if(!legacy_vs11_to_vs30_assembly(stock_assembly, modern_assembly)) {
            if(stock_assembly.find("a0.") != std::string::npos) {
                if(log) std::fprintf(log, "%03u SKIP relative-addressing path requires exact conversion\n", index);
                return ModernBuildResult::skipped_relative_addressing;
            }
            if(log) std::fprintf(log, "%03u FAIL unsupported stock assembly\n", index);
            return ModernBuildResult::failed;
        }

        ID3DBlob *assembled = nullptr;
        ID3DBlob *errors = nullptr;
        result = D3DAssemble(
            modern_assembly.data(),
            modern_assembly.size(),
            nullptr,
            nullptr,
            0,
            &assembled,
            &errors
        );
        if(FAILED(result) || !assembled) {
            if(log) {
                std::fprintf(log, "%03u FAIL D3DAssemble hr=0x%08lX\n", index, static_cast<unsigned long>(result));
                if(errors && errors->GetBufferPointer()) {
                    std::fprintf(log, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
                }
                std::fprintf(log, "--- generated VS3 assembly ---\n%s--- end ---\n", modern_assembly.c_str());
            }
            if(errors) errors->Release();
            if(assembled) assembled->Release();
            return ModernBuildResult::failed;
        }

        result = device->CreateVertexShader(
            static_cast<const DWORD *>(assembled->GetBufferPointer()),
            out
        );
        if(errors) errors->Release();
        assembled->Release();
        if(FAILED(result) || !*out) {
            if(*out) {
                (*out)->Release();
                *out = nullptr;
            }
            if(log) std::fprintf(log, "%03u FAIL CreateVertexShader hr=0x%08lX\n", index, static_cast<unsigned long>(result));
            return ModernBuildResult::failed;
        }

        if(log) std::fprintf(log, "%03u BUILT direct VS1.1 -> VS3\n", index);
        return ModernBuildResult::built;
    }

    IDirect3DVertexShader9 *rasterizer_get_modern_vertex_shader(std::uint16_t index) noexcept {
        if(index >= NUM_OF_VERTEX_SHADERS || !modern_shader_caps_available()) {
            return nullptr;
        }
        rasterizer_create_vertex_shaders_3_0();
        if(!vsh_initialized) {
            return nullptr;
        }
        return modern_vertex_shaders_3_0[index];
    }

    bool rasterizer_has_modern_vertex_shader(std::uint16_t index) noexcept {
        return rasterizer_get_modern_vertex_shader(index) != nullptr;
    }

    std::size_t rasterizer_modern_vertex_shader_count() noexcept {
        if(!modern_shader_caps_available()) {
            return 0;
        }
        rasterizer_create_vertex_shaders_3_0();
        if(!vsh_initialized) {
            return 0;
        }
        std::size_t count = 0;
        for(auto *shader : modern_vertex_shaders_3_0) {
            if(shader) {
                count++;
            }
        }
        return count;
    }

    IDirect3DVertexShader9 *rasterizer_get_vertex_shader(std::uint16_t index) noexcept {
        if(index >= NUM_OF_VERTEX_SHADERS || !vertex_shaders) {
            return nullptr;
        }

        // Preserve Chimera's long-standing behavior for the transparent-generic
        // family. Newly transpiled slots are inventory-only until explicitly
        // enabled by the D3D9On12 modern-bank compatibility layer.
        if(index >= VSH_TRANSPARENT_GENERIC
            && index <= VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M) {
            if(IDirect3DVertexShader9 *modern = rasterizer_get_modern_vertex_shader(index)) {
                return modern;
            }
        }

        return vertex_shaders[index].shader;
    }

    IDirect3DVertexShader9 *rasterizer_get_vertex_shader_for_permutation(uint16_t vertex_shader_permutation, short vertex_type) noexcept {
        if(!vertex_shader_permutations || vertex_shader_permutation >= 6 || vertex_type < 0 || vertex_type >= NUM_OF_VERTEX_DECLARATIONS) {
            return nullptr;
        }
        return rasterizer_get_vertex_shader(vertex_shader_permutations[vertex_shader_permutation + static_cast<std::size_t>(vertex_type) * 6]);
    }

    IDirect3DVertexDeclaration9 *rasterizer_get_vertex_declaration(short vertex_type) noexcept {
        if(!vertex_declarations || vertex_type < 0 || vertex_type >= NUM_OF_VERTEX_DECLARATIONS) {
            return nullptr;
        }
        return vertex_declarations[vertex_type].declaration;
    }

    void rasterizer_create_vertex_shaders_3_0() noexcept {
        if(vsh_initialized || vsh_creation_failed || !d3d9_device_caps || !global_d3d9_device || !*global_d3d9_device) {
            return;
        }
        if(d3d9_device_caps->VertexShaderVersion < 0xfffe0300 || !vertex_shaders) {
            return;
        }

        auto create_shader = [](IDirect3DDevice9 *device, const void *shader_bytecode, IDirect3DVertexShader9 **shader) noexcept {
            if(!device || !shader_bytecode || !shader) {
                return false;
            }
            *shader = nullptr;
            const auto result = IDirect3DDevice9_CreateVertexShader(
                device,
                reinterpret_cast<const DWORD *>(shader_bytecode),
                shader
            );
            return SUCCEEDED(result) && *shader;
        };

        IDirect3DDevice9 *device = *global_d3d9_device;
        std::FILE *build_log = std::fopen("chimera_d3d9_modern_build.log", "w");
        if(build_log) {
            std::fprintf(build_log, "# D3D9On12 VS3 modern-bank build\n");
            std::fprintf(build_log, "# Direct VS1.1 paths are mechanically translated; a0 paths are intentionally deferred.\n\n");
        }

        unsigned built_blob = 0;
        unsigned built_direct = 0;
        unsigned skipped_relative = 0;
        unsigned failed = 0;

        struct KnownBlob {
            VertexShaderIndex index;
            const void *bytecode;
        };
        const KnownBlob known_blobs[] = {
            {VSH_TRANSPARENT_GENERIC, vsh_transparent_generic},
            {VSH_TRANSPARENT_GENERIC_LIT_M, vsh_transparent_generic_lit_m},
            {VSH_TRANSPARENT_GENERIC_M, vsh_transparent_generic_m},
            {VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED, vsh_transparent_generic_object_centered},
            {VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M, vsh_transparent_generic_object_centered_m},
            {VSH_TRANSPARENT_GENERIC_REFLECTION, vsh_transparent_generic_reflection},
            {VSH_TRANSPARENT_GENERIC_REFLECTION_M, vsh_transparent_generic_reflection_m},
            {VSH_TRANSPARENT_GENERIC_SCREENSPACE, vsh_transparent_generic_screenspace},
            {VSH_TRANSPARENT_GENERIC_SCREENSPACE_M, vsh_transparent_generic_screenspace_m},
            {VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED, vsh_transparent_generic_viewer_centered},
            {VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M, vsh_transparent_generic_viewer_centered_m}
        };

        for(const auto &known : known_blobs) {
            if(create_shader(device, known.bytecode, &modern_vertex_shaders_3_0[known.index])) {
                built_blob++;
                if(build_log) std::fprintf(build_log, "%03u BUILT existing Chimera VS3 blob\n", static_cast<unsigned>(known.index));
            }
            else {
                failed++;
                if(build_log) std::fprintf(build_log, "%03u FAIL existing Chimera VS3 blob\n", static_cast<unsigned>(known.index));
            }
        }

        for(std::uint16_t i = 0; i < NUM_OF_VERTEX_SHADERS; i++) {
            if(modern_vertex_shaders_3_0[i]) {
                continue;
            }
            IDirect3DVertexShader9 *stock = vertex_shaders[i].shader;
            if(!stock) {
                failed++;
                if(build_log) std::fprintf(build_log, "%03u FAIL stock shader unavailable\n", static_cast<unsigned>(i));
                continue;
            }

            const auto result = transpile_stock_shader_to_vs3(
                device,
                stock,
                i,
                build_log,
                &modern_vertex_shaders_3_0[i]
            );
            if(result == ModernBuildResult::built) {
                built_direct++;
            }
            else if(result == ModernBuildResult::skipped_relative_addressing) {
                skipped_relative++;
            }
            else {
                failed++;
            }
        }

        if(build_log) {
            std::fprintf(
                build_log,
                "\n# SUMMARY existing_vs3=%u direct_transpiled=%u relative_pending=%u failed=%u total_modern=%u/%u\n",
                built_blob,
                built_direct,
                skipped_relative,
                failed,
                built_blob + built_direct,
                static_cast<unsigned>(NUM_OF_VERTEX_SHADERS)
            );
            std::fclose(build_log);
        }

        vsh_initialized = true;
        console_output(
            "D3D9 modern VS bank: %u/%u built (%u existing VS3 + %u direct), %u relative pending, %u failed.",
            built_blob + built_direct,
            static_cast<unsigned>(NUM_OF_VERTEX_SHADERS),
            built_blob,
            built_direct,
            skipped_relative,
            failed
        );
    }

    void rasterizer_release_vertex_shaders_3_0() noexcept {
        for(auto &shader : modern_vertex_shaders_3_0) {
            if(shader) {
                IDirect3DVertexShader9_Release(shader);
                shader = nullptr;
            }
        }
        vsh_initialized = false;
        vsh_creation_failed = false;
    }

}
