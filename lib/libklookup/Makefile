SDKROOT?=       $(shell xcrun --sdk macosx --show-sdk-path)
KERNEL_FW=      $(SDKROOT)/System/Library/Frameworks/Kernel.framework
KERNEL_HEADERS= $(KERNEL_FW)/Headers

#
# mach-o/nlist.h lives in usr/include, not Kernel.framework.
# We need both paths.
#
USR_HEADERS=    $(SDKROOT)/usr/include

CC ?=   clang
AR ?=   ar
RM ?=   rm -f

ARCHFLAGS?=     -arch arm64e
TARGET_TRIPLE?= arm64e-apple-macos12.0

CFLAGS?=    -target $(TARGET_TRIPLE)            \
            -std=gnu99                          \
            -ffunction-sections                 \
            -fdata-sections                     \
            -Bstatic                            \
            -x c                                \
            -nostdinc                           \
            -fno-builtin                        \
            -fno-common                         \
            -mkernel                            \
            -fno-stack-check                    \
            -fno-stack-protector                \
            $(ARCHFLAGS)                        \
            -Wall -Wextra -Wno-unused-function

ifeq ($(findstring arm64,$(TARGET_TRIPLE)),arm64)
CFLAGS+=    -I../libklookup                     \
            -I../../xnu/bsd                     \
            -I../../xnu/libkern                 \
            -I../../xnu/osfmk                   \
            -I../../xnu/pexpert                 \
            -isystem $(KERNEL_HEADERS)/arm64    \
            -isystem $(KERNEL_HEADERS)/arm
else
CFLAGS+=    -I../../xnu/bsd                     \
            -I../../xnu/libkern                 \
            -I../../xnu/osfmk                   \
            -I../../xnu/pexpert
endif

CFLAGS+=    -I../../lib                         \
            -isystem $(KERNEL_HEADERS)          \
            -isystem $(USR_HEADERS)             \
            -isystem $(USR_HEADERS)

# Compile the vendored XNU headers in their kernel-private configuration,
# matching the kext build (see kext/Makefile).
CFLAGS+=    -DKERNEL -DPRIVATE -DKERNEL_PRIVATE -DXNU_KERNEL_PRIVATE \
            -DBSD_KERNEL_PRIVATE -DMACH_KERNEL_PRIVATE -D__APPLE_API_PRIVATE \
            -D_DARWIN_C_SOURCE -DMACH_BSD -DXNU_TARGET_OS_OSX -DXNU_PLATFORM_MacOSX \
            -lklookup

EXT =       a
LIB =       libklookup.$(EXT)
SRCS =      $(wildcard *.c)
LIB_OBJS =  $(patsubst %.c,%.o,$(sort $(SRCS)))
INC =       -I.

all: $(LIB)

clean:
	@$(RM) $(LIB_OBJS) $(LIB)
	@echo "$(LIB_OBJS)" | xargs -n1 | ts "clean"

$(LIB): $(LIB_OBJS)
	@$(AR) rcs $@ $^
	@echo "$^" | xargs -n1 | ts "AR"
	@echo "AR $@"

%.o: %.c
	@$(CC) -o $@ $(CFLAGS) -c $< $(INC)
	@echo "$@" | xargs -n1 | ts "CC"
