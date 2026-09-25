# Pi 3 integration validation — 2026-09-25

These results support review of the default configuration on
`dev/iec-cache-integration`. They are a record of the user's hardware runs,
not a claim of exhaustive compatibility or a controlled throughput benchmark.

## Configuration and provenance

The tested firmware source is commit
`0efe13915b825ddf253cdabcb29a900199a59287`. The Pi 3 builds used the
Arm GNU 13.3.1 toolchain. The saved card configuration identifies a Pi 3B/3B+
with a Pi1541io rev 4 split-IEC interface and SSD1306 OLED. SD card model,
host computer model, and Wheels version were not recorded.

Both diagnostic builds used `PICMD_EMMC_WAIT_DIAGNOSTICS=1`; only
`PICMD_EMMC_FAST_POLL` differed (0 for legacy, 1 for fast). Other switches
remained at their defaults: mid-cycle input sampling and VIA idle skip on,
port-read sampling and immediate fast-serial output off. The cache/preload
defaults were 24/18 MB, with `CMDHDEjectLog = 1` and
`AutoMountImage = hd0.img` on the card.

Each run directory includes the unedited eject report, the card's options,
and capture metadata with the installed kernel's SHA-256. The installed
kernel hash was verified against the corresponding build. Card filesystem
timestamps were not treated as the run time; `captured_at` is the host time.
No ROM, disk image, or kernel binary is included here.

## Observed behavior

- The user ran normal DOS commands and Wheels, reporting that Wheels ran
  better despite its usual quirks.
- A save/copy, eject, power-off, restart and reload test passed: the user
  confirmed that both the saved BASIC program and copied Wheels file survived.
- Following the autoboot/display change, the user reported successful file
  copying/moving and eject-log generation. The baseline report contained zero
  unflushed sectors and zero write errors during detach.
- The user completed both polling runs and reported the file-operation
  workload looked fine. Both diagnostic logs contain zero unflushed sectors,
  zero write errors during detach and zero wait timeouts.

An earlier legacy diagnostic build stalled at a screen showing the ROM header
and image name, with no button selection or drive response reported. The
precise cause was not isolated. After the autoboot/display update, the
subsequent legacy and fast diagnostic runs completed. This does not establish
that a specific display change fixed the earlier stall.

## SD wait measurements

| Measurement | Legacy polling | Fast polling |
| --- | ---: | ---: |
| Wait calls | 14,244 | 17,576 |
| Total wait time | 7,287,657 us | 1,412,075 us |
| Mean wait time (total / calls) | 511.63 us | 80.34 us |
| Maximum wait time | 4,956 us | 3,587 us |
| Wait timeouts | 0 | 0 |
| Unflushed sectors at detach | 0 | 0 |
| Write errors during detach | 0 | 0 |

Sources: [legacy report](legacy-20260925-161228/PICMD-EJECT.LOG) and
[fast report](fast-20260925-162436/PICMD-EJECT.LOG).

Mean wait duration was 84.3% lower (6.37 times shorter) in the fast run. The
call counts differ; identical operations, starting-image restoration and an
identical mix of waits were not independently verified. The figures support
keeping fast polling enabled on this tested setup, but do not measure total
file-copy time or prove the cause of the historical 35 ms access stalls.
Counters exclude initial mount/preload and the report's own writes. They
include the final detach and time spent in the blocking-wait hook.

## Limits and settings retained for review

Keep `PICMD_PORT_READ_SAMPLE=0` and `PICMD_FAST_SERIAL_PUSH=0`. Neither optional
experiment received an A/B hardware comparison. Normal builds keep wait
diagnostics off and eject logging opt-in. The user elected to finish bench
testing with the working default sampler and fast polling.

Separate GEOS/GDOS versions, companion SCSI images, marginal cards, sustained
stress, power-loss recovery and physical IEC setup/hold timing were not
established by these runs. A clean detach report does not rule out earlier
session errors or prove file contents byte-for-byte. Automated cache fault,
GPIO and VIA tests provide additional coverage, but cannot establish electrical
timing or all hardware compatibility. CI and code review remain separate gates.
