#include <stdint.h>

// Deliberately remains process-global to isolate algorithm/normalisation from lifecycle.
// This is a measurement control, not the production instance-seeding design.
static uint32_t diagnostic_state = 1U;

int mr_adm_diagnostic_rand(void) {
    diagnostic_state = diagnostic_state * 1664525U + 1013904223U;
    return (int) (diagnostic_state >> 1U);
}

void mr_adm_diagnostic_seed(unsigned int seed) {
    diagnostic_state = (uint32_t) seed;
}
