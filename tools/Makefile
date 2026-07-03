CC=     cc
CFLAGS= -O2 -Wall -Wextra -std=c99 -g -fPIE
PROGS=  procfs_ksyms procfsd

all: $(PROGS)

# procfs_ksyms decompresses the booted kernelcache and stages private kernel
# symbols to /var/db/procfs.ksyms. It is run at install/boot time (by procfsd),
# never at build time.
procfs_ksyms: procfs_ksyms.c
	$(CC) $(CFLAGS) -o $@ -lcompression $<

procfsd: procfsd.c
	$(CC) $(CFLAGS) -o $@ $< -framework IOKit -framework CoreFoundation

clean:
	rm -f $(PROGS)
	rm -rf *.dSYM

.PHONY: all clean
