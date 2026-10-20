/*
 * sntpdate - simple ntpdate client
 * Copyright (C) 2004-2023 Sean MacLennan <seanm@seanm.ca>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this project; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <err.h>

/* RFC 1305 - NTP */
/* RFC 1361 - SNTP */
/* RFC 1769 - SNTP 3 */
/* RFC 2030 - SNTP 4 - supports ipv6 */


typedef struct timestamp {
	unsigned ts_secs;
	unsigned ts_frac;
} timestamp_t;


struct ntp_pkt {
	unsigned char flags;
	unsigned char stratum;
	signed char poll;
	signed char precision;

	int root_delay;
	unsigned root_dispersion;
	unsigned refid;

	timestamp_t ref_timestamp;
	timestamp_t orig_timestamp;
	timestamp_t recv_timestamp;
	timestamp_t xmit_timestamp;

	/* optional auth 96 bytes */
};

#define NTP_BASIC_SIZE sizeof(struct ntp_pkt)
#define NTP_FULL_SIZE  (NTP_BASIC_SIZE + 96)

#define NTP_PORT 123

#define OFFSET_1970 2208988800UL

#define RECV_TIMEOUT 500 // ms

_Static_assert(sizeof(timestamp_t) == sizeof(uint64_t), "timestamp");

/* buf must be NTP_FULL_SIZE */
static int send_receive(int sock, unsigned ipaddr, struct ntp_pkt *pkt)
{
	/* send request */
	memset(pkt, 0, NTP_BASIC_SIZE);
	pkt->flags = 0x1b;  /* li = 0 vn = 3 mode = 3 */

	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = ipaddr,
		.sin_port = htons(NTP_PORT),
	};

	int n = sendto(sock, pkt, NTP_BASIC_SIZE, 0, (struct sockaddr*)&sa, sizeof(sa));
	if(n != NTP_BASIC_SIZE) {
        if (n == -1) {
                if (errno == ENETUNREACH || EHOSTUNREACH)
                    usleep(500000);
                else
                    warn("Error sending");
        } else
			warn("Unable to send (%d/%ld)", n, NTP_BASIC_SIZE);
		return 1;
	}

	/* get reply */
	struct pollfd ufd = { .fd = sock, .events = POLLIN };
	do {
		n = poll(&ufd, 1, RECV_TIMEOUT);
		if(n < 0)
			warn("poll");
	} while(n < 0);
	if (n == 0)
		return 1; /* timeout */

	do {
		socklen_t fromlen = sizeof(sa);
		n = recvfrom(sock, pkt, NTP_FULL_SIZE, 0, (struct sockaddr*)&sa, &fromlen);
	} while (n < 0 && errno == EINTR);
	if(n < NTP_BASIC_SIZE) {
		warnx("Reply packet too small. (%d/%ld)", n, NTP_BASIC_SIZE);
		return 1;
	}

	/* Recommended checks. */
	if((pkt->flags & 0xc0) == 0xc0 ||
	   pkt->stratum < 1 || pkt->stratum > 15 ||
	   pkt->xmit_timestamp.ts_secs == 0) {
		printf("Flags %x %d %u\n", pkt->flags, pkt->stratum, pkt->xmit_timestamp.ts_secs);
		errx(1, "Server out of sync!");
	}

	return 0;
}

static int i_am_a_server(void)
{
	int sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == -1)
		err(1, "socket");

	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons(NTP_PORT),
	};

	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)))
		err(1, "bind");

	struct pollfd ufd = { .fd = sock, .events = POLLIN };
	while (1) {
		int n = poll(&ufd, 1, RECV_TIMEOUT);
		if(n < 0) {
			warn("poll");
			continue;
		}

		// Read the packet
		struct ntp_pkt pkt;
		do {
			socklen_t fromlen = sizeof(sa);
			n = recvfrom(sock, &pkt, NTP_BASIC_SIZE, 0, (struct sockaddr*)&sa, &fromlen);
		} while (n < 0 && errno == EINTR);

		// Assume it is good... we don't really case
		struct timeval now;
		gettimeofday(&now, NULL);
		pkt.flags = 0;    // reserved
		pkt.stratum = 15; // hint we are off

		// Frac off - give the real time
		pkt.xmit_timestamp.ts_secs = ntohl(now.tv_sec);
		pkt.xmit_timestamp.ts_frac = ntohl(now.tv_usec);

		n = sendto(sock, &pkt, NTP_BASIC_SIZE, 0, (struct sockaddr*)&sa, sizeof(sa));
		if (n != NTP_BASIC_SIZE)
			warn("Bad sendto %d/%ld\n", n, NTP_BASIC_SIZE);
	}
}

int main(int argc, char *argv[])
{
	int server = argc > 1 && strcmp(argv[1], "-s") == 0;
	if (server) { --argc; ++argv; }

	if(argc == 1)
		errx(1, "You must specify a host.");

	struct hostent *host = gethostbyname(argv[1]);
	if (!host)
		errx(1, "Unable to get ip for %s", argv[1]);

	unsigned ipaddr = *(unsigned *)host->h_addr_list[0];

	int sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == -1)
		err(1, "socket");

	char buf[NTP_FULL_SIZE];
	struct ntp_pkt *reply = (struct ntp_pkt *)buf;
	while(send_receive(sock, ipaddr, reply)) ;

	struct timeval t3;
	if (reply->flags == 0 && reply->stratum == 15) {
		// Our server
		t3.tv_sec = ntohl(reply->xmit_timestamp.ts_secs);
		t3.tv_usec = ntohl(reply->xmit_timestamp.ts_frac);
	} else {
		/* This conversion is accurate to +/-8 usecs.
		 * The correct conversion using floats:
		 *       usec = frac * (10 ^ 6) / (2 ^ 32)
		 */
		t3.tv_sec = ntohl(reply->xmit_timestamp.ts_secs) - OFFSET_1970;
		t3.tv_usec = ntohl(reply->xmit_timestamp.ts_frac) / 4295;
	}

	if(settimeofday(&t3, NULL))
		err(1, "settimeofday");

	if (server)
		i_am_a_server();

	return 0;
}
