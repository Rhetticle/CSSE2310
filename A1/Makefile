CC=gcc 
CFLAGS=-Wall -Wextra -pedantic -std=gnu99

.DEFAULT_GOAL := uqentropy

uqentropy: uqentropy.c
	$(CC) $(CFLAGS) $^ -o uqentropy -lm
	
clean: 
	rm uqentropy
