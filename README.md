# Pi-CMD

A real-time, cycle exact CMD-HD hard drive emulator for the Raspberry Pi,
based on Stephen White's [Pi1541](https://github.com/pi1541/Pi1541).

Where Pi1541 emulated a Commodore 1541/1581 floppy drive, Pi-CMD emulates the
Creative Micro Designs (CMD) HD series SCSI hard drive. It runs the **original
CMD boot ROM and HDOS on emulated hardware**, so every HDOS command, partition
type and behaviour works exactly as it does on the real drive;-

- R65C02 CPU @ 2MHz (cycle stepped, all Rockwell opcodes)
- U10 6522 VIA (IEC bus, ATN auto-acknowledge, fast serial shift register)
- U9 6522 VIA + U13 PLD (SCSI data bus and phase handshaking)
- U11 8255A PPI (front panel buttons, SCSI control, memory banking)
- U20 latch (LEDs, RAM write protection)
- RTC-72421 real time clock
- 64K RAM, 16K boot ROM
- SCSI hard disk(s) backed by DHD image files streamed from the SD card

The hardware model follows VICE's CMD-HD emulation (cmdhd.c/scsi.c by
Roberto Muscedere), rebuilt on Pi1541's bare-metal cycle-exact framework.

## Hardware

The board this is developed and tested on is **hackup's
[Pi1541io](https://github.com/hackup/Pi1541io)**, rev 4 in particular. It is
split line ("Option B") hardware with a 7406 buffer, BSS138 bidirectional level
shifters on every IEC line, five buttons and an I2C header for an OLED. For
that board:

```
splitIECLines = 1
CMDHDAtnOutGPIO = 24
```

Other Pi1541 compatible interfaces should work for ordinary drive operation,
but two things are board specific and worth checking before you build a card:

- **Line inversion.** `invertIECInputs` / `invertIECOutputs` - the defaults suit
  a 7406; use `invertIECOutputs = 0` for a 7407.
- **Driving ATN.** Needed only so the SWAP buttons can reprogram whichever
  drive currently owns device 8 or 9. Which pin, and whether the board can do
  it at all, varies - see [Swapping device numbers](#swapping-device-numbers).

A Pi 3B or 3B+ is recommended; see [Notes and limitations](#notes-and-limitations).

## What you need on the SD card

1. **A CMD HD boot ROM.** Copy a dump of the CMD HD boot ROM (v2.80
   recommended; 16K, or the common 32K dump containing the image twice) to
   the root of the SD card named `cmdhd-bootrom.bin`.
   Alternative names found automatically: `bootromCMDHD-v2-80.bin`,
   `cmd_hd_bootrom_v2.80.bin`, `cmdhd.rom` — or set `CMDHDRomName` in
   options.txt.

2. **A disk image** - a file of zeros, which is what a blank SCSI drive looks
   like to a CMD HD.

   **Create it at the size you want the drive to be.** The file *is* the
   drive's capacity: the emulator refuses any write past the end of it, and
   HD-TOOLS will not grow it for you. Sectors are 512 bytes, so make the size a
   multiple of 512.

   250MB is a sensible starting point - roughly 1,400 1541 disks' worth:

   ```
   Linux, macOS      truncate -s 250M hd0.dhd
                     (or: dd if=/dev/zero of=hd0.dhd bs=1M count=250)

   Windows           fsutil file createnew hd0.dhd 262144000

   PowerShell        $f = [IO.File]::Create("$PWD\hd0.dhd")
                     $f.SetLength(250MB); $f.Close()
   ```

   Put it in the `/cmd-images` folder. Both `.dhd` and `.img` extensions are
   recognised. A blank image then needs HDOS installing onto it - see
   [Installing HDOS](#installing-hdos).

   The card must be **FAT32**: the Pi's boot ROM only understands
   FAT12/16/32, so an exFAT card will not boot at all. FAT32 cannot hold a
   file of 4GB or more, so keep each image below that - and for more storage
   than one image can hold, add further SCSI units (point 3) rather than
   reaching for exFAT.

3. Optionally, additional SCSI units: files named like the main image but
   with extension `.s<ID><LUN>` (VICE convention, e.g. `HD0.s10` for SCSI
   ID 1 LUN 0) are attached automatically.

## Using it

- Select the DHD image in the file browser, or mount it at power up with
  `AutoMountImage = HD0.dhd` in options.txt.
- The drive then boots the real boot ROM which loads HDOS from the image -
  just like a real CMD HD spinning up. Give it a few seconds.
- The drive's device number comes from the image itself, the way a real CMD HD
  keeps it on the disk rather than in a jumper. Change it with
  CMD's HD-TOOLS in configuration mode, or with `U0>`. To force one regardless, set `CMDHDDeviceID = 8` (or any other number)
  in options.txt and the configuration block is patched on the fly, the way
  VICE does it.
- Every HDOS command works, because HDOS itself is what runs them - see
  [Commands](#commands) below.
- An IEC bus reset resets the emulated drive (like a real HD) and stays in
  emulation. Exit back to the browser with button 5 / ESC.

### Commands

There is no command list for this branch to maintain, and that is rather the
point. Pi-CMD does not reimplement the drive: it emulates the CMD HD's hardware
and runs the genuine boot ROM, which loads the genuine HDOS from the image, so
**every command a real CMD HD understands works and behaves as the CMD HD
manual describes it.** That includes a good deal Pi1541 never supported:

- **Partitions.** `CP` / `C<Shift-P>` to select one, and the full partition
  table: native, 1541, 1571, 1581, 81-C, PBUF and FORN types.
- **Native mode subdirectories.** `CD`, `MD`, `RD` with true subdirectories,
  not FAT folders pretending to be them.
- **Direct block access.** `U1`/`U2`, `B-R`/`B-W`, `B-P`, `B-A`, `B-F`.
- **Memory commands.** `M-R`, `M-W`, `M-E`, against the drive's real RAM.
- **REL files**, with `P` positioning.
- **Real time clock.** `T-RA`/`T-WA` (ASCII), `T-RB`/`T-WB` (BCD),
  `T-RD`/`T-WD` (decimal).
- **Software device swap.** `S-8` / `S-9`, the software equivalent of the front
  panel SWAP buttons.
- **Software write protect.** `W-1` / `W-0`.
- `N` (format), `S` (scratch), `R` (rename), `C` (copy), `V` (validate),
  `I` (initialise), `U0>` (device number), `UI`/`UJ` (reset).
- **JiffyDOS**, which HDOS implements itself, and C128 burst/fast serial.
- **GEOS**, using the GEOS/HD driver HD-TOOLS installs alongside HDOS.

### Front panel buttons

The real CMD HD has four front panel buttons: **SWAP 8**, **SWAP 9**,
**WRITE PROTECT** and **RESET**. While emulating, they map onto the board's
buttons like this (all remappable in options.txt);-

| Board button | CMD HD function |
|---|---|
| 1 | SWAP 8 |
| 2 | SWAP 9 |
| 3 | WRITE PROTECT |
| 4 | RESET |
| 5 | exit emulation, back to the file browser (not a CMD HD button) |

```
//CMDHDButtonSwap8 = 1
//CMDHDButtonSwap9 = 2
//CMDHDButtonWP = 3
//CMDHDButtonReset = 4
//CMDHDButtonExit = 5
```

Set any of them to `0` to disable that function. ESC on a USB keyboard also
exits emulation, so button 5 can be freed up if you prefer.

SWAP 8, SWAP 9 and WRITE PROTECT are **momentary**, exactly like the real
front panel - the button state is presented to HDOS through the 8255A and
HDOS decides what a press means (swapping the drive's device number to 8 or
9, toggling write protection, and so on).

RESET takes effect when the button is **released**. That is what makes the
hardware's start up combinations work: hold WRITE PROTECT (or a SWAP button)
down, tap RESET, and the drive samples the held buttons as it boots, exactly
as it would on the real front panel.

These buttons are independent of the file browser mappings (`buttonEnter`,
`buttonUp`, ...), which still apply while you are choosing an image.

`S-8`/`S-9` and `W-1`/`W-0` do in software what SWAP and WRITE PROTECT do in
hardware. Note the manual's caveat that a write protect applied from the front
panel cannot be released by software.

### Installing HDOS

HDOS lives **inside** the image, as it does on the real drive - there is no
separate HDOS file to copy. A blank image therefore needs it installed once,
with CMD's HD-TOOLS, the same as commissioning a new SCSI drive on real
hardware. Once that is done the image boots on its own and you can create
partitions in the normal way.

**Installation mode** is what the drive enters to allow that, and it is reached
the way the real front panel reaches it: hold SWAP 8 + SWAP 9 while tapping
RESET. The status row shows `INSTALL MODE` once you are in it, and the drive
answers on device 30. The emulator also holds those two buttons for you at
power up when the image is smaller than 144 sectors (73,728 bytes) - too small
to be a real drive, so it assumes a fresh one. An image of any useful size is
well past that threshold and will not trigger it.

From CMD's HD-TOOLS disk, in this order:

1. **LLFORMAT** - low level format.
2. **CREATE SYS** - writes the device table and partition table, then chains
   to **REWRITE DOS**, which installs HDOS and the GEOS driver. Both files must
   be on the same floppy.

CREATE SYS sets the device number to 12, the factory default. It talks to
device 30 throughout, so the drive staying on 30 during the install is correct;
it moves to 12 on the reset afterwards.

This has been done successfully on a 40MB image. Note that a blank image is
scanned end to end at mount and again by LLFORMAT - there is no configuration
block to find, so the search runs the whole disk - which on a large image takes
a noticeable while. The status row shows `SCANNING nn%` while it happens. Once
HDOS is installed the same scan stops within the first percent and mounting is
quick from then on.

### CMD HD settings in options.txt

```
// Name of the CMD HD boot ROM file
//CMDHDRomName = cmdhd-bootrom.bin

// Force the drive's device number (0 = respect what is stored in the image).
// Only works once the image HAS a stored number - see "Device numbers" below.
//CMDHDDeviceID = 0

// Size in MB of the RAM cache in front of the DHD image (default 32)
//CMDHDCacheMB = 32

// GPIO used to pull the IEC ATN line low (0 = off, 24 on a Pi1541io).
// Needed only so the SWAP buttons can reprogram another drive - see
// "Swapping device numbers" below.
//CMDHDAtnOutGPIO = 24

// Show the front panel lamps on the I2C LCD/OLED (1 = on, the default)
//CMDHDLcdLamps = 1
```

### Device numbers

The drive's device number is a **configuration parameter stored on the disk**,
not a jumper. The manual puts it plainly: "DEFAULT DEVICE NUMBER - the device
number of the HD after power on or reset. This has been preset to 12."

That has a consequence worth understanding before you commission a fresh image:

| Image | Normal boot | Installation or configuration mode |
|---|---|---|
| Blank | **device 30** (no HDOS to run) | **device 30** |
| HDOS installed | its stored number, 12 unless changed | **device 30** |

**Device 30 means the boot ROM has the bus rather than HDOS**, and that happens
for two different reasons. Installation mode (SWAP 8 + SWAP 9) and
configuration mode (WRITE PROTECT) both put it there deliberately, on a drive
that has HDOS - that is how you get back in to change settings. But a blank
image lands there too, and stays: there is no HDOS on the disk to hand over to.
So a fresh image sits on device 30 from the moment you power it up, without
touching a button, and that is how HD-TOOLS reaches a drive that has never been
installed.

The status row tells you which of the three you are in, from the buttons that
were held at the reset that started the drive - which is the same thing the
real front panel goes by, since it only samples them as the drive comes up:

| Shows | Means |
|---|---|
| `INSTALL MODE` | SWAP 8 + SWAP 9 were held. Ready for HD-TOOLS. |
| `CONFIG MODE` | WRITE PROTECT was held. |
| `NO INSTALL` | Neither. The drive is on 30 only because the image has no HDOS. |

Nothing here is simulated: the boot ROM decides all of it. Pi-CMD passes the
buttons through the 8255A exactly as the panel wires them, and reads the
resulting device number back out of HDOS's zero page.

**`CMDHDDeviceID` cannot help here.** It works by patching the configuration
block as the drive reads it, so with no block there is nothing to patch and the
setting is silently ignored. Use installation mode. Once HD-TOOLS has written a
configuration block the override behaves normally.

### The front panel lamps on an LCD

If you have an I2C display configured (`LCDName`), the drive's six indicator
lamps are shown on it while emulating:

```
POWER   ACTIVE
ERROR   WR PROT
DRIVE 8 DRIVE 9
<track / temperature>
```

These OLED panels are **monochrome**, so a lit lamp cannot be a different
colour - it is drawn as an **inverse video block** (a solid bar with the name
knocked out of it), which is easy to read at a glance. Unlit lamps are plain
text.

POWER is lit whenever the drive is running. The other five come straight from
the U20 latch, so they behave exactly as the real panel does - including the
SWAP lamps lighting when the drive has swapped to device 8 or 9, WR PROT
following the write protect state, and the lamps being used as a binary digit
display in FPPS mode.

On a 128x32 display, or any panel with room for fewer than four text rows,
the layout falls back to short tags (`PWR ACT ERR WP` / `D8 D9`) and the
track line is dropped.

Set `CMDHDLcdLamps = 0` to leave the LCD showing only what Pi1541 showed.

### Swapping device numbers

The SWAP 8 / SWAP 9 buttons make the drive become device 8 or 9. On real
hardware the HD also reprograms whichever drive currently owns that number,
handing it the HD's own device number - and it does that by taking control of
the serial bus, which requires driving **ATN**.

Pi1541 never drove ATN, so whether your board can do so at all is board
specific, and the pin differs:

| Board | ATN output |
|---|---|
| [Pi1541io](https://github.com/hackup/Pi1541io) (hackup) | GPIO 2 (header pin 3) |
| Pi1541 documented split line pinout | GPIO 12 (header pin 32) |

Set `CMDHDAtnOutGPIO` accordingly. It is **0 (disabled) by default**, because
driving the wrong pin is worse than not driving one at all - and on a Pi1541io
rev 4, GPIO 2 is shared with I2C1 SDA, so it must not be combined with a
display on I2C bus 1.

With it disabled (or if your board has no ATN driver) the drive still swaps
its *own* device number correctly. What it cannot do is move another drive out
of the way, so if you press SWAP 8 while a real drive 8 is on the bus you will
end up with two devices answering at 8 - which looks like a corrupted
directory. In that situation either use SWAP 9, or simply give the HD device 8
permanently (with HD-TOOLS, or `CMDHDDeviceID = 8`)
and move the other drive.

#### How the reprogramming works

HDOS reprograms the other drive by becoming bus controller and sending it the
plain `M-W` command, writing the new listen/talk addresses into its zero page
at `$0077`/`$0078`. The command template is literally in HDOS at `$F2BF`
(`"M-W" $77 $00 $02`), and the sender at `$F296` works like this:

  1. `ORB=0` - release everything and clear ATNA, so the drive will not
     acknowledge its own ATN. This also drives pb6 low, which asserts ATN.
  2. `DDRB=$7A` - make pb6 an output, so ATN actually goes low.
  3. delay about a millisecond.
  4. check VIA `PB0`: did any device pull DATA low in acknowledgement?
  5. if not, abort via `$F295` (restore `DDRB=$3A`) and give up.

So the drive will only proceed if something answers its ATN. `CMDHDAtnOutGPIO`
must therefore name a pin that can really pull ATN low, or step 4 always
fails and the swap silently does nothing.

ATN is open collector, so the pin is driven the way Pi1541 drives its non
split lines: the output latch is always 0, so making the pin an output pulls
the line low and making it an input again releases it. The pin is never driven
high, so it cannot fight the host or another device.

On a Pi1541io rev 4 no board modification is needed: every IEC line reaches the Pi
through a BSS138 bidirectional level shifter (Q1-Q5 with 10k pull ups), so the
same pin that reads ATN can drive it. Use `CMDHDAtnOutGPIO = 24`.

## What goes on the SD card

**One FAT32 partition spanning the whole card.** There is no second partition
and nothing resembling the Raspbian layout. Everything lives in the root
except the images:

| File | What it is |
|---|---|
| `bootcode.bin`, `start.elf`, `fixup.dat` | Raspberry Pi firmware |
| `config.txt` | **Required.** Must contain `kernel_address=0x1f00000`, which is where this kernel links. Without that line the firmware loads it at the default address and the Pi sits on the rainbow test screen. |
| `kernel.img` | Pi-CMD |
| `cmdhd-bootrom.bin` | CMD HD boot ROM (16K, or a 32K dump). **Supply your own** - it is not in this repository. |
| `options.txt` | Configuration |
| `cmd-images/*.dhd` | Your images |

Nothing else is needed: no `.dtb`, no kernel command line, no second
partition. Note that Windows only formats up to 32GB as FAT32 from Explorer;
for a larger card use Rufus, fat32format/guiformat, or format it elsewhere.

The Raspberry Pi boot files are kept in **[firmware/3b/](firmware/3b/)** so
they cannot get lost, along with a matching `options.txt` and notes. The boot
ROM is not there and is gitignored - it is not ours to redistribute - and
`kernel.img` you build.

## Notes and limitations

- Supported Pi models: 3B/3B+ recommended (RASPPI=3 build). The 2MHz 65C02
  plus two VIAs is more work per microsecond than a 1MHz 1541; Pi Zero
  builds compile but are not expected to keep up.
- DHD images are streamed from the SD card through a RAM cache
  (`CMDHDCacheMB`). A cache miss stalls the emulated CPU for the duration
  of the SD access, exactly as if the SCSI drive were slow to respond.
- That cache is **write behind**. A write is acknowledged to the computer as
  soon as it reaches RAM and only reaches the card when the serial bus goes
  quiet, when the drive is reset, or on eject - going to the card mid-command
  freezes the emulated CPU far longer than the bus will wait. Cutting the
  power, or pulling the card, with writes still in flight loses them. If a
  flush does fail, the data stays cached for a retry and the drive reports
  CHECK CONDITION / MEDIUM ERROR on its next command rather than pretending
  the write landed.
- Fast serial (C128 burst) is wired through U10's shift register at the bit
  level and SRQ is sampled/driven as the 1581 build did. C64 use (including
  JiffyDOS, which HDOS implements in software) is unaffected.
- The RTC has no battery: it powers up at 2026-01-01 00:00 and can be set
  with the `T-W` commands (or by GEOS) for the session.
- The CMD parallel bus (RAMLink) is not brought out on Pi1541 hardware; the
  lines are emulated as pulled up.
- Pi1541's browse mode - the SD2IEC style handler that answered the bus so
  you could pick an image from the C64 - has been removed, along with its
  D64/G64 floppy emulation. Images are chosen with the buttons and the display,
  or with `AutoMountImage`.

## Source layout

```
src/
├── main.cpp, options.*     top level and configuration
├── rpi/                    bare metal Raspberry Pi: startup, GPIO, mailbox,
│                           I2C, interrupts, timers, EMMC, USB glue
├── fatfs/                  ChaN's FatFS, third party and unmodified
├── emulation/              the CMD HD: R65C02, two 6522 VIAs, the 8255A PPI,
│                           the RTC, SCSI, the machine, and the IEC bus
└── ui/                     screen, I2C LCD, fonts, file browser, input
target/                     all build output; nothing is written into src/
```

Sources include each other by bare name (`#include "iec_bus.h"`), with the
layout expressed as `-I` paths in the Makefile rather than in several hundred
include lines.

## Building

**arm-none-eabi-gcc 10.2 or newer**, plus **make**.

On Windows, the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
(the `mingw-w64-i686` host build for `arm-none-eabi`), and make from
[MSYS2](https://www.msys2.org/): `pacman -S make`. On dpkg based Linux systems:

```
apt-get install binutils-arm-none-eabi gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

Unpack the toolchain somewhere with a short path. A deeply nested location
caused the compiler to fail to find its own `sys/errno.h` here.

```
make RASPPI=3          # produces target/kernel.img
```

`RASPPI=2` and `RASPPI=0` (Pi Zero) also build, though only the Pi 3 is tested.
`make clean` removes `target/` outright, so a clean build costs nothing but
time.

On Windows, `build.bat` finds the toolchain and the right make for you, and
avoids two traps worth knowing about:

```
build.bat            clean build for a Pi 3
build.bat 2          clean build for a Pi 2
build.bat 3 quick    incremental
```

1. **`make` must be MSYS2's**, not GnuWin32's. GnuWin32 make gets part way and
   then fails on the recursive sub-make for `uspi`, because its own install
   path contains spaces and brackets (`C:\Program Files (x86)\...`) which the
   shell it invokes cannot parse. The error is a shell syntax error near `(`
   and looks nothing like a build problem. If both are installed, GnuWin32 is
   usually first on `PATH`, so this is the default outcome rather than an edge
   case.

2. **The Makefile does not track header dependencies**, and a good deal of this
   emulator lives in headers - `iec_bus.h` is inlined into `main.o`, for
   instance. An incremental build after editing a header yields a kernel
   containing only some of your changes, which is unpleasant to debug on real
   hardware. `build.bat` cleans first unless you say `quick`; with plain `make`,
   run `make clean` before any build you intend to flash.

## Licence & credits

GPL v3, like Pi1541 - see [LICENSE](LICENSE), and
[3rdPartyFiles.txt](3rdPartyFiles.txt) for the third party code this builds on.

- Pi1541 (c) 2018 Stephen White
- CMD HD hardware model, SCSI and i8255a emulation ported from VICE,
  written by Roberto Muscedere
- RTC-72421 register model after VICE's rtc-72421.c by Marco van den Heuvel

The CMD HD boot ROM is **not** included and is not covered by that licence.
Supply your own dump - see "What you need on the SD card" above.
