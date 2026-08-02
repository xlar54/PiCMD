# SD card files for a Raspberry Pi 3B / 3B+

The Raspberry Pi boot files needed for a bootable Pi-CMD card, kept here so
they cannot get lost. Copy the contents of this folder to the **root** of the
card, add `kernel.img` (built from source into `target/`, see the top level
README) and a CMD HD boot ROM, and put your DHD images in a `cmd-images`
folder.

The boot ROM is **not in this repository** - it is not ours to redistribute, so
it is gitignored. Supply your own dump, name it `cmdhd-bootrom.bin`, and drop it
in this folder alongside these files; it will then be copied to the card with
the rest of them.

```
SD:/
├── bootcode.bin
├── start.elf
├── fixup.dat
├── config.txt
├── kernel.img            <- built by: make RASPPI=3  (from target/kernel.img)
├── cmdhd-bootrom.bin     <- supply your own; not in this repository
├── options.txt
└── cmd-images/
    └── hd0.dhd
```

## One FAT32 partition, spanning the whole card

There is no second partition and nothing resembling the Raspbian layout (a
small FAT boot partition plus a large ext4 one) - delete both and make a single
FAT32 partition.

Do **not** format the card exFAT. FatFS is built with exFAT support so the
emulator could read such a volume, but the Pi's boot ROM only understands
FAT12/16/32, so the card would never boot.

Windows only formats up to 32GB as FAT32 from Explorer; for a larger card use
Rufus, fat32format/guiformat, or format it on Linux or a Mac.

Because FAT32 cannot hold a file of 4GB or more, keep each image below that.
For more storage, add further SCSI units as `.sXY` files alongside the main
image (`hd0.s10`, `hd0.s20`, ...) - they are attached automatically.

## The files

| File | Notes |
|---|---|
| `bootcode.bin` | Raspberry Pi firmware, version 1.20200902 |
| `start.elf` | Raspberry Pi firmware |
| `fixup.dat` | Raspberry Pi firmware. Must match `start.elf`. |
| `LICENCE.broadcom` | Licence covering the three files above |
| `config.txt` | **Required, and easy to overlook.** `kernel_address=0x1f00000` tells the firmware where to load the kernel, because that is where it links. Leave this out and the Pi stops at the rainbow test screen with no other symptom. `force_turbo=1` keeps the ARM clock from scaling, which matters for cycle exact timing. |
| `cmdhd-bootrom.bin` | **Not in this repository - supply your own.** CMD HD boot ROM, v2.80 recommended. A 32K dump containing the 16K image twice works, as does a plain 16K image. |
| `options.txt` | The one canonical configuration file - working values for hackup's Pi1541io rev 4 with an I2C OLED, and every option the emulator understands, commented with its default. |

Nothing else is needed: no `.dtb`, no `cmdline.txt`, no second partition.

## Firmware version

These are the 1.20200902 files rather than anything newer, because that era is
what Pi1541 and its derivatives are known to work with. Much newer firmware has
not been tested here and is not worth the risk for the sake of being current -
this is a bare metal kernel, so it gains nothing from firmware updates.

## Licences

`bootcode.bin`, `start.elf` and `fixup.dat` are Broadcom/Raspberry Pi firmware,
redistributed under the terms in `LICENCE.broadcom`.

The CMD HD boot ROM is deliberately **not** kept here. CMD (Creative Micro
Designs) has been defunct for many years and dumps circulate freely for use
with emulators, but it is not ours to redistribute and is not covered by this
project's GPL, so `cmdhd-bootrom.bin` is gitignored. The emulator is useless
without one, so you will need to find a dump before any of this works.
