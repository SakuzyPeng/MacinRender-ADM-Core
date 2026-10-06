// Algorithm-level differential oracle: EBU libear 2db69f8f (Apache-2.0).
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <ear/ear.hpp>

#include "ear.h"
#include "speaker_layouts.h"

namespace {
struct Stats {
    size_t count{};
    double maximum{};
    void compare(double a, double b, double tolerance, const std::string& context) {
        const double error = std::abs(a - b) / (1.0 + std::abs(b));
        if (!std::isfinite(error) || error > tolerance) {
            throw std::runtime_error(context + " candidate=" + std::to_string(a) + " reference=" + std::to_string(b) +
                                     " error=" + std::to_string(error));
        }
        maximum = std::max(maximum, error);
        ++count;
    }
};
mradm::dsp::EarLayout project_layout(const mradm::render_layouts::SpeakerLayout& spec) {
    mradm::dsp::EarLayout result{std::string{spec.id}, {}};
    for (const auto& s : spec.speakers) {
        MradmEarChannel c{};
        mradm::dsp::ear_string(c.name, s.label);
        c.position[0] = s.azimuth;
        c.position[1] = s.elevation;
        c.position[2] = 1;
        std::copy(std::begin(c.position), std::end(c.position), c.nominal);
        c.azimuth_range[0] = s.azimuth_range ? s.azimuth_range->first : s.azimuth;
        c.azimuth_range[1] = s.azimuth_range ? s.azimuth_range->second : s.azimuth;
        c.elevation_range[0] = s.elevation_range ? s.elevation_range->first : s.elevation;
        c.elevation_range[1] = s.elevation_range ? s.elevation_range->second : s.elevation;
        c.lfe = s.is_lfe ? 1U : 0U;
        result.channels.push_back(c);
    }
    return result;
}
ear::Layout reference_layout(const mradm::dsp::EarLayout& layout) {
    std::vector<ear::Channel> channels;
    for (const auto& c : layout.channels) {
        channels.emplace_back(std::string{mradm::dsp::ear_channel_name(c)},
                              ear::PolarPosition{c.position[0], c.position[1], c.position[2]},
                              ear::PolarPosition{c.nominal[0], c.nominal[1], c.nominal[2]},
                              std::make_pair(c.azimuth_range[0], c.azimuth_range[1]),
                              std::make_pair(c.elevation_range[0], c.elevation_range[1]),
                              c.lfe != 0);
    }
    return {layout.name, std::move(channels)};
}
} // namespace
int main(int argc, char** argv) try {
    Stats point, extent, ds, hoa, fir;
    for (const auto geometry : {mradm::SpeakerGeometry::standard, mradm::SpeakerGeometry::apple}) {
        for (const auto& spec : mradm::render_layouts::speaker_layouts(geometry)) {
            const auto id = std::string{spec.id};
            auto current = (id == "4+5+4" || id == "9.1.6")
                               ? project_layout(*mradm::render_layouts::find_speaker_layout(id))
                               : mradm::dsp::ear_layout(id == "wav71" ? "0+7+0" : id);
            if (id != "4+5+4" && id != "9.1.6") {
                const auto original = ear::getLayout(current.name);
                if (original.channels().size() != current.channels.size()) {
                    throw std::runtime_error("layout count");
                }
                for (size_t i = 0; i < current.channels.size(); ++i) {
                    const auto& a = original.channels()[i];
                    const auto& b = current.channels[i];
                    if (a.name() != mradm::dsp::ear_channel_name(b) || a.polarPosition().azimuth != b.position[0] ||
                        a.polarPosition().elevation != b.position[1] || a.polarPosition().distance != b.position[2] ||
                        a.polarPositionNominal().azimuth != b.nominal[0] ||
                        a.polarPositionNominal().elevation != b.nominal[1] ||
                        a.polarPositionNominal().distance != b.nominal[2] ||
                        a.azimuthRange() != std::make_pair(b.azimuth_range[0], b.azimuth_range[1]) ||
                        a.elevationRange() != std::make_pair(b.elevation_range[0], b.elevation_range[1]) ||
                        a.isLfe() != (b.lfe != 0)) {
                        throw std::runtime_error("layout table");
                    }
                }
            }
            if (geometry == mradm::SpeakerGeometry::apple) {
                for (auto& c : current.channels) {
                    const auto s = std::ranges::find(
                        spec.speakers, mradm::dsp::ear_channel_name(c), &mradm::render_layouts::SpeakerSpec::label);
                    if (s == spec.speakers.end()) {
                        throw std::runtime_error("missing Apple speaker");
                    }
                    c.position[0] = s->azimuth;
                    c.position[1] = s->elevation;
                }
            }
            std::cerr << id << " geometry=" << static_cast<int>(geometry) << '\n';
            const auto ref = reference_layout(current);
            const size_t n = current.channels.size();
            mradm::dsp::EarCalculator candidate{current};
            ear::GainCalculatorObjects objects{ref};
            std::vector<double> a(n), b(n), c(n), d(n);
            for (double el : {-90.0, -60.0, -30.0, 0.0, 30.0, 60.0, 90.0}) {
                for (int az = -180; az <= 180; az += 15) {
                    for (const auto shape : std::array<std::array<double, 3>, 7>{{{0, 0, 0},
                                                                                  {1, 1, 0},
                                                                                  {90, 40, 0},
                                                                                  {40, 90, 0},
                                                                                  {180, 180, 0},
                                                                                  {360, 360, 0},
                                                                                  {30, 20, 1}}}) {
                        MradmEarObject m{{static_cast<double>(az), el, 1}, shape[0], shape[1], shape[2], 0.7, 0.3};
                        ear::ObjectsTypeMetadata old;
                        old.position = ear::PolarPosition{m.position[0], m.position[1], m.position[2]};
                        old.width = m.width;
                        old.height = m.height;
                        old.depth = m.depth;
                        old.gain = m.gain;
                        old.diffuse = m.diffuse;
                        candidate.objects(m, a, b);
                        objects.calculate(old, c, d);
                        auto& stats = shape[0] == 0 ? point : extent;
                        const double tolerance = shape[0] == 0 ? 1e-9 : 1e-5;
                        for (size_t ch = 0; ch < n; ++ch) {
                            const auto context = id + " objects az=" + std::to_string(az) +
                                                 " el=" + std::to_string(el) + " w=" + std::to_string(m.width) +
                                                 " ch=" + std::to_string(ch);
                            stats.compare(a[ch], c[ch], tolerance, context);
                            stats.compare(b[ch], d[ch], tolerance, context);
                        }
                    }
                }
            }
            for (double distance : {0.0, 0.1, 0.5, 2.0}) {
                MradmEarObject m{{17, 37, distance}, 40, 60, 2, 1, 0};
                ear::ObjectsTypeMetadata old;
                old.position = ear::PolarPosition{17, 37, distance};
                old.width = 40;
                old.height = 60;
                old.depth = 2;
                candidate.objects(m, a, b);
                objects.calculate(old, c, d);
                for (size_t i = 0; i < n; ++i) {
                    extent.compare(a[i], c[i], 1e-5, id + " distance");
                }
            }
            ear::GainCalculatorDirectSpeakers direct{ref};
            for (const auto& ch : ref.channels()) {
                for (const std::string pack : {"", "AP_00010003", "AP_00010009"}) {
                    for (bool labelled : {false, true}) {
                        MradmEarDirect m{};
                        const auto pos = ch.polarPositionNominal();
                        m.position[0] = pos.azimuth;
                        m.position[1] = pos.elevation;
                        m.position[2] = pos.distance;
                        std::vector<MradmEarLabel> labels;
                        ear::DirectSpeakersTypeMetadata old;
                        old.position = ear::PolarSpeakerPosition{pos.azimuth, pos.elevation, pos.distance};
                        if (labelled) {
                            labels.resize(1);
                            mradm::dsp::ear_string(labels[0], ch.name());
                            old.speakerLabels = {ch.name()};
                        }
                        if (labelled && !pack.empty()) {
                            m.present |= 256U;
                            mradm::dsp::ear_string(m.pack, pack);
                            old.audioPackFormatID = pack;
                        }
                        if (ch.isLfe()) {
                            m.present |= 64U;
                            m.low_pass = 120;
                            old.channelFrequency.lowPass = 120;
                        }
                        candidate.direct_speakers(m, labels, a);
                        direct.calculate(old, b);
                        for (size_t i = 0; i < n; ++i) {
                            ds.compare(a[i], b[i], 1e-9, id + " DS");
                        }
                    }
                }
            }
            // Off-grid, bounded, unknown-label and common-definition downmix cases.
            for (const std::string label :
                 {"U+090", "T+000", "M+060", "LFER", "urn:itu:bs:2051:12:speaker:M+030", "unknown"}) {
                for (const std::string pack : {"AP_00010009", "AP_00010004", "AP_00010017", "AP_0001000f"}) {
                    for (bool bounded : {false, true}) {
                        MradmEarDirect metadata{};
                        metadata.position[0] = 21.0;
                        metadata.position[1] = 9.0;
                        metadata.position[2] = 1.0;
                        metadata.present = 256U;
                        mradm::dsp::ear_string(metadata.pack, pack);
                        MradmEarLabel item{};
                        mradm::dsp::ear_string(item, label);
                        const std::array labels{item};
                        ear::DirectSpeakersTypeMetadata previous;
                        previous.speakerLabels = {label};
                        previous.audioPackFormatID = pack;
                        ear::PolarSpeakerPosition position{21.0, 9.0, 1.0};
                        if (bounded) {
                            metadata.present |= 15U;
                            metadata.bounds[0] = 10.0;
                            metadata.bounds[1] = 40.0;
                            metadata.bounds[2] = -1.0;
                            metadata.bounds[3] = 40.0;
                            position.azimuthMin = 10.0;
                            position.azimuthMax = 40.0;
                            position.elevationMin = -1.0;
                            position.elevationMax = 40.0;
                        }
                        previous.position = position;
                        candidate.direct_speakers(metadata, labels, a);
                        direct.calculate(previous, b);
                        for (size_t i = 0; i < n; ++i) {
                            ds.compare(a[i], b[i], 1e-9, id + " DS fallback " + label);
                        }
                    }
                }
            }
            ear::GainCalculatorHOA decoder{ref};
            for (const std::string normalization : {"SN3D", "N3D", "FuMa"}) {
                for (int order : {0, 1, 3, 6}) {
                    if (order == 6 && normalization == "FuMa") {
                        continue;
                    }
                    ear::HOATypeMetadata m;
                    m.normalization = normalization;
                    for (int degree = -order; degree <= order; ++degree) {
                        m.orders.push_back(order);
                        m.degrees.push_back(degree);
                    }
                    if (order != 0) {
                        m.orders.insert(m.orders.begin(), 0);
                        m.degrees.insert(m.degrees.begin(), 0);
                    }
                    const auto actual = candidate.hoa(m.orders, m.degrees, normalization);
                    std::vector<std::vector<double>> expected(m.orders.size(), std::vector<double>(n));
                    decoder.calculate(m, expected);
                    for (size_t i = 0; i < expected.size(); ++i) {
                        for (size_t j = 0; j < n; ++j) {
                            hoa.compare(actual[i * n + j], expected[i][j], 1e-9, id + " HOA");
                        }
                    }
                }
            }
            const auto actual = candidate.filters();
            const auto expected = ear::designDecorrelators<float>(ref);
            for (size_t i = 0; i < n; ++i) {
                for (size_t j = 0; j < 512; ++j) {
                    fir.compare(actual[i * 512 + j], expected[i][j], 2e-7, id + " FIR");
                }
            }
        }
    }
    std::ostream* stream = &std::cout;
    std::ofstream file;
    if (argc > 1) {
        file.open(argv[1]);
        stream = &file;
    }
    *stream << std::setprecision(17) << "{\n";
    const std::array stats{point, extent, ds, hoa, fir};
    const std::array names{"point", "extent", "direct_speakers", "hoa", "fir"};
    for (size_t i = 0; i < stats.size(); ++i) {
        *stream << '"' << names[i] << "\": {\"values\": " << stats[i].count
                << ", \"max_scaled_error\": " << stats[i].maximum << '}' << (i + 1 == stats.size() ? "\n" : ",\n");
    }
    *stream << "}\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
