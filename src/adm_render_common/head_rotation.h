#pragma once
#include "adm/options.h"

#include "../adm_dsp/scene_math.h"
namespace mradm::render_common {
class HeadRotation {
  public:
    explicit HeadRotation(const ListenerOrientation& orientation)
        : rotation_({orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg}) {}
    void update(const ListenerOrientation& orientation) noexcept {
        rotation_.update({orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg});
    }
    [[nodiscard]] std::pair<float, float> rotate_az_el(float azimuth, float elevation) const noexcept {
        return rotation_.apply(azimuth, elevation);
    }

  private:
    dsp::SceneRotation rotation_;
};
} // namespace mradm::render_common
