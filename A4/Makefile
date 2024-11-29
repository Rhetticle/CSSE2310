CC=gcc
CFLAGS=-Wextra -Wall -pedantic -std=gnu99

LIBDIR=../lib
INCDIR=../include

DEFAULT_GOAL=all

all: uqchessclient uqchessserver

uqchessclient: uqchessclient.c
	$(CC) $^ $(CFLAGS) -I$(INCDIR) -L$(LIBDIR) -lcsse2310a4 -pthread -g -o uqchessclient

uqchessserver: uqchessserver.c 
	$(CC) $^ $(CFLAGS) -I$(INCDIR) -L$(LIBDIR) -lcsse2310a4 -pthread -o uqchessserver

clean:
	rm -f uqchessclient uqchessserver
