// Research-only driver. Comparison PCM must be produced by its Release build.
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "room_compat_size_panner.h"
#include "room_compat_size_processor.h"

int main(int argc, char** argv) {
    if (argc == 2) {
        std::ifstream configuration(argv[1]);
        const auto request = nlohmann::json::parse(configuration);
        std::vector<mradm::room_compat::SizeEvent> events;
        for (const auto& event : request.at("events")) {
            const auto& xyz = event.at("xyz");
            events.push_back({event.at("start_sample").get<uint64_t>(),
                              {xyz[0].get<float>(), xyz[1].get<float>(), xyz[2].get<float>()},
                              event.at("size").get<float>()});
        }
        auto processor = mradm::room_compat::SizeObjectProcessor::create(events, request.at("layout"), 48000);
        if (!processor) {
            std::cerr << processor.error().message << '\n';
            return 1;
        }
        std::ifstream input(request.at("input_f32").get<std::string>(), std::ios::binary);
        std::ofstream output(request.at("output_f32").get<std::string>(), std::ios::binary);
        if (!input || !output) {
            return 1;
        }
        std::vector<float> chunk(request.value("block_frames", 1024U));
        std::vector<float> result;
        while (input) {
            input.read(reinterpret_cast<char*>(chunk.data()),
                       static_cast<std::streamsize>(chunk.size() * sizeof(float)));
            const auto count = static_cast<std::size_t>(input.gcount()) / sizeof(float);
            result.clear();
            auto status = processor->push(std::span<const float>(chunk.data(), count), result);
            if (!status) {
                std::cerr << status.error().message << '\n';
                return 1;
            }
            output.write(reinterpret_cast<const char*>(result.data()),
                         static_cast<std::streamsize>(result.size() * sizeof(float)));
        }
        result.clear();
        if (!processor->finish(result)) {
            return 1;
        }
        output.write(reinterpret_cast<const char*>(result.data()),
                     static_cast<std::streamsize>(result.size() * sizeof(float)));
        return output.good() ? 0 : 1;
    }
    mradm::room_compat::QuantizedSizeParameters parameters;
    std::cout << std::setprecision(10);
    while (std::cin >> parameters.xyz[0] >> parameters.xyz[1] >> parameters.xyz[2] >> parameters.size) {
        const auto gains = mradm::room_compat::raw_size_gains(parameters);
        if (!gains) {
            std::cerr << gains.error().message << '\n';
            return 1;
        }
        for (const auto gain : *gains) {
            std::cout << gain << ' ';
        }
        std::cout << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
