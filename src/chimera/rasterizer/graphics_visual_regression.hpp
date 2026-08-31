// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_GRAPHICS_VISUAL_REGRESSION_HPP
#define CHIMERA_RASTERIZER_GRAPHICS_VISUAL_REGRESSION_HPP

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <windows.h>
#include <d3d9.h>

#include "../chimera.hpp"
#include "../event/d3d9_end_scene.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace GraphicsVisualRegression {
        struct Settings {
            bool enabled = false;
            std::uint64_t capture_frame = 120;
            std::string capture_path = "chimera_graphics_capture.bmp";
        };

        struct State {
            Settings settings {};
            std::uint64_t frame_counter = 0;
            bool capture_attempted = false;
        };

        inline State &state() noexcept {
            static State instance;
            return instance;
        }

        template<typename T> inline void release_com(T *&resource) noexcept {
            if(resource) {
                resource->Release();
                resource = nullptr;
            }
        }

        inline Settings load_settings() noexcept {
            Settings settings;
            const auto *ini = get_chimera().get_ini();
            if(!ini) {
                return settings;
            }

            settings.enabled = ini->get_value_bool("debug.visual_regression").value_or(false);
            if(const auto frame = ini->get_value_long("debug.visual_regression_capture_frame")) {
                const long clamped = std::max<long>(1, std::min<long>(*frame, 60000));
                settings.capture_frame = static_cast<std::uint64_t>(clamped);
            }
            if(const auto path = ini->get_value_string("debug.visual_regression_capture_path")) {
                if(!path->empty()) {
                    settings.capture_path = *path;
                }
            }
            return settings;
        }

        inline void write_status(bool success, const char *reason, UINT width = 0, UINT height = 0) noexcept {
            const auto &s = state();
            std::ofstream log("chimera_graphics_visual_capture.log", std::ios::trunc);
            if(!log.good()) {
                return;
            }
            log << "Chimera Graphics visual regression capture\n";
            log << "success=" << (success ? 1 : 0) << '\n';
            log << "capture_frame=" << s.settings.capture_frame << '\n';
            log << "capture_path=" << s.settings.capture_path << '\n';
            log << "width=" << width << '\n';
            log << "height=" << height << '\n';
            log << "reason=" << (reason ? reason : "") << '\n';
        }

        inline bool write_bmp(IDirect3DSurface9 *surface, UINT width, UINT height, const std::string &path) noexcept {
            if(!surface || width == 0 || height == 0 || path.empty()) {
                return false;
            }

            D3DLOCKED_RECT locked {};
            if(FAILED(IDirect3DSurface9_LockRect(surface, &locked, nullptr, D3DLOCK_READONLY))) {
                return false;
            }

            BITMAPFILEHEADER file_header {};
            BITMAPINFOHEADER info_header {};
            const std::uint32_t row_bytes = width * 4U;
            const std::uint32_t image_bytes = row_bytes * height;

            file_header.bfType = 0x4D42;
            file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            file_header.bfSize = file_header.bfOffBits + image_bytes;

            info_header.biSize = sizeof(BITMAPINFOHEADER);
            info_header.biWidth = static_cast<LONG>(width);
            info_header.biHeight = static_cast<LONG>(height);
            info_header.biPlanes = 1;
            info_header.biBitCount = 32;
            info_header.biCompression = BI_RGB;
            info_header.biSizeImage = image_bytes;

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            bool ok = output.good();
            if(ok) {
                output.write(reinterpret_cast<const char *>(&file_header), sizeof(file_header));
                output.write(reinterpret_cast<const char *>(&info_header), sizeof(info_header));
                for(UINT y = 0; y < height && output.good(); y++) {
                    const UINT source_y = height - 1U - y;
                    const auto *row = reinterpret_cast<const char *>(locked.pBits) +
                                      static_cast<std::size_t>(source_y) * static_cast<std::size_t>(locked.Pitch);
                    output.write(row, row_bytes);
                }
                ok = output.good();
            }

            IDirect3DSurface9_UnlockRect(surface);
            return ok;
        }

        inline bool capture(IDirect3DDevice9 *device, UINT &width, UINT &height, const char *&reason) noexcept {
            width = 0;
            height = 0;
            reason = "unknown";
            if(!device) {
                reason = "device unavailable";
                return false;
            }

            IDirect3DSurface9 *back_buffer = nullptr;
            IDirect3DSurface9 *resolved = nullptr;
            IDirect3DSurface9 *staging = nullptr;

            if(FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) || !back_buffer) {
                reason = "GetBackBuffer failed";
                return false;
            }

            D3DSURFACE_DESC description {};
            if(FAILED(IDirect3DSurface9_GetDesc(back_buffer, &description)) ||
               description.Width == 0 || description.Height == 0) {
                reason = "GetDesc failed";
                release_com(back_buffer);
                return false;
            }
            width = description.Width;
            height = description.Height;

            const D3DFORMAT capture_format =
                description.Format == D3DFMT_A8R8G8B8 || description.Format == D3DFMT_X8R8G8B8
                    ? description.Format
                    : D3DFMT_A8R8G8B8;

            HRESULT hr = IDirect3DDevice9_CreateRenderTarget(
                device,
                width,
                height,
                capture_format,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &resolved,
                nullptr
            );
            if(FAILED(hr) || !resolved) {
                reason = "CreateRenderTarget failed";
                release_com(back_buffer);
                return false;
            }

            hr = IDirect3DDevice9_StretchRect(device, back_buffer, nullptr, resolved, nullptr, D3DTEXF_NONE);
            if(FAILED(hr)) {
                reason = "StretchRect failed";
                release_com(resolved);
                release_com(back_buffer);
                return false;
            }

            hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
                device,
                width,
                height,
                capture_format,
                D3DPOOL_SYSTEMMEM,
                &staging,
                nullptr
            );
            if(FAILED(hr) || !staging) {
                reason = "CreateOffscreenPlainSurface failed";
                release_com(resolved);
                release_com(back_buffer);
                return false;
            }

            hr = IDirect3DDevice9_GetRenderTargetData(device, resolved, staging);
            if(FAILED(hr)) {
                reason = "GetRenderTargetData failed";
                release_com(staging);
                release_com(resolved);
                release_com(back_buffer);
                return false;
            }

            const bool written = write_bmp(staging, width, height, state().settings.capture_path);
            reason = written ? "capture complete" : "BMP write failed";

            release_com(staging);
            release_com(resolved);
            release_com(back_buffer);
            return written;
        }

        inline void on_end_scene_after(IDirect3DDevice9 *device) noexcept {
            auto &s = state();
            if(!s.settings.enabled || s.capture_attempted || !device) {
                return;
            }
            s.frame_counter++;
            if(s.frame_counter < s.settings.capture_frame) {
                return;
            }

            s.capture_attempted = true;
            UINT width = 0;
            UINT height = 0;
            const char *reason = nullptr;
            const bool success = capture(device, width, height, reason);
            write_status(success, reason, width, height);
            if(success) {
                console_output("Chimera Graphics visual regression capture saved to %s", s.settings.capture_path.c_str());
            }
            else {
                console_error("Chimera Graphics visual regression capture failed: %s", reason ? reason : "unknown error");
            }
        }

        inline void set_up() noexcept {
            auto &s = state();
            s.settings = load_settings();
            s.frame_counter = 0;
            s.capture_attempted = false;
            if(s.settings.enabled) {
                add_d3d9_end_scene_after_event(on_end_scene_after, EVENT_PRIORITY_AFTER);
            }
        }
    }
}

#endif
