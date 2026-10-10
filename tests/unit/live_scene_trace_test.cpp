#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "../../src/adm_render_common/live_scene_trace.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TraceDirectory {
  public:
    TraceDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("mradm-scene-trace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        require(std::filesystem::create_directory(path_), "create trace directory");
    }
    ~TraceDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TraceDirectory(const TraceDirectory&) = delete;
    TraceDirectory& operator=(const TraceDirectory&) = delete;
    TraceDirectory(TraceDirectory&&) = delete;
    TraceDirectory& operator=(TraceDirectory&&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

std::vector<std::uint32_t> words(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "checkpoint exists");
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    require(!bytes.empty() && bytes.size() % 4U == 0U, "checkpoint has complete words");
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < bytes.size(); i += 4U) {
        result.push_back(static_cast<std::uint32_t>(bytes[i]) | (static_cast<std::uint32_t>(bytes[i + 1U]) << 8U) |
                         (static_cast<std::uint32_t>(bytes[i + 2U]) << 16U) |
                         (static_cast<std::uint32_t>(bytes[i + 3U]) << 24U));
    }
    return result;
}
} // namespace

int main() {
    try {
        const TraceDirectory directory;
#ifdef _WIN32
        require(_putenv_s("MR_ADM_TRACE_DIR", directory.path().string().c_str()) == 0, "set trace directory");
#else
        require(setenv("MR_ADM_TRACE_DIR", directory.path().string().c_str(), 1) == 0, "set trace directory");
#endif
        namespace trace = mradm::consistency;
        mradm::live_scene::Frame frame;
        frame.epoch_id = 1;
        frame.generation_id = 2;
        frame.media_sample_start = 8192;
        const auto current = trace::scene_renderer_key(frame);
        std::string outgoing;
        std::string incoming;
        {
            const trace::SceneRendererTraceScope scope("outgoing");
            outgoing = trace::scene_renderer_key(frame) + ".30-binaural-commands.i32";
            trace::dump(outgoing, {111});
            {
                const trace::SceneRendererTraceScope nested("incoming");
                incoming = trace::scene_renderer_key(frame) + ".30-binaural-commands.i32";
                trace::dump(incoming, {222});
            }
            require(trace::scene_renderer_key(frame) + ".30-binaural-commands.i32" == outgoing,
                    "nested scope restores outgoing renderer");
        }
        require(trace::scene_renderer_key(frame) == current, "scope restores current renderer");
        require(outgoing != incoming, "same-backend crossfade keys are distinct");
        require(words(directory.path() / outgoing) == std::vector<std::uint32_t>{111}, "outgoing command retained");
        require(words(directory.path() / incoming) == std::vector<std::uint32_t>{222}, "incoming command retained");

        const auto first = trace::scene_renderer_key(frame) + ".30-triple-lane-kinds.i32";
        trace::dump(first, {1});
        frame.epoch_id = 2;
        const auto second = trace::scene_renderer_key(frame) + ".30-triple-lane-kinds.i32";
        trace::dump(second, {2});
        require(words(directory.path() / first) == std::vector<std::uint32_t>{1}, "first epoch retained");
        require(words(directory.path() / second) == std::vector<std::uint32_t>{2}, "reused generation retains epoch");

        mradm::live_scene::StateEntry entry;
        entry.element_id = 0x1234567800000001ULL;
        entry.state.x = -0.0F;
        const auto state = trace::scene_slice_key(frame) + ".15-producer";
        trace::dump_states(state, std::span{&entry, 1U}, {});
        const auto values = words(directory.path() / (state + ".f32"));
        const auto fields = words(directory.path() / (state + "-fields.i32"));
        require(values.size() == 12U && values[1] == 0x80000000U, "state signed zero is preserved");
        require(fields.size() == 15U && fields[1] == 1U && fields[2] == 0x12345678U, "full element id is preserved");
        std::cout << "Scene trace scopes, epochs and word streams passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
