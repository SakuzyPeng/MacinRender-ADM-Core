#pragma once
// Include shim for the frozen `tests/reference/scene_numeric/live_binaural.h`, which included the
// former C++ Scene binaural creation header; renderer creation is Rust mradm-engine
// `scene::renderer` (PCM consistency 5.4 A) and the reference only needs the Scene renderer types.
#include <memory>

#include "live_scene_renderer.h"
