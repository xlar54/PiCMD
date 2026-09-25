#ifndef PICMD_SWITCHES_H
#define PICMD_SWITCHES_H

// Hardware bisection switches. Clean rebuild after changing a header.
#ifndef PICMD_VIA_IDLE_SKIP
#define PICMD_VIA_IDLE_SKIP 1
#endif
#ifndef PICMD_SECOND_SAMPLE
#define PICMD_SECOND_SAMPLE 1
#endif

// Use a snapshot-guarded, port-only sample at the CPU's ORB read instead
// of the mid-cycle sample. Both zero select main's legacy live sampler.
#ifndef PICMD_PORT_READ_SAMPLE
#define PICMD_PORT_READ_SAMPLE 0
#endif

// Experimental: drive fast-serial DATA then SRQ after each 2 MHz update.
// Off until hardware establishes setup time, polarity and loop budget.
#ifndef PICMD_FAST_SERIAL_PUSH
#define PICMD_FAST_SERIAL_PUSH 0
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
