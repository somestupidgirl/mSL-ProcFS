SDKROOT?=       $(shell xcrun --sdk macosx --show-sdk-path)
KERNEL_FW=      $(SDKROOT)/System/Library/Frameworks/Kernel.framework
KERNEL_HEADERS= $(KERNEL_FW)/Headers

CC =       $(shell xcrun -find -sdk $(SDKROOT) cc)
AR ?=   ar
RM ?=   rm -f

#
# Target arm64e — the Apple Silicon kernel ABI.
#
ARCHFLAGS?=     -arch arm64e
TARGET_TRIPLE?= arm64e-apple-macos12.0

CFLAGS?=    -target $(TARGET_TRIPLE)            \
            -std=c99                            \
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
            -Wall -Wextra -Wno-unused-function  \
            -I../include/xnu/bsd                \
            -I../include/xnu/libkern            \
            -I../include/xnu/osfmk              \
            -I../lib                            \
            -isystem $(KERNEL_HEADERS)          \
            -isystem $(KERNEL_HEADERS)/arm64    \
            -isystem $(KERNEL_HEADERS)/arm      \
            -DKEXTNAME_S=\"procfs\"

EXT =       a
LIB =       libkext.$(EXT)
SRCS =      $(wildcard *.c)
LIB_OBJS =  $(patsubst %.c,%.o,$(sort $(SRCS)))
INC =       -I.

all: $(LIB)

clean:
	@$(RM) $(LIB_OBJS) $(LIB)
	@echo "$(LIB_OBJS)" | xargs -n1 | sed 's|^|clean |'

$(LIB): $(LIB_OBJS)
	@$(AR) rcs $@ $^
	@echo "$^" | xargs -n1 | sed 's|^|AR |'
	@echo "AR $@"

%.o: %.c
	@$(CC) -o $@ $(CFLAGS) -c $< $(INC)
	@echo "$@" | xargs -n1 | sed 's|^|CC |'
