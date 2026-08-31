// SPDX-License-Identifier: MIT
//
// Runtime generation of the SMAA AreaTex/SearchTex lookup data.
// The mathematics and texture layout follow the reference SMAA generators by
// Jorge Jimenez, Jose I. Echevarria, Tiago Sousa and Diego Gutierrez.
// https://github.com/iryoku/smaa

#ifndef CHIMERA_RASTERIZER_SMAA_LUT_GENERATOR_HPP
#define CHIMERA_RASTERIZER_SMAA_LUT_GENERATOR_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Chimera::SMAA::Lut {
    inline constexpr std::size_t AREA_WIDTH = 160;
    inline constexpr std::size_t AREA_HEIGHT = 560;
    inline constexpr std::size_t AREA_CHANNELS = 2;
    inline constexpr std::size_t SEARCH_WIDTH = 64;
    inline constexpr std::size_t SEARCH_HEIGHT = 16;

    struct Vec2 {
        double x;
        double y;

        constexpr Vec2 operator+(const Vec2 &other) const noexcept { return {x + other.x, y + other.y}; }
        constexpr Vec2 operator-(const Vec2 &other) const noexcept { return {x - other.x, y - other.y}; }
    };

    inline double saturate(double value) noexcept { return std::clamp(value, 0.0, 1.0); }
    inline Vec2 lerp(const Vec2 &a, const Vec2 &b, double p) noexcept { return {a.x + (b.x - a.x) * p, a.y + (b.y - a.y) * p}; }
    inline Vec2 smooth_area_value(double distance, Vec2 value) noexcept {
        const Vec2 base {std::sqrt(value.x * 2.0) * 0.5, std::sqrt(value.y * 2.0) * 0.5};
        return lerp(base, value, saturate(distance / 32.0));
    }

    inline Vec2 ortho_line_area(Vec2 p1, Vec2 p2, int pixel_x) noexcept {
        const Vec2 d = p2 - p1;
        const double x1 = static_cast<double>(pixel_x);
        const double x2 = x1 + 1.0;
        const double y1 = p1.y + d.y * (x1 - p1.x) / d.x;
        const double y2 = p1.y + d.y * (x2 - p1.x) / d.x;
        const bool inside = (x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x);
        if(!inside) return {0.0, 0.0};
        const bool trapezoid = std::copysign(1.0, y1) == std::copysign(1.0, y2) || std::abs(y1) < 1.0e-4 || std::abs(y2) < 1.0e-4;
        if(trapezoid) {
            const double a = (y1 + y2) * 0.5;
            return a < 0.0 ? Vec2 {std::abs(a), 0.0} : Vec2 {0.0, std::abs(a)};
        }
        const double crossing_x = -p1.y * d.x / d.y + p1.x;
        double integral = 0.0;
        const double fraction = std::modf(crossing_x, &integral);
        const double a1 = crossing_x > p1.x ? y1 * fraction * 0.5 : 0.0;
        const double a2 = crossing_x < p2.x ? y2 * (1.0 - fraction) * 0.5 : 0.0;
        const double a = std::abs(a1) > std::abs(a2) ? a1 : -a2;
        return a < 0.0 ? Vec2 {std::abs(a1), std::abs(a2)} : Vec2 {std::abs(a2), std::abs(a1)};
    }

    inline Vec2 area_ortho(int pattern, int left, int right, double offset) noexcept {
        const double distance = static_cast<double>(left + right + 1);
        const double o1 = 0.5 + offset;
        const double o2 = o1 - 1.0;
        const Vec2 mid {distance * 0.5, 0.0};
        switch(pattern) {
            case 0: case 5: case 10: case 15: return {0.0, 0.0};
            case 1: return left <= right ? ortho_line_area({0.0, o2}, mid, left) : Vec2 {0.0, 0.0};
            case 2: return left >= right ? ortho_line_area(mid, {distance, o2}, left) : Vec2 {0.0, 0.0};
            case 3: {
                const Vec2 a1 = smooth_area_value(distance, ortho_line_area({0.0, o2}, mid, left));
                const Vec2 a2 = smooth_area_value(distance, ortho_line_area(mid, {distance, o2}, left));
                return {a1.x + a2.x, a1.y + a2.y};
            }
            case 4: return left <= right ? ortho_line_area({0.0, o1}, mid, left) : Vec2 {0.0, 0.0};
            case 6:
                if(std::abs(offset) > 0.0) {
                    const Vec2 a1 = ortho_line_area({0.0, o1}, {distance, o2}, left);
                    const Vec2 a2a = ortho_line_area({0.0, o1}, mid, left);
                    const Vec2 a2b = ortho_line_area(mid, {distance, o2}, left);
                    return {(a1.x + a2a.x + a2b.x) * 0.5, (a1.y + a2a.y + a2b.y) * 0.5};
                }
                return ortho_line_area({0.0, o1}, {distance, o2}, left);
            case 7: return ortho_line_area({0.0, o1}, {distance, o2}, left);
            case 8: return left >= right ? ortho_line_area(mid, {distance, o1}, left) : Vec2 {0.0, 0.0};
            case 9:
                if(std::abs(offset) > 0.0) {
                    const Vec2 a1 = ortho_line_area({0.0, o2}, {distance, o1}, left);
                    const Vec2 a2a = ortho_line_area({0.0, o2}, mid, left);
                    const Vec2 a2b = ortho_line_area(mid, {distance, o1}, left);
                    return {(a1.x + a2a.x + a2b.x) * 0.5, (a1.y + a2a.y + a2b.y) * 0.5};
                }
                return ortho_line_area({0.0, o2}, {distance, o1}, left);
            case 11: return ortho_line_area({0.0, o2}, {distance, o1}, left);
            case 12: {
                const Vec2 a1 = smooth_area_value(distance, ortho_line_area({0.0, o1}, mid, left));
                const Vec2 a2 = smooth_area_value(distance, ortho_line_area(mid, {distance, o1}, left));
                return {a1.x + a2.x, a1.y + a2.y};
            }
            case 13: return ortho_line_area({0.0, o2}, {distance, o1}, left);
            case 14: return ortho_line_area({0.0, o1}, {distance, o2}, left);
            default: return {0.0, 0.0};
        }
    }

    inline bool same_point(const Vec2 &a, const Vec2 &b) noexcept { return a.x == b.x && a.y == b.y; }
    inline double diagonal_halfplane_coverage(Vec2 p1, Vec2 p2, Vec2 pixel) noexcept {
        constexpr int samples = 30;
        int covered = 0;
        for(int sx = 0; sx < samples; sx++) {
            for(int sy = 0; sy < samples; sy++) {
                const Vec2 point {pixel.x + static_cast<double>(sx) / static_cast<double>(samples - 1), pixel.y + static_cast<double>(sy) / static_cast<double>(samples - 1)};
                bool inside = true;
                if(!same_point(p1, p2)) {
                    const double xm = (p1.x + p2.x) * 0.5;
                    const double ym = (p1.y + p2.y) * 0.5;
                    const double a = p2.y - p1.y;
                    const double b = p1.x - p2.x;
                    inside = a * (point.x - xm) + b * (point.y - ym) > 0.0;
                }
                covered += inside ? 1 : 0;
            }
        }
        return static_cast<double>(covered) / static_cast<double>(samples * samples);
    }

    inline Vec2 diagonal_line_area(int pattern, Vec2 p1, Vec2 p2, int left, Vec2 offset) noexcept {
        static constexpr int edges[16][2] = {{0,0},{1,0},{0,2},{1,2},{2,0},{3,0},{2,2},{3,2},{0,1},{1,1},{0,3},{1,3},{2,1},{3,1},{2,3},{3,3}};
        if(edges[pattern][0] > 0) p1 = p1 + offset;
        if(edges[pattern][1] > 0) p2 = p2 + offset;
        const Vec2 base {static_cast<double>(left), static_cast<double>(left)};
        const double a1 = diagonal_halfplane_coverage(p1, p2, Vec2 {1.0, 0.0} + base);
        const double a2 = diagonal_halfplane_coverage(p1, p2, Vec2 {1.0, 1.0} + base);
        return {1.0 - a1, a2};
    }
    inline Vec2 average(Vec2 a, Vec2 b) noexcept { return {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}; }

    inline Vec2 area_diag(int pattern, int left, int right, Vec2 offset) noexcept {
        const double d = static_cast<double>(left + right + 1);
        const Vec2 dd {d, d};
        auto area = [&](Vec2 p1, Vec2 p2) noexcept { return diagonal_line_area(pattern, p1, p2, left, offset); };
        switch(pattern) {
            case 0: return average(area({1.0,1.0}, Vec2 {1.0,1.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 1: return average(area({1.0,0.0}, Vec2 {0.0,0.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 2: return average(area({0.0,0.0}, Vec2 {1.0,0.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 3: return area({1.0,0.0}, Vec2 {1.0,0.0} + dd);
            case 4: return average(area({1.0,1.0}, Vec2 {0.0,0.0} + dd), area({1.0,1.0}, Vec2 {1.0,0.0} + dd));
            case 5: return average(area({1.0,1.0}, Vec2 {0.0,0.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 6: return area({1.0,1.0}, Vec2 {1.0,0.0} + dd);
            case 7: return average(area({1.0,1.0}, Vec2 {1.0,0.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 8: return average(area({0.0,0.0}, Vec2 {1.0,1.0} + dd), area({1.0,0.0}, Vec2 {1.0,1.0} + dd));
            case 9: return area({1.0,0.0}, Vec2 {1.0,1.0} + dd);
            case 10: return average(area({0.0,0.0}, Vec2 {1.0,1.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 11: return average(area({1.0,0.0}, Vec2 {1.0,1.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            case 12: return area({1.0,1.0}, Vec2 {1.0,1.0} + dd);
            case 13: return average(area({1.0,1.0}, Vec2 {1.0,1.0} + dd), area({1.0,0.0}, Vec2 {1.0,1.0} + dd));
            case 14: return average(area({1.0,1.0}, Vec2 {1.0,1.0} + dd), area({1.0,1.0}, Vec2 {1.0,0.0} + dd));
            case 15: return average(area({1.0,1.0}, Vec2 {1.0,1.0} + dd), area({1.0,0.0}, Vec2 {1.0,0.0} + dd));
            default: return {0.0,0.0};
        }
    }

    inline std::uint8_t to_byte(double value) noexcept { return static_cast<std::uint8_t>(255.0 * saturate(value)); }
    inline std::array<std::uint8_t, AREA_WIDTH * AREA_HEIGHT * AREA_CHANNELS> build_area_data() {
        std::array<std::uint8_t, AREA_WIDTH * AREA_HEIGHT * AREA_CHANNELS> data {};
        static constexpr double ortho_offsets[7] = {0.0,-0.25,0.25,-0.125,0.125,-0.375,0.375};
        static constexpr Vec2 diag_offsets[5] = {{0.0,0.0},{0.25,-0.25},{-0.25,0.25},{0.125,-0.125},{-0.125,0.125}};
        static constexpr int ortho_edges[16][2] = {{0,0},{3,0},{0,3},{3,3},{1,0},{4,0},{1,3},{4,3},{0,1},{3,1},{0,4},{3,4},{1,1},{4,1},{1,4},{4,4}};
        static constexpr int diag_edges[16][2] = {{0,0},{1,0},{0,2},{1,2},{2,0},{3,0},{2,2},{3,2},{0,1},{1,1},{0,3},{1,3},{2,1},{3,1},{2,3},{3,3}};
        auto store = [&](std::size_t x, std::size_t y, Vec2 value) {
            const std::size_t i = (y * AREA_WIDTH + x) * AREA_CHANNELS;
            data[i] = to_byte(value.x);
            data[i + 1] = to_byte(value.y);
        };
        for(std::size_t sub = 0; sub < 7; sub++) for(int pattern = 0; pattern < 16; pattern++) for(int left = 0; left < 16; left++) for(int right = 0; right < 16; right++) {
            const std::size_t x = static_cast<std::size_t>(left + 16 * ortho_edges[pattern][0]);
            const std::size_t y = sub * 80U + static_cast<std::size_t>(right + 16 * ortho_edges[pattern][1]);
            store(x, y, area_ortho(pattern, left * left, right * right, ortho_offsets[sub]));
        }
        for(std::size_t sub = 0; sub < 5; sub++) for(int pattern = 0; pattern < 16; pattern++) for(int left = 0; left < 20; left++) for(int right = 0; right < 20; right++) {
            const std::size_t x = 80U + static_cast<std::size_t>(left + 20 * diag_edges[pattern][0]);
            const std::size_t y = sub * 80U + static_cast<std::size_t>(right + 20 * diag_edges[pattern][1]);
            store(x, y, area_diag(pattern, left, right, diag_offsets[sub]));
        }
        return data;
    }

    struct Edge4 { int v[4] {}; };
    inline std::array<std::uint8_t, SEARCH_WIDTH * SEARCH_HEIGHT> build_search_data() noexcept {
        std::array<std::uint8_t, 66U * 33U> full {};
        std::array<Edge4, 33> reverse {};
        std::array<bool, 33> valid {};
        for(int mask = 0; mask < 16; mask++) {
            Edge4 e {{(mask >> 0) & 1,(mask >> 1) & 1,(mask >> 2) & 1,(mask >> 3) & 1}};
            const int key = e.v[0] + 3 * e.v[1] + 7 * e.v[2] + 21 * e.v[3];
            reverse[static_cast<std::size_t>(key)] = e;
            valid[static_cast<std::size_t>(key)] = true;
        }
        auto delta_left = [](const Edge4 &left, const Edge4 &top) noexcept { int d = 0; if(top.v[3] == 1) d++; if(d == 1 && top.v[2] == 1 && left.v[1] != 1 && left.v[3] != 1) d++; return d; };
        auto delta_right = [](const Edge4 &left, const Edge4 &top) noexcept { int d = 0; if(top.v[3] == 1 && left.v[1] != 1 && left.v[3] != 1) d++; if(d == 1 && top.v[2] == 1 && left.v[0] != 1 && left.v[2] != 1) d++; return d; };
        for(int x = 0; x < 33; x++) for(int y = 0; y < 33; y++) {
            if(!valid[static_cast<std::size_t>(x)] || !valid[static_cast<std::size_t>(y)]) continue;
            const auto &left = reverse[static_cast<std::size_t>(x)];
            const auto &top = reverse[static_cast<std::size_t>(y)];
            full[static_cast<std::size_t>(y) * 66U + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(127 * delta_left(left, top));
            full[static_cast<std::size_t>(y) * 66U + static_cast<std::size_t>(33 + x)] = static_cast<std::uint8_t>(127 * delta_right(left, top));
        }
        std::array<std::uint8_t, SEARCH_WIDTH * SEARCH_HEIGHT> packed {};
        for(std::size_t y = 0; y < SEARCH_HEIGHT; y++) {
            const std::size_t source_y = 32U - y;
            for(std::size_t x = 0; x < SEARCH_WIDTH; x++) packed[y * SEARCH_WIDTH + x] = full[source_y * 66U + x];
        }
        return packed;
    }

    inline const auto &area_data() { static const auto data = build_area_data(); return data; }
    inline const auto &search_data() { static const auto data = build_search_data(); return data; }
}

#endif
