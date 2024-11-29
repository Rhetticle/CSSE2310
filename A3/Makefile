CC=gcc
CFLAGS=-Wall -Wextra -pedantic -std=gnu99
#INCDIR=/local/courses/csse2310/include
#LIBDIR=/local/courses/csse2310/lib
INCDIR=../include
LIBDIR=../lib

LDFLAGS=-L$(LIBDIR) -lcsse2310a3

DEFAULT_GOAL=uqzip

uqzip: uqzip.c
	$(CC) $^ $(CFLAGS) -I$(INCDIR) $(LDFLAGS) -o uqzip

clean:
	rm -f uqzip
