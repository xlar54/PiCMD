# Host tests

The emulation modules are pure logic — they reach hardware through a handful of
free functions and nothing else. That makes them testable on a desktop compiler
without an emulator, a Pi, or a serial cable.

```
cd tests
make                    build and run
make SANITIZE=1         with ASan and UBSan (Linux/macOS; MinGW has no libasan)
make clean
```

## How it works

The emulator sources are compiled **unmodified**. No `#ifdef TEST`, no
dependency injection, no test-only constructors. The substitution happens on
the include path:

```
-Ishims -I../src -I../src/emulation ...
```

`shims/` sits first, so `ff.h`, `rpihardware.h`, `debug.h` and `integer.h`
resolve there instead of in `src/`. The code under test cannot tell.

That property is worth defending. **If a header in `src/` ever has to change to
make a test work, the seam has moved and the design is drifting** — fix the
seam rather than the test.

| Shim | Stands in for | Why |
|---|---|---|
| `ff.h` / `ff.cpp` | FatFS | In-memory files, and every call can be made to fail. Short writes, hard errors, failing sync — the cases that matter are the ones a real card only does when it is dying. |
| `rpihardware.h` | `read32` / `write32` | Serves a fake clock, so a flush budget measured in microseconds can be tested without sleeping. |
| `fakeclock.*` | the system timer | Tests advance it by hand; the filesystem shim charges it per access, so an SD access can be made to "take" 35 ms. |
| `debug.h` | `DEBUG_LOG` | Quiet by default, switchable when a case is misbehaving. |
| `integer.h` | FatFS's | The real one takes an `#ifdef _WIN32 -> #include <windows.h>` branch that collides with uspi's `boolean`. |

## What is covered

`test_scsi_cache.cpp` — the write-back disk cache. Every case corresponds to a
bug that reached `main` and was found by reading rather than by running:

- a flushed write actually reaches the card, and a clean cache does no I/O
- a short write is a failure, and the data survives it
- a hard write failure keeps the data for a retry
- an uncached read of an unwritten sector comes from the card, not from
  uninitialised cache RAM
- a budgeted flush stops near its deadline, and leaves the rest dirty
- an unbudgeted flush finishes and marks the cache clean
- eviction does not discard what it could not write
- detach on a dead card reports rather than pretending

## Check that a test can fail

A suite that has only ever been green is not evidence of anything. Break
something on purpose and confirm it goes red — for instance, drop the
`written != run * SECTOR_SIZE` condition in `FlushChunk` and re-run:

```
FAIL  a short write is a failure, and the data survives it
        OnCard(5, SECTOR - 1) == 0x22
        got 171, expected 34
```

That `got 171` is the original fill byte still sitting on the card: the tail of
the sector never landed. Restore with `git checkout -- src/emulation/scsi.cpp`.

Do this for a case you have just written, before trusting it. The eviction test
here passed its first review while asserting nothing at all — it recovered the
card, flushed, and never looked at whether the data had survived, so clearing a
failed victim's dirty mask left all 135 checks green. A case that cannot fail is
worse than no case, because it reads as coverage.

## Worth knowing

These tests replace the hardware layer, so they **cannot** catch a bug in it.
The `disk_ioctl` stub that returned `RES_PARERR` for `CTRL_SYNC` — which made
every `f_sync` fail on real hardware, and every successful flush report itself
as a lost write — would pass all of this without complaint, because the fake
filesystem's `f_sync` returns `FR_OK`.

Tests raise the floor. They do not remove the need to check what a stubbed
layer actually does on the real thing.

## Adding more

The obvious next ones, in order of value:

1. **The SCSI command state machine.** Already a pure function of
   `(context, byte in) -> (byte out, state)`. Feed it command bytes, assert the
   phase transitions and the sense data. Would have caught `REQUEST SENSE`
   never sending its payload, the unreachable medium-not-present branch, and
   the `REASSIGN BLOCKS` write past `data_buf`.
2. **The 65C02 core**, against Klaus Dormann's 6502 functional test suite —
   one test, enormous coverage.
3. **Register-level tests** for the 6522, 8255A and RTC. Write a register, read
   it back, assert you get what you wrote. That is exactly the RTC register F
   bit mismatch.
