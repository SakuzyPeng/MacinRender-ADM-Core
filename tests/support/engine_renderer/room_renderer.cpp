
#include "adm/render_room.h"

#include "capability_records.h"
#include "room_internal.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {
using render_common::EnginePrepared;

// Sources, events and the room resources are prepared in Rust (mradm-engine `backend`,
// mradm-render `room`, PCM consistency 4.6, 5.2 C1); offline windows render in the engine over the
// file stream, which opens in mradm-engine `stream` (5.3 A) with its format check.
class RoomRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return room_capabilities(); }
    Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        const void* profile = plan.room.profile ? room_internal::Access::get(*plan.room.profile) : nullptr;
        auto prepared = render_common::prepare_backend(render_common::EngineBackend::room, plan, logs, profile);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }
    Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& state, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&state);
        if (prepared == nullptr) {
            return make_error(ErrorCode::invalid_argument, "room-binaural prepared 类型错误");
        }
        return render_common::open_engine_stream(*prepared, plan, &room_internal::control);
    }
};
} // namespace
CapabilityReport room_capabilities() {
    return render_common::engine_capabilities(render_common::EngineBackend::room);
}
std::unique_ptr<IRenderer> create_room_renderer() {
    return std::make_unique<RoomRenderer>();
}
} // namespace mradm
