/* Headless control-thread consumer. Build against the current adm/c_api.h.
 * Replace on_pose/on_reference_change with the host's recenter/smoothing and
 * listener-orientation pipeline. Never run this polling loop in an audio callback.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "adm/c_api.h"
#ifdef _WIN32
#include <windows.h>
static void wait_ms(unsigned ms) {
    Sleep(ms);
}
#else
#include <time.h>
static void wait_ms(unsigned ms) {
    struct timespec value = {ms / 1000U, (long) (ms % 1000U) * 1000000L};
    nanosleep(&value, NULL);
}
#endif

static void on_reference_change(uint64_t instance, uint64_t reference) {
    /* Host decides whether to preserve/reacquire its listening-forward reference. */
    printf(
        "reference changed: instance=%llu epoch=%llu\n", (unsigned long long) instance, (unsigned long long) reference);
}
static void on_pose(const adm_head_tracking_pose_t* pose) {
    /* Update the host's target orientation here; use pose->yaw_deg/pitch_deg/roll_deg
     * with the existing listener setter AFTER host recenter/smoothing. */
    (void) pose;
}

int main(int argc, char** argv) {
    if (adm_api_version_major() != 1 || adm_api_version_minor() < 42) {
        fputs("MacinRender head-tracking ABI mismatch\n", stderr);
        return 1;
    }
    adm_osc_head_tracking_config_t config = {0};
    config.struct_size = sizeof(config);
    config.listen_port = 9000;
    config.source_id = argc > 1 ? argv[1] : NULL;
    if (argc > 2) {
        char* end = NULL;
        const unsigned long port = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || port > 65535UL) {
            return 1;
        }
        config.listen_port = (uint32_t) port;
    }
    adm_osc_head_tracking_t* receiver = NULL;
    if (adm_create_osc_head_tracking(&config, &receiver) != ADM_ERROR_OK) {
        return 1;
    }
    if (adm_osc_head_tracking_start(receiver) != ADM_ERROR_OK) {
        fprintf(stderr, "%s\n", adm_osc_head_tracking_last_error_message(receiver));
        adm_destroy_osc_head_tracking(receiver);
        return 1;
    }
    adm_osc_head_tracking_status_t status = {0};
    status.struct_size = sizeof(status);
    if (adm_osc_head_tracking_get_status(receiver, &status) != ADM_ERROR_OK) {
        adm_destroy_osc_head_tracking(receiver);
        return 1;
    }
    printf("PORT %u\n", status.bound_port);
    fflush(stdout);
    uint64_t last_instance = 0, last_reference = 0, last_sequence = 0, last_session = 0;
    unsigned updates = 0;
    int active = 0;
    for (unsigned tick = 0; tick < 1200; ++tick) {
        adm_head_tracking_pose_t pose = {0};
        pose.struct_size = sizeof(pose);
        if (adm_osc_head_tracking_get_pose(receiver, &pose) != ADM_ERROR_OK) {
            adm_destroy_osc_head_tracking(receiver);
            return 1;
        }
        if (!pose.has_pose || !pose.fresh) {
            if (active) {
                puts("stale: freeze the presented orientation and stop host keepalive");
            }
            active = 0;
        } else {
            if (last_instance != pose.instance_id || last_reference != pose.reference_epoch) {
                on_reference_change(pose.instance_id, pose.reference_epoch);
                last_instance = pose.instance_id;
                last_reference = pose.reference_epoch;
            }
            if (last_session != pose.source_session_id || last_sequence != pose.source_sequence) {
                on_pose(&pose);
                ++updates;
                last_session = pose.source_session_id;
                last_sequence = pose.source_sequence;
            }
            active = 1;
        }
        wait_ms(5);
    }
    char* json = NULL;
    if (adm_osc_head_tracking_snapshot_json(receiver, &json) == ADM_ERROR_OK) {
        puts(json);
        adm_free_string(json);
    }
    adm_osc_head_tracking_stop(receiver);
    adm_destroy_osc_head_tracking(receiver);
    return updates > 0 ? 0 : 2;
}
