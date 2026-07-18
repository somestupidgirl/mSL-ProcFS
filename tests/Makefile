CC=     cc
CFLAGS= -Wall -Wextra -std=c99 -fPIE -g

#
# Every program here exercises a node that currently exists. They are plain
# single-file programs, so one pattern rule covers them all.
#
#   test_mount          mount(2) of the filesystem
#   test_readdir        readdir() over the root
#   test_getdents       getdirentries64() over the root
#   test_getdents_multi multi-call readdir resumption with small buffers,
#                       guarding the regression fixed by the readdir EIO work
#   test_misc           /proc/misc device listing
#   test_taskinfo       /proc/<pid>/taskinfo against proc_pidinfo ground truth
#
PROGS=  test_mount \
        test_readdir \
        test_getdents \
        test_getdents_multi \
        test_misc \
        test_taskinfo

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
	rm -rf *.dSYM

.PHONY: all clean
