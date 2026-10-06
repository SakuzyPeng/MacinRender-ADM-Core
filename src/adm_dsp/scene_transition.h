#pragma once
#include "scene_math.h"
extern "C" {
struct MradmSceneTransitionStatus {
    uint64_t backend_position;
    uint64_t generation_position;
    uint64_t generation_remaining;
};
int mradm_dsp_scene_transition_create(size_t, uint32_t, uint64_t, void**);
void mradm_dsp_scene_transition_destroy(void*);
int mradm_dsp_scene_transition_control(void*, uint32_t);
int mradm_dsp_scene_transition_status(const void*, MradmSceneTransitionStatus*);
int mradm_dsp_scene_transition_mix(void*, float*, size_t, const float*, size_t, size_t, uint32_t*);
int mradm_dsp_scene_transition_output(void*, float*, size_t, size_t, uint32_t);
int mradm_dsp_scene_transition_snapshot(const void*, float*, size_t, float*, size_t, MradmSceneTransitionStatus*);
}
namespace mradm::dsp {
class SceneTransitions {
  public:
    SceneTransitions(size_t channels, uint32_t rate, uint64_t fade_frames) {
        void* raw = nullptr;
        scene_check(mradm_dsp_scene_transition_create(channels, rate, fade_frames, &raw));
        handle_.reset(raw);
    }
    void reset() noexcept { scene_check(mradm_dsp_scene_transition_control(handle_.get(), 0)); }
    void reset_backend() noexcept { scene_check(mradm_dsp_scene_transition_control(handle_.get(), 1)); }
    void begin_generation() noexcept { scene_check(mradm_dsp_scene_transition_control(handle_.get(), 2)); }
    MradmSceneTransitionStatus status() const noexcept {
        MradmSceneTransitionStatus result{};
        scene_check(mradm_dsp_scene_transition_status(handle_.get(), &result));
        return result;
    }
    bool mix(std::span<float> old, std::span<const float> incoming, size_t frames) noexcept {
        uint32_t done = 0;
        scene_check(mradm_dsp_scene_transition_mix(
            handle_.get(), old.data(), old.size(), incoming.data(), incoming.size(), frames, &done));
        return done != 0;
    }
    void process_output(std::span<float> pcm, size_t frames, bool silence) noexcept {
        scene_check(
            mradm_dsp_scene_transition_output(handle_.get(), pcm.data(), pcm.size(), frames, silence ? 1U : 0U));
    }
    const void* get() const noexcept { return handle_.get(); }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_scene_transition_destroy)> handle_{nullptr,
                                                                                 mradm_dsp_scene_transition_destroy};
};
} // namespace mradm::dsp
