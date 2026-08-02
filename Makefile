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
