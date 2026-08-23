# Host build of the emulator's pure logic, with the hardware stubbed out.
#
# The emulator sources are compiled UNMODIFIED. The shims work by sitting
# earlier on the include path than the real ff.h and rpihardware.h, so no
# production file needs a test-only #ifdef in it. Keep it that way: the moment
# a header here has to change for the tests, the seam has moved.
#
#   make            build and run
#   make build      build only
#   make clean

CXX      ?= g++

SRCDIR    = ../src
SHIMS     = shims

# shims first, so ff.h and rpihardware.h resolve here rather than in src.
INCLUDE   = -I$(SHIMS) -I. -I$(SRCDIR) -I$(SRCDIR)/emulation -I$(SRCDIR)/fatfs -I../uspi/include

CXXFLAGS  = -g -O1 -Wall -Wextra -Wno-unused-parameter -std=c++14 $(INCLUDE)

# Address and UB sanitizers catch the class of thing that keeps turning up in
# this codebase - the one byte past data_buf, the sixteenth byte in a fifteen
# byte buffer - so run with them wherever they exist.
#
# Off by default because MinGW on Windows ships no libasan or libubsan and the
# link simply fails. On Linux and macOS, and in CI, use:
#
#   make SANITIZE=1
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

# Code under test - real emulator sources, no copies.
UNDER_TEST = $(SRCDIR)/emulation/scsi.cpp

TEST_SRCS  = main.cpp harness.cpp \
             test_scsi_cache.cpp \
             $(SHIMS)/ff.cpp $(SHIMS)/fakeclock.cpp $(SHIMS)/debug.cpp

TARGET     = run_tests

.PHONY: all build run clean

all: run

build: $(TARGET)

$(TARGET): $(TEST_SRCS) $(UNDER_TEST)
	@echo "  HOSTCXX $(TARGET)"
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(TEST_SRCS) $(UNDER_TEST)

run: $(TARGET)
	@./$(TARGET)

clean:
	$(RM) $(TARGET) $(TARGET).exe
