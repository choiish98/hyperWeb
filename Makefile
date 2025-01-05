.PHONY: clean

CFLAGS := -Wall -g -O3 -Werror
MAKE := make
CC := gcc
MSG := @echo
HIDE := @

default: server 

server.o: server.c
	$(MSG) "	CC $<"
	$(HIDE) $(CC) -c $< $(CFLAGS) 

server: server.o
	$(MSG) "	LD $<"
	$(HIDE) $(CC) $< -o $@

clean:
	$(MSG) "	CLEAN server"
	$(HIDE) rm -f server *.o
