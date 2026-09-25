#ifndef PICMD_SWITCHES_H
#define PICMD_SWITCHES_H

// Hardware bisection switches. Clean rebuild after changing a header.
#ifndef PICMD_VIA_IDLE_SKIP
#define PICMD_VIA_IDLE_SKIP 1
#endif
#ifndef PICMD_SECOND_SAMPLE
#define PICMD_SECOND_SAMPLE 1
#endif

// Compare polling strategies independently of the IEC experiments.
#ifndef PICMD_EMMC_FAST_POLL
#define PICMD_EMMC_FAST_POLL 1
#endif
// Adds two timer reads per wait and aggregates in RAM; no per-wait logging.
#ifndef PICMD_EMMC_WAIT_DIAGNOSTICS
#define PICMD_EMMC_WAIT_DIAGNOSTICS 0
#endif

#endif
