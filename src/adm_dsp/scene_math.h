#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <utility>

// POD fields cross the private Rust boundary.
// cppcheck-suppress-begin unusedStructMember
extern "C" {
struct MradmSceneCloudPoint {
    float azimuth;
    float elevation;
    float weight;
    uint32_t slot;
};
struct MradmSceneSpeaker {
    float azimuth;
    float elevation;
    uint32_t is_lfe;
};
int mradm_dsp_scene_math(uint32_t, const float*, size_t, float*, size_t);
int mradm_dsp_scene_cloud(
    const float*, size_t, uint32_t, uint32_t, MradmSceneCloudPoint*, size_t, size_t*, float*, size_t);
int mradm_dsp_scene_divergence(const float*, size_t, uint32_t, MradmSceneCloudPoint*, size_t, size_t*);
int mradm_dsp_scene_nearest(const float*, size_t, uint32_t, const MradmSceneSpeaker*, size_t, size_t*, float*);
int mradm_dsp_scene_rotation_create(const float*, size_t, uint32_t, void**);
void mradm_dsp_scene_rotation_destroy(void*);
int mradm_dsp_scene_rotation_update(void*, const float*, size_t);
int mradm_dsp_scene_rotation_apply(const void*, const float*, size_t, float*, size_t, uint32_t);
int mradm_dsp_scene_rotate_pose(const float*, size_t, float*, size_t, uint32_t);
int mradm_dsp_scene_pose(const float*, size_t, float*, size_t, uint32_t);
}
// cppcheck-suppress-end unusedStructMember

namespace mradm::dsp {
#if defined(__aarch64__) && defined(__APPLE__) && !defined(MRADM_SCENE_STRICT_FP)
inline constexpr uint32_t scene_cpp_contract = 256U;
#else
inline constexpr uint32_t scene_cpp_contract = 0U;
#endif

inline void scene_check(int status) noexcept {
    if (status != 0) {
        std::terminate();
    }
}
template <size_t N, size_t M>
std::array<float, M> scene_math(uint32_t operation, const std::array<float, N>& input) noexcept {
    std::array<float, M> result{};
    // Operation IDs occupy the low byte; the high bit records the native arithmetic policy.
    scene_check(
        mradm_dsp_scene_math(operation + scene_cpp_contract, input.data(), input.size(), result.data(), result.size()));
    return result;
}
class SceneRotation {
  public:
    explicit SceneRotation(std::array<float, 3> pose) {
        void* raw = nullptr;
        scene_check(mradm_dsp_scene_rotation_create(pose.data(), pose.size(), scene_cpp_contract != 0 ? 1U : 0U, &raw));
        handle_.reset(raw);
    }
    void update(std::array<float, 3> pose) noexcept {
        scene_check(mradm_dsp_scene_rotation_update(handle_.get(), pose.data(), pose.size()));
    }
    std::pair<float, float> apply(float azimuth, float elevation, bool apple = false) const noexcept {
        const std::array<float, 2> input{azimuth, elevation};
        std::array<float, 2> result{};
        scene_check(mradm_dsp_scene_rotation_apply(
            handle_.get(), input.data(), input.size(), result.data(), result.size(), apple ? 1U : 0U));
        return {result[0], result[1]};
    }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_scene_rotation_destroy)> handle_{nullptr,
                                                                               mradm_dsp_scene_rotation_destroy};
};
} // namespace mradm::dsp
