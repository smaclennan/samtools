CFLAGS += -Wall -O2

SYS = $(shell uname -s)
ifeq ($(SYS), QNX)
LIBS += -lsocket
endif

all: ipaddr myps sntpdate

ipaddr: ipaddr.c
	$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

sntpdate: sntpdate.c
	$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

clean:
	rm -f ipaddr myps sntpdate
