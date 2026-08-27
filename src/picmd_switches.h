// Pi-CMD build switches.
//
// They live in their own header, included from types.h, for a reason that cost
// an afternoon twice. They were first put in picmdhd.h, where m6522.h could not
// see them, so the VIA counters silently compiled to nothing and the build only
// broke because a caller referenced them. Moving them to m65c02.h fixed that
// and broke scsi.h the same way - scsi.h does not include m65c02.h - so
// PICMD_ACCESS_FORENSICS evaluated to 0 there whatever it was set to, and the
// "off" build was right by accident rather than by construction.
//
// An #if on a macro the translation unit cannot see is not a switch. It is a
// deletion wearing a switch's clothes. types.h is included by everything, so
// this is the one place they cannot go wrong.
//
// After changing any of these: touch the .cpp files or build clean. The
// Makefile tracks no header dependencies, so editing a header alone relinks
// the previous objects and produces a kernel that does not contain the change.

#ifndef PICMD_SWITCHES_H
#define PICMD_SWITCHES_H

// Diagnostic split of the emulation cost: per-section cycle attribution inside
// cpu+vias, the sticky VIA idle bits, and the blocking-read micro-benchmark.
// Off in anything you would deploy.
#define PICMD_SPLIT_CPUVIAS 0

// Bisection switches. Both default on; turning one off builds an earlier stage
// from the SAME source, so the three test kernels differ only in the change
// under test and not in their instrumentation. Comparing logs between builds
// that differ in more than one thing is how this investigation lost a week.
//   1 = skip an idle VIA in m6522::Execute
#define PICMD_VIA_IDLE_SKIP 1
//   1 = take the trimmed second look at the bus between the emulated cycles
#define PICMD_SECOND_SAMPLE 1

// Access forensics: the per-access address log and the payload ring, with the
// PICMD-LBAnn.LOG and PICMD-WRnn.LOG dumps they feed. They cost 256KB of BSS -
// accessLog and writeLogData are 128KB each, together 47% of the kernel's BSS
// and the largest objects in it - and that comes straight out of the heap the
// disk cache draws on. Off unless a run needs to know what the computer asked
// for, which is a debugging question and not a running one.
#define PICMD_ACCESS_FORENSICS 0

// Section attribution of the emulation loop: five coprocessor reads and four
// 64 bit accumulations per iteration, measured at about 30 cycles against a
// budget of 1200. The cheap counters - the overrun count, the histogram, the
// worst case - stay compiled in either way, because they are the drive's
// health meter and they only run on an iteration that already overran.
#define PICMD_SECTION_PROBES 0

#endif
