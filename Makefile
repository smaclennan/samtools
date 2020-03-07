CFLAGS += -Wall -O2

SYS = $(shell uname -s)
ifeq ($(SYS), QNX)
LIBS += -lsocket
endif

all: ipaddr myps

ipaddr: ipaddr.c
	$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

clean:
	rm -f ipaddr myps
