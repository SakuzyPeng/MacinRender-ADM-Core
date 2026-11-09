#pragma once
// Include shim for the frozen `tests/reference/live_vbap/legacy.h`, which included the former C++
// Scene VBAP creation header; renderer creation is Rust mradm-engine `scene::renderer` (PCM
// consistency 5.4 A) and the reference only needs the Scene renderer types.
#include <memory>

#include "live_scene_renderer.h"
