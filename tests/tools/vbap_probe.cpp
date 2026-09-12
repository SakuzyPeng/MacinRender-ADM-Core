// Spread/gain replay loops adapted from SAF's saf_vbap.c:
// Copyright 2017-2018 Leo McCormack
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
// OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.
//
// Release-only stage attribution for the two 5.1.4 MDAP fixtures. Inputs come from the
// real renderer. Every native decomposition is bit-checked against the linked SAF API.
// The spread/gain loops below preserve saf_vbap.c's operation order; they are diagnostic
// replay code, not a replacement renderer or a claim of correctly rounded arithmetic.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <saf.h>
#include <saf_utility_complex.h>
#include <saf_vbap.h>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "consistency_trace.h"
#include "vbap_probe_backend.h"

#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
extern "C" void mr_adm_diagnostic_seed(unsigned int seed);
#endif

namespace {
using mradm::consistency::dump;
using Path = std::filesystem::path;

struct SafFree {
    template <typename T> void operator()(T* ptr) const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        std::free(ptr);
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T> std::vector<T> read_words(const Path& path) {
    std::ifstream in(path, std::ios::binary);
    require(in.is_open(), "cannot open " + path.string());
    std::vector<T> result;
    std::array<unsigned char, 4> bytes{};
    while (in.read(reinterpret_cast<char*>(bytes.data()), 4)) {
        const std::uint32_t word = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                   (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                   (static_cast<std::uint32_t>(bytes[3]) << 24U);
        result.push_back(std::bit_cast<T>(word));
    }
    require(in.eof() && in.gcount() == 0 && !result.empty(), "invalid checkpoint " + path.string());
    return result;
}

bool same_bits(std::span<const float> a, std::span<const float> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
               return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
           });
}

void seed_rng(unsigned int seed) {
    std::srand(seed);
#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
    mr_adm_diagnostic_seed(seed);
#endif
}

std::vector<float> inverse_for(std::vector<float>& vertices, std::vector<int>& faces) {
    require(vertices.size() % 3U == 0U && faces.size() % 3U == 0U, "invalid geometry dimensions");
    for (int index : faces) {
        require(index >= 0 && static_cast<std::size_t>(index) < vertices.size() / 3U, "invalid triangle index");
    }
    float* raw = nullptr;
    invertLsMtx3D(vertices.data(), faces.data(), static_cast<int>(faces.size() / 3U), &raw);
    const std::unique_ptr<float, SafFree> owner(raw);
    require(raw != nullptr, "SAF inverse allocation failed");
    return {raw, raw + (faces.size() * 3U)};
}

// Specialisation of getSpreadSrcDirs3D for the real MDAP call: eight sources, one ring.
// The volatile count keeps sinf(theta)/cosf(theta) as runtime libm calls, as in SAF.
std::vector<float> spread_directions(const std::vector<float>& source) {
    const float az = source[0] * SAF_PI / 180.0F;
    const float el = source[1] * SAF_PI / 180.0F;
    const volatile int runtime_count = 8;
    const int count = runtime_count;
    const float theta = 2.0F * SAF_PI / static_cast<float>(count);
    const float sin_theta = std::sin(theta);
    const float cos_theta = std::cos(theta);
    std::array<float, 3> u{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
    const std::array<float, 9> cross{0.0F, -u[2], u[1], u[2], 0.0F, -u[0], -u[1], u[0], 0.0F};
    std::array<float, 9> rotation{};
    for (std::size_t i = 0; i < 3U; ++i) {
        for (std::size_t j = 0; j < 3U; ++j) {
            const float outer = i == j ? std::pow(u.at(i), 2.0F) : u.at(i) * u.at(j);
            rotation.at((i * 3U) + j) =
                (sin_theta * cross.at((i * 3U) + j)) + ((1.0F - cos_theta) * outer) + (i == j ? cos_theta : 0.0F);
        }
    }
    std::vector<float> ring(24U, 0.0F);
    if (el > (SAF_PI / 2.0F) - 0.01F || el < -((SAF_PI / 2.0F) - 0.01F)) {
        ring[0] = 1.0F;
    } else {
        std::array<float, 3> axis{0.0F, 0.0F, 1.0F};
        std::array<float, 3> perpendicular{};
        mr_adm_vbap_probe_cross(u.data(), axis.data(), perpendicular.data());
        float norm = 0.0F;
        for (float value : perpendicular) {
            // Retain SAF's explicit sequential reduction in this diagnostic replay.
            // cppcheck-suppress useStlAlgorithm
            norm += std::pow(value, 2.0F);
        }
        norm = std::sqrt(norm);
        for (std::size_t i = 0; i < 3U; ++i) {
            ring[i] = perpendicular.at(i) / norm;
        }
    }
    const float spread_rad = (source[3] / 2.0F) * SAF_PI / 180.0F;
    dump("spread.01-trig.f32",
         {az,
          el,
          std::cos(el),
          std::cos(az),
          std::sin(az),
          std::sin(el),
          theta,
          sin_theta,
          cos_theta,
          spread_rad,
          std::tan(spread_rad)});
    dump("spread.02-axis.f32", std::span<const float>(u));
    dump("spread.03-rotation.f32", std::span<const float>(rotation));
    dump("spread.04-base.f32", std::span<const float>(ring.data(), 3U));
    for (std::size_t i = 1; i < 8U; ++i) {
        std::array<float, 3> previous{};
        std::copy_n(ring.data() + ((i - 1U) * 3U), 3U, previous.begin());
        mr_adm_vbap_probe_rotate(rotation.data(), previous.data(), ring.data() + (i * 3U));
    }
    dump("spread.05-rotated-ring.f32", ring);
    std::vector<float> result(27U, 0.0F);
    for (std::size_t i = 0; i < 24U; ++i) {
        result[i] = u.at(i % 3U) + (ring[i] * std::tan(spread_rad));
    }
    dump("spread.06-before-normalize.f32", result);
    const float norm = std::sqrt(std::pow(result[0], 2.0F) + std::pow(result[1], 2.0F) + std::pow(result[2], 2.0F));
    dump("spread.07-norm.f32", {norm});
    for (std::size_t i = 0; i < 24U; ++i) {
        result[i] /= norm;
    }
    std::ranges::copy(u, result.begin() + 24);
    std::vector<float> actual(27U);
    getSpreadSrcDirs3D(az, el, source[3], 8, 1, actual.data());
    dump("spread.08-directions.f32", actual);
    dump("spread.09-replayed-directions.f32", result);
    require(same_bits(actual, result), "spread replay differs from linked SAF");
    return actual;
}

enum class DotMode { backend, separate_021, fused_012 };

// MDAP branch of vbap3D. The default retains the real utility_svvdot implementation;
// the other modes are explicit arithmetic hypotheses for the fixed-input replay only.
std::vector<float> gains_from(const std::vector<float>& directions,
                              const std::vector<int>& faces,
                              const std::vector<float>& inverse,
                              std::size_t speakers,
                              const std::string& prefix,
                              DotMode mode = DotMode::backend) {
    require(directions.size() == 27U && inverse.size() == faces.size() * 3U, "invalid gain dimensions");
    std::vector<float> gains(speakers, 0.0F);
    std::vector<float> dots;
    std::vector<int> accepted;
    for (std::size_t source = 0; source < 9U; ++source) {
        std::array<float, 3> u{};
        std::copy_n(directions.data() + (source * 3U), 3U, u.begin());
        for (std::size_t face = 0; face < faces.size() / 3U; ++face) {
            std::array<float, 3> dot{};
            for (std::size_t row = 0; row < 3U; ++row) {
                std::array<float, 3> matrix_row{};
                std::copy_n(inverse.data() + (face * 9U) + (row * 3U), 3U, matrix_row.begin());
                if (mode == DotMode::backend) {
                    utility_svvdot(matrix_row.data(), u.data(), 3, &dot.at(row));
                } else if (mode == DotMode::separate_021) {
                    // Initial +0 is intentional: it also fixes the sign of zero terms.
                    const float first = 0.0F + (matrix_row[0] * u[0]);
                    const float second = 0.0F + (matrix_row[1] * u[1]);
                    dot.at(row) = (first + (matrix_row[2] * u[2])) + second;
                } else {
                    dot.at(row) = std::fma(
                        matrix_row[2], u[2], std::fma(matrix_row[1], u[1], std::fma(matrix_row[0], u[0], 0.0F)));
                }
            }
            dots.insert(dots.end(), dot.begin(), dot.end());
            float min_value = 2.23e13F;
            float norm = 0.0F;
            for (float value : dot) {
                min_value = std::min(min_value, value);
                norm += std::pow(value, 2.0F);
            }
            norm = std::sqrt(norm);
            const bool active = static_cast<double>(min_value) > -0.001;
            accepted.push_back(active ? 1 : 0);
            if (active) {
                for (std::size_t j = 0; j < 3U; ++j) {
                    const auto speaker = static_cast<std::size_t>(faces[(face * 3U) + j]);
                    require(speaker < speakers, "invalid gain speaker index");
                    gains[speaker] += dot.at(j) / norm;
                }
            }
        }
    }
    dump(prefix + "-dots.f32", dots);
    dump(prefix + "-accepted.i32", accepted);
    dump(prefix + "-before-normalize.f32", gains);
    float norm = 0.0F;
    for (float gain : gains) {
        // Retain SAF's explicit sequential reduction in this diagnostic replay.
        // cppcheck-suppress useStlAlgorithm
        norm += std::pow(gain, 2.0F);
    }
    norm = std::sqrt(norm);
    dump(prefix + "-norm.f32", {norm});
    for (float& gain : gains) {
        // Keep the replay's scalar normalisation step aligned with the SAF loop.
        // cppcheck-suppress useStlAlgorithm
        gain = std::max(gain / norm, 0.0F);
    }
    dump(prefix + "-gains.f32", gains);
    return gains;
}

void probe(const Path& input, unsigned int seed, const Path& reference) {
    auto source = read_words<float>(input / "vbap.01-source.f32");
    auto speakers = read_words<float>(input / "vbap.02-speakers.f32");
    require(source.size() == 4U && source[3] > 0.1F && speakers.size() == 18U,
            "probe requires one spread source and the nine non-LFE speakers of 5.1.4");
    dump("input.01-source.f32", source);
    dump("input.02-speakers.f32", speakers);
    seed_rng(seed);
    float* raw_full = nullptr;
    int rows = 0;
    int full_faces = 0;
    generateVBAPgainTable3D_srcs(source.data(), 1, speakers.data(), 9, 1, 1, source[3], &raw_full, &rows, &full_faces);
    const std::unique_ptr<float, SafFree> full_owner(raw_full);
    require(raw_full != nullptr && rows == 1, "SAF full gain table failed");
    const std::vector<float> full(raw_full, raw_full + 9);
    dump("output.01-full-gains.f32", full);
    if (seed == 1U) {
        require(same_bits(full, read_words<float>(input / "vbap.03-gains.f32")), "seed 1 differs from real renderer");
    }

    // Restrict the probe to captured 5.1.4 layouts with no bottom speaker. Match SAF's
    // bottom dummy and optional top dummy, using its actual elevation threshold.
    bool top = false;
    const float dummy_limit = mr_adm_vbap_probe_dummy_limit();
    for (std::size_t i = 1; i < speakers.size(); i += 2U) {
        require(speakers[i] > -dummy_limit, "unexpected bottom speaker");
        top = top || speakers[i] >= dummy_limit;
    }
    speakers.insert(speakers.end(), {0.0F, -90.0F});
    if (!top) {
        speakers.insert(speakers.end(), {0.0F, 90.0F});
    }
    seed_rng(seed);
    float* raw_vertices = nullptr;
    int* raw_faces = nullptr;
    int vertex_count = 0;
    int face_count = 0;
    findLsTriplets(speakers.data(),
                   static_cast<int>(speakers.size() / 2U),
                   1,
                   &raw_vertices,
                   &vertex_count,
                   &raw_faces,
                   &face_count);
    const std::unique_ptr<float, SafFree> vertices_owner(raw_vertices);
    const std::unique_ptr<int, SafFree> faces_owner(raw_faces);
    require(raw_vertices != nullptr && raw_faces != nullptr && face_count == full_faces && vertex_count > 0,
            "SAF triangulation failed");
    std::vector<float> vertices(raw_vertices, raw_vertices + (static_cast<std::size_t>(vertex_count) * 3U));
    std::vector<int> faces(raw_faces, raw_faces + (static_cast<std::size_t>(face_count) * 3U));
    dump("geometry.01-vertices.f32", vertices);
    dump("geometry.02-faces.i32", faces);
    auto inverse = inverse_for(vertices, faces);
    dump("geometry.03-inverse.f32", inverse);
    float* raw_staged = nullptr;
    vbap3D(source.data(), 1, vertex_count, faces.data(), face_count, source[3], inverse.data(), &raw_staged);
    const std::unique_ptr<float, SafFree> staged_owner(raw_staged);
    require(raw_staged != nullptr, "SAF staged gain table failed");
    dump("output.02-staged-gains.f32", std::span<const float>(raw_staged, static_cast<std::size_t>(vertex_count)));
    require(same_bits(full, {raw_staged, 9U}), "staged SAF differs from full wrapper");
    const auto directions = spread_directions(source);
    const auto gains = gains_from(directions, faces, inverse, static_cast<std::size_t>(vertex_count), "native");
    require(same_bits(gains, {raw_staged, static_cast<std::size_t>(vertex_count)}),
            "gain replay differs from linked SAF");
    std::cout << "PASS full wrapper / staged SAF / spread replay / gain replay; seed=" << seed
              << " renderer_checked=" << (seed == 1U) << "\n";

    if (!reference.empty()) {
        auto ref_vertices = read_words<float>(reference / "geometry.01-vertices.f32");
        auto ref_faces = read_words<int>(reference / "geometry.02-faces.i32");
        const auto ref_inverse = read_words<float>(reference / "geometry.03-inverse.f32");
        const auto ref_directions = read_words<float>(reference / "spread.08-directions.f32");
        const auto ref_source = read_words<float>(reference / "input.01-source.f32");
        require(same_bits(source, ref_source), "reference must use identical source arguments");
        auto local_inverse = inverse_for(ref_vertices, ref_faces);
        dump("replay.01-inverse-fixed-geometry.f32", local_inverse);
        const auto count = ref_vertices.size() / 3U;
        gains_from(directions, ref_faces, local_inverse, count, "replay.02-fixed-geometry");
        gains_from(directions, ref_faces, ref_inverse, count, "replay.03-fixed-inverse");
        gains_from(ref_directions, ref_faces, local_inverse, count, "replay.04-fixed-spread");
        gains_from(ref_directions, ref_faces, ref_inverse, count, "replay.05-fixed-both");
        gains_from(ref_directions, ref_faces, ref_inverse, count, "replay.06-separate-021", DotMode::separate_021);
        gains_from(ref_directions, ref_faces, ref_inverse, count, "replay.07-fused-012", DotMode::fused_012);
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    if (args.size() < 3U || args.size() > 4U || std::getenv("MR_ADM_TRACE_DIR") == nullptr) {
        std::cerr
            << "usage: MR_ADM_TRACE_DIR=out mr_adm_vbap_probe <renderer-checkpoints> <seed> [reference-checkpoints]\n";
        return 2;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long seed = std::stoul(args[2], &consumed);
        require(consumed == args[2].size() && seed <= 0xffffffffUL, "invalid seed");
        probe(args[1], static_cast<unsigned int>(seed), args.size() == 4U ? Path(args[3]) : Path{});
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 2;
    }
    return 0;
}
