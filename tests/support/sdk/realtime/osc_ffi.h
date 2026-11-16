#pragma once
#include <stddef.h>
#include <stdint.h>
// The PoseBridge loopback receiver in Rust mradm-osc. POD fields cross the private Rust boundary.
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
// Same field order as mradm::HeadTrackingTiming.
typedef struct MradmOscTiming {
    uint32_t protocol_version;
    uint32_t sample_time_kind;
    uint64_t instance_id;
    uint64_t tx_sequence;
    uint64_t reference_epoch;
    uint64_t metadata_revision;
    uint64_t source_age_at_send_ns;
    uint64_t source_session_id;
    uint64_t source_sequence;
    uint64_t source_received_ns;
    uint64_t sample_time_ms;
    uint64_t sample_clock_epoch;
} MradmOscTiming;
// cppcheck-suppress-end unusedStructMember

// ── Loopback receiver (Rust mradm-osc `receiver`) ────────────────────────────────────────────
typedef struct MradmOscReceiver MradmOscReceiver;
// Owned UTF-8 text; read with mradm_osc_text_view, release with mradm_osc_text_destroy.
typedef struct MradmOscText MradmOscText;
// cppcheck-suppress-begin unusedStructMember
// mradm::OscHeadTrackingSnapshot; flags are 0 or 1, state is OscHeadTrackingState.
typedef struct MradmOscSnapshot {
    int32_t state;
    uint32_t bound_port;
    uint32_t has_pose;
    uint32_t fresh;
    uint32_t has_heartbeat;
    uint32_t heartbeat_alive;
    uint32_t info_matches_pose;
    uint32_t status_matches_pose;
    uint64_t session_id;
    uint64_t sequence;
    uint64_t received_ns;
    uint64_t age_ms;
    uint64_t packets_received;
    uint64_t rejected_packets;
    uint64_t recovery_count;
    uint64_t pose_packets;
    uint64_t telemetry_packets;
    uint64_t ignored_sources;
    uint64_t protocol_mismatches;
    uint64_t missing_tx_packets;
    uint64_t heartbeat_age_ms;
    float quaternion_xyzw[4];
    float euler_deg[3];
    MradmOscTiming timing;
    size_t source_id_len;
    uint8_t source_id[256];
} MradmOscSnapshot;
// cppcheck-suppress-end unusedStructMember
size_t mradm_osc_text_view(const MradmOscText* text, const uint8_t** data);
void mradm_osc_text_destroy(MradmOscText* text);
// NULL for a panic. The source filter (may be empty) is checked by start.
MradmOscReceiver* mradm_osc_receiver_create(uint16_t port, const uint8_t* source, size_t source_len);
// Stops the worker and releases the port.
void mradm_osc_receiver_destroy(MradmOscReceiver* receiver);
// 0, or the adm::ErrorCode with its message in *message (owned by the caller).
int32_t mradm_osc_receiver_start(const MradmOscReceiver* receiver, MradmOscText** message);
void mradm_osc_receiver_stop(const MradmOscReceiver* receiver);
int32_t mradm_osc_receiver_snapshot(const MradmOscReceiver* receiver, MradmOscSnapshot* out);
// 0 with the JSON in *out, or 6 with the failure text in *out.
int32_t mradm_osc_receiver_snapshot_json(const MradmOscReceiver* receiver, MradmOscText** out);
int32_t mradm_osc_receiver_last_error(const MradmOscReceiver* receiver, MradmOscText** out);
#ifdef __cplusplus
}
#endif
