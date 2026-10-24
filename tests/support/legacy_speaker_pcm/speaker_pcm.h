#pragma once
// The frozen Triple Balance reference (tests/reference/triple_balance) includes "speaker_pcm.h"
// for mix types that left production in PCM consistency 4.6; this directory comes first on its
// include path.
#include "../../../src/adm_render_common/speaker_pcm.h"
#include "../speaker_mix.h"
