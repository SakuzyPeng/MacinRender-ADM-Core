#include "render_common.h"

#include <algorithm>

namespace mradm::render_common {

PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                         const SceneObject& object,
                                         const std::vector<SceneOutputSpeaker>& speakers,
                                         LogSink& logs,
                                         std::string_view log_module,
                                         bool& screen_ref_warned) {
    SceneObjectBlock block = raw_block;
    if (object.position_offset) {
        block.position = apply_position_offset(block.position, *object.position_offset);
    }
    if (block.screen_ref && !screen_ref_warned) {
        logs.log(LogLevel::warning,
                 log_module,
                 "screenRef requires referenceScreen geometry; rendering block as screenRef=false");
        screen_ref_warned = true;
    }
    block = apply_channel_lock(block, speakers);

    return {
        expand_object_divergence(block),
        raw_block.start_sample,
        std::min(raw_block.end_sample, object.end_sample),
        raw_block.jump_position,
        raw_block.interp_length_samples,
    };
}

} // namespace mradm::render_common
