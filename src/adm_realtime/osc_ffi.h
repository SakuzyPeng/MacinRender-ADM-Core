#pragma once
#include <stddef.h>
#include <stdint.h>
// PoseBridge OSC decoding in Rust mradm-osc. POD fields cross the private Rust boundary.
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
// kind: 0 pose, 1 info, 2 status, 3 incompatible. json_offset/json_len index the decoded datagram.
typedef struct MradmOscMessage {
    uint32_t kind;
    uint32_t source_active;
    float quaternion_xyzw[4];
    float euler_deg[3];
    MradmOscTiming timing;
    uint64_t message_sequence;
    uint64_t reported_samples;
    size_t source_id_len;
    uint8_t source_id[256];
    size_t json_offset;
    size_t json_len;
} MradmOscMessage;
// Owned by the caller; zero-initialized is the initial state.
typedef struct MradmOscSourceOrder {
    MradmOscTiming last;
    MradmOscTiming clock;
    uint64_t retired[16];
    size_t retired_next;
    uint64_t last_gap;
} MradmOscSourceOrder;
// cppcheck-suppress-end unusedStructMember
// Returns 0 and fills out for a decoded datagram; nonzero rejects it.
int mradm_osc_decode(const uint8_t* data, size_t len, MradmOscMessage* out);
uint32_t mradm_osc_valid_source_id(const uint8_t* data, size_t len);
uint32_t mradm_osc_source_order_accept(MradmOscSourceOrder* order, const MradmOscTiming* timing);
uint32_t mradm_osc_source_order_retired(const MradmOscSourceOrder* order, uint64_t instance_id);
#ifdef __cplusplus
}
#endif
