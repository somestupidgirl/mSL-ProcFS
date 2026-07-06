CC=     cc
CFLAGS= -O2 -Wall -Wextra -std=c99 -g -fPIE
PROGS=  procfsd

all: $(PROGS)

procfsd: procfsd.c
	$(CC) $(CFLAGS) -o $@ $< -framework IOKit -framework CoreFoundation

clean:
	rm -f $(PROGS)
	rm -rf *.dSYM

.PHONY: all clean
