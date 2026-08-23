SRCDIR    ?= src
TARGETDIR ?= target

# Everything the compiler may be asked to include by bare name. The sources use
# plain #include "foo.h" throughout, so the layout is expressed here rather than
# by rewriting several hundred include lines.
SRCSUBDIRS = . rpi fatfs emulation ui

# armc-start.o must stay first: it holds the entry point, and the linker script
# places the first object at the load address.
OBJS = \
	rpi/armc-start.o rpi/armc-cstartup.o rpi/armc-cstubs.o rpi/armc-cppstubs.o \
	rpi/exception.o rpi/interrupt.o rpi/rpi-interrupts.o rpi/cache.o \
	rpi/rpi-aux.o rpi/rpi-i2c.o rpi/rpi-gpio.o rpi/rpi-mailbox.o \
	rpi/rpi-mailbox-interface.o rpi/performance.o rpi/spinlock.o rpi/timer.o \
	rpi/emmc.o rpi/diskio.o \
	fatfs/ff.o \
	emulation/picmdhd.o emulation/m65c02.o emulation/m6522.o emulation/scsi.o \
	emulation/i8255a.o emulation/rtc72421.o emulation/iec_bus.o \
	ui/screen.o ui/ssd1306.o ui/screenlcd.o ui/xga_font_data.o \
	ui/filebrowser.o ui/inputmappings.o ui/keyboard.o \
	main.o options.o

OBJS := $(addprefix $(TARGETDIR)/, $(OBJS))

LIBS     = uspi/libuspi.a
INCLUDE  = -Iuspi/include/ $(addprefix -I$(SRCDIR)/, $(SRCSUBDIRS))

TARGET ?= kernel
KERNEL  = $(TARGETDIR)/$(TARGET)

.PHONY: all clean $(LIBS)

all: $(KERNEL).img

# Objects are built for one Pi and are not interchangeable - RASPPI selects
# different -mcpu flags and different conditional code - but nothing in their
# names or timestamps says which. Building for one target and then another
# without cleaning silently reused the first target's objects; if you were
# lucky it failed at link with missing symbols, and if you were not it linked
# and produced a kernel for neither.
#
# So: a stamp named after the target. Asking for a different one finds no stamp
# for it, and the tree is cleaned before anything is compiled.
RASPPI_STAMP = $(TARGETDIR)/.built-for-$(RASPPI)

$(RASPPI_STAMP):
	@echo "  TARGET RASPPI=$(RASPPI) (cleaning: previous build was for another target)"
	$(Q)$(RM) -r $(TARGETDIR)
	$(Q)$(MAKE) -C uspi clean
	@mkdir -p $(TARGETDIR)
	@touch $@

$(OBJS): $(RASPPI_STAMP)

$(KERNEL).img: $(OBJS) $(LIBS)
	@echo "  LINK $@"
	$(Q)$(CC) $(CFLAGS) -o $(KERNEL).elf -Xlinker -Map=$(KERNEL).map -T linker.ld -nostartfiles $(OBJS) $(LIBS)
	$(Q)$(PREFIX)objdump -d $(KERNEL).elf | $(PREFIX)c++filt > $(KERNEL).lst
	$(Q)$(PREFIX)objcopy $(KERNEL).elf -O binary $(KERNEL).img

# uspi is a vendored third party library with its own build, and keeps its
# objects in uspi/lib next to its sources.
uspi/libuspi.a:
	$(MAKE) -C uspi

clean:
	$(Q)$(RM) -r $(TARGETDIR)
	$(MAKE) -C uspi clean

include Makefile.rules

# Pull in the header dependencies the compiler recorded next to each object.
# Leading dash so the first build, when none of them exist yet, is not an
# error. These live under $(TARGETDIR) and go with it on a clean.
-include $(OBJS:.o=.d)
