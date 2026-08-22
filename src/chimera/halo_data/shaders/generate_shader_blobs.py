# SPDX-License-Identifier: GPL-3.0-only

import os
import struct
import sys

NUM_VERTEX_SHADERS = 64
D3D9ON12_PATCH_SLOTS = {
    25: 0xFFFE0300,
    26: 0xFFFE0300,
    27: 0xFFFE0300,
    28: 0xFFFE0300,
    29: 0xFFFE0300,
    30: 0xFFFE0300,
    31: 0xFFFE0300,
    32: 0xFFFE0300,
    33: 0xFFFE0300,
    34: 0xFFFE0300,
    39: 0xFFFE0200,
    59: 0xFFFE0200,
}


def output_file_for_type(shader_type):
    if shader_type == 0:
        return sys.argv[2] + "d3dx_effects.cpp"
    if shader_type == 1:
        return sys.argv[2] + "effects_collection.cpp"
    if shader_type == 2:
        return sys.argv[2] + "vertex_shaders.cpp"
    if shader_type == 3:
        return sys.argv[2] + "pixel_shaders.cpp"
    raise ValueError("invalid shader blob type")


def generate_shader_blob_data(name, data, shader_type):
    output_path = output_file_for_type(shader_type)
    collection_size = os.path.getsize(output_path)

    with open(output_path, "a") as collection:
        if collection_size == 0:
            collection.write("#include <cstddef>\n" + "#include " + '"' + sys.argv[1] + 'shader_blob.hpp"')

        collection.write("\n\n")
        collection.write("const size_t " + name + "_size = " + str(len(data)) + ";\n\n")
        collection.write("unsigned char " + name + "[" + str(len(data)) + "] = {\n")

        for index, byte in enumerate(data):
            if index != 0:
                collection.write(", ")
            collection.write("0x{:02x}".format(byte))
            if (index + 1) % 20 == 0:
                collection.write("\n")

        collection.write("\n};\n")


def generate_shader_blobs(name, binary, shader_type):
    with open(binary, "rb") as shaders:
        generate_shader_blob_data(name, shaders.read(), shader_type)


def parse_vsh_collection(data):
    shaders = []
    offset = 0

    for index in range(NUM_VERTEX_SHADERS):
        if offset + 4 > len(data):
            raise RuntimeError("vsh.bin ended before shader {}".format(index))

        size = struct.unpack_from("<I", data, offset)[0]
        offset += 4

        if size < 4 or offset + size > len(data):
            raise RuntimeError("vsh.bin has invalid shader {} size {}".format(index, size))

        shaders.append(data[offset:offset + size])
        offset += size

    if offset != len(data):
        raise RuntimeError("vsh.bin has trailing data after 64 shaders")

    return shaders


def build_vsh_collection(shaders):
    output = bytearray()
    for shader in shaders:
        output += struct.pack("<I", len(shader))
        output += shader
    return bytes(output)


def read_d3d9on12_patch(path):
    with open(path, "rb") as patch_file:
        data = patch_file.read()

    if len(data) < 8 or data[:4] != b"V9P1":
        raise RuntimeError("invalid D3D9On12 VSH patch header")

    count = struct.unpack_from("<I", data, 4)[0]
    offset = 8
    entries = {}

    for _ in range(count):
        if offset + 8 > len(data):
            raise RuntimeError("truncated D3D9On12 VSH patch")

        index, size = struct.unpack_from("<II", data, offset)
        offset += 8

        if size < 4 or offset + size > len(data):
            raise RuntimeError("invalid D3D9On12 VSH patch slot size")

        if index in entries:
            raise RuntimeError("duplicate D3D9On12 VSH patch slot {}".format(index))

        entries[index] = data[offset:offset + size]
        offset += size

    if offset != len(data):
        raise RuntimeError("D3D9On12 VSH patch has trailing data")

    return entries


def build_d3d9on12_vsh_collection(base_path, patch_path):
    with open(base_path, "rb") as base_file:
        shaders = parse_vsh_collection(base_file.read())

    replacements = read_d3d9on12_patch(patch_path)
    expected_indices = set(D3D9ON12_PATCH_SLOTS)

    if set(replacements) != expected_indices:
        raise RuntimeError(
            "D3D9On12 VSH patch slots do not match the validated set: {}".format(
                sorted(replacements)
            )
        )

    for index, bytecode in replacements.items():
        version = struct.unpack_from("<I", bytecode, 0)[0]
        expected_version = D3D9ON12_PATCH_SLOTS[index]
        if version != expected_version:
            raise RuntimeError(
                "shader model mismatch for D3D9On12 VSH slot {}: 0x{:08x} != 0x{:08x}".format(
                    index, version, expected_version
                )
            )
        shaders[index] = bytecode

    rebuilt = build_vsh_collection(shaders)
    parse_vsh_collection(rebuilt)
    return rebuilt


if __name__ == '__main__':
    def eprint(message):
        print(message, file=sys.stderr)

    if len(sys.argv) != 3:
        eprint("Syntax: {} <shader_files> <output>".format(sys.argv[0]))
        sys.exit(1)

    for output_name in ("d3dx_effects.cpp", "effects_collection.cpp", "vertex_shaders.cpp", "pixel_shaders.cpp"):
        output_path = sys.argv[2] + output_name
        if os.path.exists(output_path):
            os.remove(output_path)
        with open(output_path, "w"):
            pass

    generate_shader_blobs("fx_collection", sys.argv[1] + "fx/fx.bin", 0)
    generate_shader_blobs("ce_effects_collection", sys.argv[1] + "fx/EffectCollection_ps_2_0.bin", 1)

    vsh_base_path = sys.argv[1] + "vertex/vsh.bin"
    generate_shader_blobs("vsh_collection", vsh_base_path, 2)
    vsh_9on12 = build_d3d9on12_vsh_collection(vsh_base_path, sys.argv[1] + "vertex/vsh_9on12_patch.bin")
    generate_shader_blob_data("vsh_9on12_collection", vsh_9on12, 2)

    generate_shader_blobs("vsh_transparent_generic", sys.argv[1] + "vertex/transparent_generic.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_lit_m", sys.argv[1] + "vertex/transparent_generic_lit_m.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_m", sys.argv[1] + "vertex/transparent_generic_m.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_object_centered", sys.argv[1] + "vertex/transparent_generic_object_centered.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_object_centered_m", sys.argv[1] + "vertex/transparent_generic_object_centered_m.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_reflection", sys.argv[1] + "vertex/transparent_generic_reflection.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_reflection_m", sys.argv[1] + "vertex/transparent_generic_reflection_m.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_screenspace", sys.argv[1] + "vertex/transparent_generic_screenspace.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_screenspace_m", sys.argv[1] + "vertex/transparent_generic_screenspace_m.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_viewer_centered", sys.argv[1] + "vertex/transparent_generic_viewer_centered.cso", 2)
    generate_shader_blobs("vsh_transparent_generic_viewer_centered_m", sys.argv[1] + "vertex/transparent_generic_viewer_centered_m.cso", 2)

    generate_shader_blobs("shader_transparent_generic_source", sys.argv[1] + "pixel/hlsl/shader_transparent_generic.psh", 3)
    generate_shader_blobs("shader_transparent_generic_2_0_source", sys.argv[1] + "pixel/hlsl/shader_transparent_generic_2_0.psh", 3)

    generate_shader_blobs("white", sys.argv[1] + "pixel/white.cso", 3)
    generate_shader_blobs("white_1_1", sys.argv[1] + "pixel/white.cso", 3)
    generate_shader_blobs("hud_meters", sys.argv[1] + "pixel/hud_meters.cso", 3)
    generate_shader_blobs("fog", sys.argv[1] + "pixel/fog.cso", 3)
    generate_shader_blobs("fog_akill", sys.argv[1] + "pixel/fog_akill.cso", 3)
    generate_shader_blobs("fog_screen", sys.argv[1] + "pixel/fog_screen.cso", 3)
    generate_shader_blobs("black", sys.argv[1] + "pixel/black.cso", 3)

    generate_shader_blobs("eff_nlin_tint_add_z", sys.argv[1] + "pixel/eff_nlin_tint_add_z.cso", 3)
    generate_shader_blobs("eff_nlin_tint_alpha_blend_z", sys.argv[1] + "pixel/eff_nlin_tint_alpha_blend_z.cso", 3)
    generate_shader_blobs("eff_nlin_tint_double_mul_z", sys.argv[1] + "pixel/eff_nlin_tint_double_mul_z.cso", 3)
    generate_shader_blobs("eff_nlin_tint_mul_add_z", sys.argv[1] + "pixel/eff_nlin_tint_mul_add_z.cso", 3)
    generate_shader_blobs("eff_nlin_tint_mul_z", sys.argv[1] + "pixel/eff_nlin_tint_mul_z.cso", 3)
    generate_shader_blobs("eff_nlin_tint_z", sys.argv[1] + "pixel/eff_nlin_tint_z.cso", 3)
    generate_shader_blobs("eff_normal_tint_add_z", sys.argv[1] + "pixel/eff_normal_tint_add_z.cso", 3)
    generate_shader_blobs("eff_normal_tint_alpha_blend_z", sys.argv[1] + "pixel/eff_normal_tint_alpha_blend_z.cso", 3)
    generate_shader_blobs("eff_normal_tint_double_mul_z", sys.argv[1] + "pixel/eff_normal_tint_double_mul_z.cso", 3)
    generate_shader_blobs("eff_normal_tint_mul_add_z", sys.argv[1] + "pixel/eff_normal_tint_mul_add_z.cso", 3)
    generate_shader_blobs("eff_normal_tint_mul_z", sys.argv[1] + "pixel/eff_normal_tint_mul_z.cso", 3)
    generate_shader_blobs("eff_normal_tint_z", sys.argv[1] + "pixel/eff_normal_tint_z.cso", 3)

    generate_shader_blobs("decal_add", sys.argv[1] + "pixel/decal_add.cso", 3)
    generate_shader_blobs("decal_multiply", sys.argv[1] + "pixel/decal_multiply.cso", 3)
    generate_shader_blobs("decal_multiply2x", sys.argv[1] + "pixel/decal_multiply2x.cso", 3)
    generate_shader_blobs("decal_alpha_blend", sys.argv[1] + "pixel/decal_alpha_blend.cso", 3)
    generate_shader_blobs("decal_alpha_madd", sys.argv[1] + "pixel/decal_alpha_madd.cso", 3)
