#pragma once

// Forced into SAF C translation units only for a diagnostic intervention.
// Both the recurrence and RAND_MAX must be identical; srand(1) alone cannot do that.
#include <stdlib.h>

int mr_adm_diagnostic_rand(void);
void mr_adm_diagnostic_seed(unsigned int seed);

#undef RAND_MAX
#define RAND_MAX 2147483647
#define rand mr_adm_diagnostic_rand
#define srand mr_adm_diagnostic_seed
