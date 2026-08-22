// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_VSH_ENC_EXPORT_HPP
#define CHIMERA_D3D9_VSH_ENC_EXPORT_HPP

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "d3d9_modern_shader_bank.hpp"
#include "d3d9_transparent_shader_compat.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9VshEncExport {
        static constexpr std::uint32_t XTEA_KEY[4] = {
            0x003FFFEFu,
            0x000000E5u,
            0x3FFFFFDDu,
            0x00007FC3u
        };
        static constexpr std::uint32_t XTEA_DELTA = 0x61C88647u;

        static std::uint32_t rotate_left(std::uint32_t value, unsigned amount) noexcept {
            return (value << amount) | (value >> (32u - amount));
        }

        static std::string md5_hex(const std::vector<unsigned char> &input) {
            static constexpr std::uint32_t shifts[64] = {
                7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
                5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
                4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
                6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
            };
            static constexpr std::uint32_t constants[64] = {
                0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
                0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
                0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
                0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
                0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
                0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
                0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
                0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
            };

            std::vector<unsigned char> padded = input;
            const std::uint64_t bit_count = static_cast<std::uint64_t>(padded.size()) * 8u;
            padded.push_back(0x80);
            while((padded.size() & 63u) != 56u) {
                padded.push_back(0);
            }
            for(unsigned i = 0; i < 8; i++) {
                padded.push_back(static_cast<unsigned char>((bit_count >> (i * 8u)) & 0xFFu));
            }

            std::uint32_t a0 = 0x67452301u;
            std::uint32_t b0 = 0xefcdab89u;
            std::uint32_t c0 = 0x98badcfeu;
            std::uint32_t d0 = 0x10325476u;

            for(std::size_t offset = 0; offset < padded.size(); offset += 64) {
                std::uint32_t words[16] = {};
                for(unsigned i = 0; i < 16; i++) {
                    const unsigned char *p = padded.data() + offset + i * 4u;
                    words[i] = static_cast<std::uint32_t>(p[0])
                        | (static_cast<std::uint32_t>(p[1]) << 8u)
                        | (static_cast<std::uint32_t>(p[2]) << 16u)
                        | (static_cast<std::uint32_t>(p[3]) << 24u);
                }

                std::uint32_t a = a0;
                std::uint32_t b = b0;
                std::uint32_t c = c0;
                std::uint32_t d = d0;

                for(unsigned i = 0; i < 64; i++) {
                    std::uint32_t f = 0;
                    unsigned g = 0;
                    if(i < 16) {
                        f = (b & c) | ((~b) & d);
                        g = i;
                    }
                    else if(i < 32) {
                        f = (d & b) | ((~d) & c);
                        g = (5u * i + 1u) & 15u;
                    }
                    else if(i < 48) {
                        f = b ^ c ^ d;
                        g = (3u * i + 5u) & 15u;
                    }
                    else {
                        f = c ^ (b | (~d));
                        g = (7u * i) & 15u;
                    }

                    const std::uint32_t old_d = d;
                    d = c;
                    c = b;
                    b = b + rotate_left(a + f + constants[i] + words[g], shifts[i]);
                    a = old_d;
                }

                a0 += a;
                b0 += b;
                c0 += c;
                d0 += d;
            }

            const std::uint32_t digest_words[4] = {a0, b0, c0, d0};
            static constexpr char hex[] = "0123456789abcdef";
            std::string result;
            result.reserve(32);
            for(std::uint32_t word : digest_words) {
                for(unsigned i = 0; i < 4; i++) {
                    const unsigned char byte = static_cast<unsigned char>((word >> (i * 8u)) & 0xFFu);
                    result.push_back(hex[byte >> 4u]);
                    result.push_back(hex[byte & 0x0Fu]);
                }
            }
            return result;
        }

        static void crypt_block(unsigned char *block, bool encrypt) noexcept {
            std::uint32_t first = 0;
            std::uint32_t second = 0;
            std::memcpy(&first, block, sizeof(first));
            std::memcpy(&second, block + 4, sizeof(second));

            if(encrypt) {
                std::uint32_t sum = 0;
                for(unsigned i = 0; i < 32; i++) {
                    sum -= XTEA_DELTA;
                    first += (((second << 4u) + XTEA_KEY[2]) ^ ((second >> 5u) + XTEA_KEY[3])) ^ (sum + second);
                    second += (((first >> 5u) + XTEA_KEY[0]) ^ ((first << 4u) + XTEA_KEY[1])) ^ (sum + first);
                }
            }
            else {
                std::uint32_t sum = 0xC6EF3720u;
                for(unsigned i = 0; i < 32; i++) {
                    second -= (((first >> 5u) + XTEA_KEY[0]) ^ ((first << 4u) + XTEA_KEY[1])) ^ (sum + first);
                    first -= (((second << 4u) + XTEA_KEY[2]) ^ ((second >> 5u) + XTEA_KEY[3])) ^ (sum + second);
                    sum += XTEA_DELTA;
                }
            }

            std::memcpy(block, &first, sizeof(first));
            std::memcpy(block + 4, &second, sizeof(second));
        }

        static bool decrypt_container(
            const std::vector<unsigned char> &encrypted,
            std::vector<unsigned char> &payload
        ) {
            if(encrypted.size() < 41) {
                return false;
            }

            std::vector<unsigned char> buffer = encrypted;
            const std::size_t buffer_size = buffer.size();
            if(buffer_size & 7u) {
                crypt_block(buffer.data() + buffer_size - 8u, false);
            }
            for(std::size_t offset = 0; offset < (buffer_size / 8u) * 8u; offset += 8u) {
                crypt_block(buffer.data() + offset, false);
            }

            if(buffer.size() < 33 || buffer.back() != 0) {
                return false;
            }

            const std::size_t payload_size = buffer.size() - 33u;
            std::string stored_hash(
                reinterpret_cast<const char *>(buffer.data() + payload_size),
                32u
            );
            payload.assign(buffer.begin(), buffer.begin() + payload_size);
            return md5_hex(payload) == stored_hash;
        }

        static std::vector<unsigned char> encrypt_container(const std::vector<unsigned char> &payload) {
            std::vector<unsigned char> buffer = payload;
            const std::string hash = md5_hex(payload);
            buffer.insert(buffer.end(), hash.begin(), hash.end());
            buffer.push_back(0);

            const std::size_t buffer_size = buffer.size();
            for(std::size_t offset = 0; offset < (buffer_size / 8u) * 8u; offset += 8u) {
                crypt_block(buffer.data() + offset, true);
            }
            if(buffer_size & 7u) {
                crypt_block(buffer.data() + buffer_size - 8u, true);
            }
            return buffer;
        }

        static bool read_file(const std::string &path, std::vector<unsigned char> &data) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if(!input) {
                return false;
            }
            const std::streamoff size = input.tellg();
            if(size <= 0) {
                return false;
            }
            input.seekg(0, std::ios::beg);
            data.resize(static_cast<std::size_t>(size));
            return static_cast<bool>(input.read(reinterpret_cast<char *>(data.data()), size));
        }

        static bool write_file(const std::string &path, const std::vector<unsigned char> &data) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if(!output) {
                return false;
            }
            output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
            output.flush();
            return static_cast<bool>(output);
        }

        static std::string halo_directory() {
            char path[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if(length == 0 || length >= MAX_PATH) {
                return std::string();
            }
            std::string result(path, path + length);
            const std::size_t separator = result.find_last_of("\\/");
            if(separator == std::string::npos) {
                return std::string();
            }
            result.resize(separator + 1u);
            return result;
        }

        static bool parse_shader_records(
            const std::vector<unsigned char> &payload,
            std::array<std::vector<unsigned char>, NUM_OF_VERTEX_SHADERS> &shaders
        ) {
            std::size_t offset = 0;
            for(std::size_t i = 0; i < NUM_OF_VERTEX_SHADERS; i++) {
                if(offset + sizeof(std::uint32_t) > payload.size()) {
                    return false;
                }
                std::uint32_t size = 0;
                std::memcpy(&size, payload.data() + offset, sizeof(size));
                offset += sizeof(size);
                if(size < sizeof(std::uint32_t) || offset + size > payload.size()) {
                    return false;
                }
                shaders[i].assign(payload.begin() + offset, payload.begin() + offset + size);
                offset += size;
            }
            return offset == payload.size();
        }

        static std::vector<unsigned char> build_shader_records(
            const std::array<std::vector<unsigned char>, NUM_OF_VERTEX_SHADERS> &shaders
        ) {
            std::vector<unsigned char> payload;
            std::size_t total = 0;
            for(const auto &shader : shaders) {
                total += sizeof(std::uint32_t) + shader.size();
            }
            payload.reserve(total);
            for(const auto &shader : shaders) {
                const std::uint32_t size = static_cast<std::uint32_t>(shader.size());
                const unsigned char *size_bytes = reinterpret_cast<const unsigned char *>(&size);
                payload.insert(payload.end(), size_bytes, size_bytes + sizeof(size));
                payload.insert(payload.end(), shader.begin(), shader.end());
            }
            return payload;
        }

        static bool shader_bytecode(
            IDirect3DVertexShader9 *shader,
            std::vector<unsigned char> &bytecode,
            std::uint32_t expected_version
        ) {
            if(!shader) {
                return false;
            }
            UINT size = 0;
            if(FAILED(shader->GetFunction(nullptr, &size)) || size < sizeof(std::uint32_t)) {
                return false;
            }
            bytecode.resize(size);
            if(FAILED(shader->GetFunction(bytecode.data(), &size))) {
                return false;
            }
            bytecode.resize(size);
            std::uint32_t version = 0;
            std::memcpy(&version, bytecode.data(), sizeof(version));
            return version == expected_version;
        }

        static bool export_vsh_9on12_impl() {
            if(!D3D9ModernShaderBank::d3d9on12_requested()) {
                console_error("D3D9 vsh export requires video_mode.d3d_backend=9on12.");
                return false;
            }
            if(!global_d3d9_device || !*global_d3d9_device || !vertex_shaders) {
                console_error("D3D9 vsh export: live device/shader table is not ready yet.");
                return false;
            }

            const std::string root = halo_directory();
            if(root.empty()) {
                console_error("D3D9 vsh export: could not resolve the Halo executable directory.");
                return false;
            }
            const std::string input_path = root + "shaders\\vsh.enc";
            const std::string output_path = root + "shaders\\vsh_9on12.enc";
            const std::string temporary_path = output_path + ".tmp";

            std::vector<unsigned char> encrypted_stock;
            if(!read_file(input_path, encrypted_stock)) {
                console_error("D3D9 vsh export: could not read %s", input_path.c_str());
                return false;
            }

            std::vector<unsigned char> stock_payload;
            if(!decrypt_container(encrypted_stock, stock_payload)) {
                console_error("D3D9 vsh export: stock vsh.enc failed XTEA/MD5 validation; refusing to write output.");
                return false;
            }

            std::array<std::vector<unsigned char>, NUM_OF_VERTEX_SHADERS> shaders;
            if(!parse_shader_records(stock_payload, shaders)) {
                console_error("D3D9 vsh export: stock payload is not the expected 64-record vertex shader collection.");
                return false;
            }

            // The MODEL family is the validated VS3 path: it restores stock colour
            // and lighting while the two known spike-producing transparent model
            // passes remain on their exact VS2 compatibility shaders below.
            for(std::uint16_t index = VSH_MODEL_FOGGED; index <= VSH_MODEL_ZBUFFER; index++) {
                IDirect3DVertexShader9 *modern = rasterizer_get_modern_vertex_shader(index);
                std::vector<unsigned char> bytecode;
                if(!shader_bytecode(modern, bytecode, 0xFFFE0300u)) {
                    console_error("D3D9 vsh export: MODEL slot %u does not have a valid VS3 candidate.", static_cast<unsigned>(index));
                    return false;
                }
                shaders[index] = std::move(bytecode);
            }

            if(!D3D9TransparentShaderCompat::prepare_exact_shaders()
                || !D3D9TransparentShaderCompat::exact_generic_m
                || !D3D9TransparentShaderCompat::exact_plasma_m) {
                console_error("D3D9 vsh export: exact GENERIC_M/PLASMA_M VS2 shaders are unavailable.");
                return false;
            }

            std::vector<unsigned char> generic_bytecode;
            std::vector<unsigned char> plasma_bytecode;
            if(!shader_bytecode(D3D9TransparentShaderCompat::exact_generic_m, generic_bytecode, 0xFFFE0200u)
                || !shader_bytecode(D3D9TransparentShaderCompat::exact_plasma_m, plasma_bytecode, 0xFFFE0200u)) {
                console_error("D3D9 vsh export: exact transparent compatibility shaders failed VS2 validation.");
                return false;
            }
            shaders[VSH_TRANSPARENT_GENERIC_M] = std::move(generic_bytecode);
            shaders[VSH_TRANSPARENT_PLASMA_M] = std::move(plasma_bytecode);

            const std::vector<unsigned char> rebuilt_payload = build_shader_records(shaders);
            const std::vector<unsigned char> encrypted_output = encrypt_container(rebuilt_payload);

            // Round-trip our own output before touching the destination file.
            std::vector<unsigned char> verified_payload;
            if(!decrypt_container(encrypted_output, verified_payload) || verified_payload != rebuilt_payload) {
                console_error("D3D9 vsh export: generated collection failed internal XTEA/MD5 round-trip validation.");
                return false;
            }
            std::array<std::vector<unsigned char>, NUM_OF_VERTEX_SHADERS> verified_shaders;
            if(!parse_shader_records(verified_payload, verified_shaders)) {
                console_error("D3D9 vsh export: generated collection failed 64-record validation.");
                return false;
            }

            DeleteFileA(temporary_path.c_str());
            if(!write_file(temporary_path, encrypted_output)) {
                DeleteFileA(temporary_path.c_str());
                console_error("D3D9 vsh export: could not write temporary output.");
                return false;
            }
            if(!MoveFileExA(
                temporary_path.c_str(),
                output_path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            )) {
                DeleteFileA(temporary_path.c_str());
                console_error("D3D9 vsh export: could not finalize %s (Win32=%lu).", output_path.c_str(), static_cast<unsigned long>(GetLastError()));
                return false;
            }

            console_output("D3D9 vsh export: created shaders\\vsh_9on12.enc (%lu bytes).", static_cast<unsigned long>(encrypted_output.size()));
            console_output("D3D9 vsh export: 10 MODEL slots=VS3, GENERIC_M=exact VS2, PLASMA_M=exact VS2; 52 stock slots preserved.");
            console_output("D3D9 vsh export: original shaders\\vsh.enc was not modified.");
            return true;
        }

        static bool export_vsh_9on12() noexcept {
            try {
                return export_vsh_9on12_impl();
            }
            catch(...) {
                console_error("D3D9 vsh export: unexpected failure; original vsh.enc was left untouched.");
                return false;
            }
        }
    }
}

#endif
