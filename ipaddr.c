/* ipaddr.c - simple script friendly ifconfig/ip replacement
 * Copyright (C) 2004-2023 Sean MacLennan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this project; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <err.h>
#include <sys/ioctl.h>
#include <net/if.h> // Must be before ifaddrs.h
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <net/route.h>

#define W_ADDRESS  (1 <<  0)
#define W_MASK     (1 <<  1)
#define W_SUBNET   (1 <<  2)
#define W_BITS     (1 <<  3)
#define W_GATEWAY  (1 <<  4)
#define W_GUESSED  (1 <<  5)
#define W_ALL	   (1 <<  6)
#define W_FLAGS    (1 <<  7)
#define W_SET      (1 <<  8)
#define W_QUIET    (1 <<  9)
#define W_MAC      (1 << 10)
#define W_DOWN     (1 << 11)
#define W_EXISTS   (1 << 12)
#define W_TUNTAP   (1 << 13)
#define W_TOP_BYTE (1 << 14)
#define W_NO_VIRT  (1 << 15)

#define VIRBR "virbr"

#ifdef __QNX__
#include <sys/nto_version.h>
#if _NTO_VERSION < 720
// SIOCAIFADDR does not seem to work for io_pkt
#undef SIOCAIFADDR
#endif
#endif

#if defined(__linux__)
/* Currently only used to copy ifnames */
static void strlcpy(char *dst, const char *src, int dstlen)
{
	--dstlen; // leave room for null
	for (int i = 0; *src && i < dstlen; ++i)
		*dst++ = *src++;
	*dst = 0;
}

/* Returns 0 on success, < 0 for errors, and > 0 if ifname not found.
 * The gateway arg can be NULL.
 */
static int get_gateway(const char *ifname, struct in_addr *gateway)
{
	FILE *fp = fopen("/proc/net/route", "r");
	if (!fp)
		return -1;

	char line[128], iface[8];
	uint32_t dest, gw, flags;
	while (fgets(line, sizeof(line), fp))
		if (sscanf(line, "%s %x %x %x", iface, &dest, &gw, &flags) == 4 &&
			dest == 0 && (flags & 2))
			if (!ifname || strcmp(iface, ifname) == 0) {
				fclose(fp);
				gateway->s_addr = gw;
				return 0;
			}

	fclose(fp);
	return 1;
}

static int set_gateway(const char *gw)
{
	int fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (fd == -1)
		return -1;

	struct rtentry rtreq = {
		.rt_flags = (RTF_UP | RTF_GATEWAY),
		.rt_gateway.sa_family = AF_INET,
		.rt_genmask.sa_family = AF_INET,
		.rt_dst.sa_family = AF_INET,
	};

	struct sockaddr_in *sa = (struct sockaddr_in*)&rtreq.rt_gateway;
	sa->sin_addr.s_addr = inet_addr(gw);

	int rc = ioctl(fd, SIOCADDRT, &rtreq);
	close(fd);
	return rc;
}

static int get_hw_addr(const char *ifname, unsigned char *hwaddr)
{
	int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock == -1)
		return -1;

	struct ifreq ifreq = { 0 };
	strlcpy(ifreq.ifr_name, ifname, IF_NAMESIZE);
	int rc = ioctl(sock, SIOCGIFHWADDR, &ifreq);
	close(sock);

	memcpy(hwaddr, ifreq.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
	return rc;
}
#else
#include <net/if_dl.h>
#include <net/if_media.h>

#define RTM_ADDRS ((1 << RTAX_DST) | (1 << RTAX_GATEWAY) | (1 << RTAX_NETMASK))
#define RTM_SEQ 42
#define RTM_FLAGS (RTF_STATIC | RTF_UP | RTF_GATEWAY)
#define	READ_TIMEOUT 10

struct rtmsg {
	struct rt_msghdr hdr;
	struct sockaddr_in data[3];
};

static int rtmsg_send(int s, int cmd, const char *gw)
{
	struct rtmsg rtmsg = {
		.hdr.rtm_type = cmd,
		.hdr.rtm_flags = RTM_FLAGS,
		.hdr.rtm_version = RTM_VERSION,
		.hdr.rtm_seq = RTM_SEQ,
		.hdr.rtm_addrs = RTM_ADDRS,
		.hdr.rtm_msglen = sizeof(rtmsg),
		.data = {
			{ .sin_len = sizeof(struct sockaddr_in), .sin_family = AF_INET },
			{ .sin_len = sizeof(struct sockaddr_in), .sin_family = AF_INET },
			{ .sin_len = sizeof(struct sockaddr_in), .sin_family = AF_INET }
		}
	};

	if (gw)
		rtmsg.data[RTAX_GATEWAY].sin_addr.s_addr = inet_addr(gw);

	if (write(s, &rtmsg, sizeof(rtmsg)) != sizeof(rtmsg))
		return -1;

	return 0;
}

static int rtmsg_recv(int s, struct in_addr *gateway)
{
	struct rtmsg rtmsg;

	do {
		if (read(s, (char *)&rtmsg, sizeof(rtmsg)) <= 0)
			return -1;
	} while (rtmsg.hdr.rtm_type != RTM_GET ||
			 rtmsg.hdr.rtm_seq != RTM_SEQ ||
			 rtmsg.hdr.rtm_pid != getpid());

	if (rtmsg.hdr.rtm_version != RTM_VERSION)
		return -1;
	if (rtmsg.hdr.rtm_errno)  {
		errno = rtmsg.hdr.rtm_errno;
		return -1;
	}

	if ((rtmsg.hdr.rtm_addrs & (1 << RTAX_GATEWAY)) == 0) {
		errno = ENOENT;
		return 1; /* not found */
	}

	*gateway = rtmsg.data[RTAX_GATEWAY].sin_addr;
	return 0;
}

/* ifname ignored */
static int get_gateway(const char *ifname, struct in_addr *gateway)
{
	int s = socket(PF_ROUTE, SOCK_RAW, 0);
	if (s < 0)
		return -1;

	struct timeval tv = { .tv_sec = READ_TIMEOUT };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

	if (rtmsg_send(s, RTM_GET, NULL)) {
		close(s);
		return -1;
	}

	int n = rtmsg_recv(s, gateway);
	close(s);
	return n;
}

static int set_gateway(const char *gw)
{
	int s = socket(PF_ROUTE, SOCK_RAW, 0);
	if (s < 0)
		return -1;

	shutdown(s, SHUT_RD); /* Don't want to read back our messages */

	if (rtmsg_send(s, RTM_ADD, gw) == 0) {
		close(s);
		return 0;
	}

	if (errno == EEXIST)
		if (rtmsg_send(s, RTM_CHANGE, gw) == 0) {
			close(s);
			return 0;
		}

	close(s);
	return -1;
}

static int get_hw_addr(const char *ifname, unsigned char *hwaddr)
{
	struct ifaddrs *ifa = NULL;
	struct sockaddr_dl *sa = NULL;

	if (getifaddrs(&ifa)) {
		perror("getifaddrs");
		return -1;
	}

	for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
		if (p->ifa_addr->sa_family == AF_LINK &&
			strcmp(p->ifa_name, ifname) == 0) {
			sa = (struct sockaddr_dl *)p->ifa_addr;
			if (sa->sdl_type == 1 || sa->sdl_type == 6) { // ethernet
				memcpy(hwaddr, LLADDR(sa), sa->sdl_alen);
				freeifaddrs(ifa);
				return 0;
			} else
				return -1;
		}
	}

	errno = ENOENT;
	return -1;
}
#endif

#ifdef SIOCSIFLLADDR
// Some BSD, QNX
#define HWADDR_IOCTL SIOCSIFLLADDR
#define HWADDR_FAMILY AF_LINK
#elif defined(SIOCSIFHWADDR)
// Linux
#define HWADDR_IOCTL SIOCSIFHWADDR
#define HWADDR_FAMILY ARPHRD_ETHER
#endif

#ifdef HWADDR_IOCTL
// Errors are fatal
static void mac_to_binary(const char *text, uint8_t *macaddr)
{
	int mac_i = 0;
	int state = 0;

	for (const char *mac = text; *mac; ++mac) {
		uint8_t nibble;
		if (*mac >= '0' && *mac <= '9')
			nibble = *mac - '0';
		else if (*mac >= 'a' && *mac <= 'f')
			nibble = *mac - 'a' + 10;
		else if (*mac >= 'A' && *mac <= 'F')
			nibble = *mac - 'A' + 10;
		else if (*mac == ':') // colons are optional
			continue;
		else
			errx(1, "Invalid char %c", *mac);

		macaddr[mac_i] = (macaddr[mac_i] << 4) | nibble;
		if (state == 1)
			++mac_i;
		state ^= 1;
	}

	if (mac_i != 6)
		errx(1, "Invalid mac length");
}

// Errors are fatal
static void set_hw_addr(const char *name, const char *mac)
{
	uint8_t macaddr[ETHER_ADDR_LEN] = { 0 };
	mac_to_binary(mac, macaddr);

	struct ifreq ifr = {
		.ifr_addr.sa_family = HWADDR_FAMILY,
#ifndef __linux__
		.ifr_addr.sa_len = ETHER_ADDR_LEN,
#endif
	};

	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	memcpy(ifr.ifr_addr.sa_data, macaddr, 6);

	int s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0)
		err(1, "socket");

	if (ioctl(s, HWADDR_IOCTL, &ifr))
		err(1, "ioctl");

	close(s);
}
#else
static void set_hw_addr(const char *name, const char *mac)
{
	errx(1, "Not supported");
}
#endif

/* This is so fast, it is not worth optimizing. */
static int maskcnt(unsigned mask)
{
	unsigned count = 32;

	mask = ntohl(mask);
	while ((mask & 1) == 0) {
		mask >>= 1;
		--count;
	}

	return count;
}

static int ip_addr(const char *ifname, struct in_addr *addr, struct in_addr *mask)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;

	struct ifreq ifr = { 0 };
	strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);

	if (ioctl(s, SIOCGIFADDR, &ifr) < 0)
		goto failed;
	*addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;

	// We need this zero for QNX
	((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr = 0;
	if (ioctl(s, SIOCGIFNETMASK, &ifr) < 0)
		goto failed;
	*mask = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;

	close(s);

	return 0;

failed:
	close(s);
	return -1;
}

static int set_ip(const char *ifname, const char *ip, unsigned mask, int down)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == -1) {
		perror("set_ip socket");
		return -1;
	}

	struct ifreq req = {
#ifndef __linux__
		.ifr_addr.sa_len = sizeof(struct sockaddr_in),
#endif
		.ifr_addr.sa_family = AF_INET,
	};

	strlcpy(req.ifr_name, ifname, IF_NAMESIZE);

#if defined(SIOCAIFADDR)
	struct ifaliasreq areq = {
		.ifra_addr.sa_len = sizeof(struct sockaddr_in),
		.ifra_addr.sa_family = AF_INET,
		.ifra_mask.sa_len = sizeof(struct sockaddr_in),
		.ifra_mask.sa_family = AF_INET,
	};

	strlcpy(areq.ifra_name, ifname, IF_NAMESIZE);
	if (ip) {
		struct sockaddr_in *in = (struct sockaddr_in *)&areq.ifra_addr;
		in->sin_addr.s_addr = inet_addr(ip);
	}
	if (mask) {
		struct sockaddr_in *in = (struct sockaddr_in *)&areq.ifra_mask;
		in->sin_addr.s_addr = mask;
	}

	if (ioctl(s, SIOCAIFADDR, &areq)) {
		perror("SIOCAIFADDR");
		goto failed;
	}
#else
	struct sockaddr_in *in = (struct sockaddr_in *)&req.ifr_addr;

	if (ip)
		in->sin_addr.s_addr = inet_addr(ip);

	if (ioctl(s, SIOCSIFADDR, &req)) {
		perror("SIOCSIFADDR");
		goto failed;
	}

	if (mask) {
		in->sin_addr.s_addr = mask;
		if (ioctl(s, SIOCSIFNETMASK, &req)) {
			perror("SIOCSIFNETMASK");
			goto failed;
		}
	}
#endif

	req.ifr_flags = down ? 0 : IFF_UP;
	if (ioctl(s, SIOCSIFFLAGS, &req)) {
		perror("SIOCSIFFLAGS");
		goto failed;
	}

	close(s);
	return 0;

failed:
	close(s);
	return -1;
}

#ifdef __linux__
static int
link_status(int sock, const char *ifname, short flags)
{
	// If the interface is not up, we cannot get the link status
	if ((flags & IFF_UP) == 0)
		return -1;

	char buf[40];
	snprintf(buf, sizeof(buf), "/sys/class/net/%s/carrier", ifname);
	int fd = open(buf, O_RDONLY);
	if (fd == -1)
		return -1;

	int n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n < 1)
		return -1;

	return *buf == '1';
}
#else
// Both bits must be set
#define ACTIVE (IFM_AVALID | IFM_ACTIVE)

static int
link_status(int sock, const char *ifname, short flags)
{
	struct ifmediareq ifmr = { 0 };
	strlcpy(ifmr.ifm_name, ifname, sizeof(ifmr.ifm_name));
	if (ioctl(sock, SIOCGIFMEDIA, &ifmr))
		return -1;

	return (ifmr.ifm_status & ACTIVE) == ACTIVE;
}
#endif

static char *ip_flags(const char *ifname)
{
	static char flagstr[64];
	struct ifreq ifreq;

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return "failed";

	memset(&ifreq, 0, sizeof(ifreq));
	strlcpy(ifreq.ifr_name, ifname, IF_NAMESIZE);
	if (ioctl(sock, SIOCGIFFLAGS, &ifreq)) {
		close(sock);
		return "Failed";
	}

	int link_stat = link_status(sock, ifname, ifreq.ifr_flags);

	close(sock);

	sprintf(flagstr, "0x%04hx %s%s %s", ifreq.ifr_flags,
			(ifreq.ifr_flags & IFF_UP) ? "UP" : "DOWN",
			(ifreq.ifr_flags & IFF_RUNNING) ? ",RUNNING" : "",
			link_stat == -1 ? "unknown" :
			link_stat == 1 ? "active" : "no carrier");

	return flagstr;
}

static int check_one(const char *ifname, struct sockaddr *in, int state, unsigned what)
{
	int n = 0;
	struct in_addr addr = { 0 }, mask = { 0 }, gw;
	char mac_str[ETHER_ADDR_LEN * 3];

	if (what & W_MAC) {
		unsigned char mac[ETHER_ADDR_LEN];
		if (get_hw_addr(ifname, mac))
			return 1;
		for (int i = 0; i < ETHER_ADDR_LEN; ++i)
			sprintf(mac_str + (i * 3), "%02x:", mac[i]);
		mac_str[sizeof(mac_str) - 1] = 0;

		// We may want the mac before interface is up
		if (what == W_MAC) {
			puts(mac_str);
			return 0;
		}
	}

	int rc = 0;
	if (in)
		addr = ((struct sockaddr_in *)in)->sin_addr;
	else
		rc = ip_addr(ifname, &addr, &mask);

	if (rc == 0 && (what & W_GATEWAY))
		rc = get_gateway(ifname, &gw);

	if (what & W_QUIET)
		return !!rc;

	if (rc) {
		if (errno == EADDRNOTAVAIL) {
			if (what & W_ALL) {
				/* not an error, they asked for down interfaces */
				if (state)
					fputs("0.0.0.0", stdout);
				else
					fputs("down", stdout);
				if (what & W_MAC)
					printf(" %s", mac_str);
				printf(" (%s)\n", ifname);
				return 0;
			} else if ((what & W_GUESSED) == 0)
				fprintf(stderr, "%s: No address\n", ifname);
		} else
			perror(ifname);
		return 1;
	}

	if (what & W_ADDRESS) {
		++n;
		printf("%s", inet_ntoa(addr));
		if (what & W_BITS)
			printf("/%d", maskcnt(mask.s_addr));
	}
	if (what & W_SUBNET) {
		addr.s_addr &= mask.s_addr;
		if (n++)
			putchar(' ');
		printf("%s", inet_ntoa(addr));
		if (what & W_BITS)
			printf("/%d", maskcnt(mask.s_addr));
	}
	if (what & W_MASK) {
		if (n++)
			putchar(' ');
		printf("%s", inet_ntoa(mask));
	}
	if (what & W_MAC) {
		if (n++)
			putchar(' ');
		fputs(mac_str, stdout);
	}
	if (what & W_FLAGS) {
		if (n++)
			putchar(' ');
		printf("<%s>", ip_flags(ifname));
	}
	if (what & W_TOP_BYTE) {
		char ipstr[16];
		strcpy(ipstr, inet_ntoa(addr));
		char *p = strchr(ipstr, '.');
		if (p) *p = 0;
		printf("%s", ipstr);
		n += strlen(ipstr);
	}

	if (what & W_GATEWAY) {
		if (n++)
			putchar(' ');
		printf("%s", inet_ntoa(gw));
	}

	if (n) {
		if (what & W_GUESSED)
			printf(" (%s)", ifname);
		putchar('\n');
	}

	return 0;
}

#ifdef __linux__
// FreeBSD?
#include <linux/if_tun.h>

static int taptun(const char *dev)
{
	struct ifreq ifr = { 0 };
	strlcpy(ifr.ifr_name, dev, IFNAMSIZ);
	ifr.ifr_flags = strncmp(dev, "tap", 3) == 0 ? IFF_TAP : IFF_TUN;
	ifr.ifr_flags |= IFF_NO_PI;

	int fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		perror("/dev/net/tun");
		exit(1);
	}

	if (ioctl(fd, TUNSETIFF, (void *) &ifr)) {
		perror("TUNSETIFF");
		exit(1);
	}

	if (ioctl(fd, TUNSETPERSIST, 1)) {
		perror("TUNSETPERSIST");
		exit(1);
    }

	close(fd);

	// up tunnel
	return !!set_ip(dev, NULL, 0, 0);
}
#endif

static void usage(int rc)
{
	fputs("usage: ipaddr [-abefgimsqM] [interface]\n"
		  "       ipaddr <interface> <ip> <mask> [gateway]\n"
		  "       ipaddr <interface> <ip>/<bits> [gateway]\n"
		  "       ipaddr -D <interface>\n"
		  "       ipaddr -C <interface>\n"
		  "       ipaddr -M <interface> [mac]\n"
#ifdef __linux__
		  "       ipaddr -T <interface>\n"
#endif
		  "where: -e displays everything (-ibMf)\n"
		  "       -i displays IP address (default)\n"
		  "       -f displays up and running flags and link status\n"
		  "       -g displays gateway\n"
		  "       -m displays network mask\n"
		  "       -s displays subnet\n"
		  "       -t top byte of IP address\n"
		  "       -b add bits as /bits to -i and/or -s\n"
		  "       -a displays all interfaces (even down)\n"
		  "       -q quiet, return error code only\n"
		  "       -D down interface\n"
		  "       -C check interface exists\n"
		  "       -M display, or optionally set, hardware address (mac)\n"
#ifdef __linux__
		  "       -T create a TAP/TUN interface. Linux only.\n"
#endif
		  "       -V no virtual network\n"
		  "\nInterface defaults to all interfaces.\n"
		  "\n-q returns 0 if the interface (or gw) is up and has an IP address.\n"
		  "\nDesigned to be easily used in scripts. All error output to stderr.\n",
		  stderr);

	exit(rc);
}

#define MUST_ARGS(m, n) do {								\
		if ((what & ~(m)) || !ifname || argc - optind < n)	\
			usage(1);										\
	} while (0)


int main(int argc, char *argv[])
{
	int c, rc = 0;
	unsigned what = 0;
	char *ifname = NULL;

	while ((c = getopt(argc, argv, "abefgmisthqCDSTMV")) != EOF)
		switch (c) {
		case 'e':
			what |= W_ADDRESS | W_BITS | W_FLAGS | W_MAC;
			break;
		case 'i':
			what |= W_ADDRESS;
			break;
		case 'b':
			what |= W_BITS;
			break;
		case 'f':
			what |= W_FLAGS;
			break;
		case 'g':
			what |= W_GATEWAY;
			break;
		case 'm':
			what |= W_MASK;
			break;
		case 's':
			what |= W_SUBNET;
			break;
		case 't':
			what |= W_TOP_BYTE;
			break;
		case 'a':
			what |= W_ALL;
			break;
		case 'h':
			usage(0);
		case 'q':
			what |= W_QUIET;
			break;
		case 'C':
			what |= W_EXISTS;
			break;
		case 'D':
			what |= W_DOWN;
			break;
		case 'S':
			what |= W_SET;
			break;
		case 'T':
#ifdef __linux__
			what |= W_TUNTAP;
#else
			puts("Sorry, -T is Linux only.");
			exit(2);
#endif
			break;
		case 'M':
			what |= W_MAC;
			break;
		case 'V':
			what |= W_NO_VIRT;
			break;
		default:
			exit(2);
		}

	if (optind < argc)
		ifname = argv[optind++];

	if (optind < argc) {
		MUST_ARGS(W_SET | W_MAC, 1);
		char *ip = argv[optind++];

		if (what & W_MAC) {
			set_hw_addr(ifname, ip);
			return 0;
		}

		unsigned mask = 0;
		char *p = strchr(ip, '/');
		if (p) {
			*p++ = 0;
			unsigned bits = strtol(p, NULL, 10);
			mask = htonl(((1ul << bits) - 1) << (32 - bits));
		} else if (optind < argc) {
			mask = inet_addr(argv[optind]);
			++optind;
		} else
			usage(1);

		if (set_ip(ifname, ip, mask, 0))
			exit(1);
		if (optind < argc) {
			if (set_gateway(argv[optind])) {
				perror("set_gateway");
				exit(1);
			}
		}
		return 0;
	}

	if (what & W_EXISTS) {
		MUST_ARGS(W_EXISTS, 0);
		struct ifaddrs *ifa;
		if (getifaddrs(&ifa) == 0)
			for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
				if (strcmp(p->ifa_name, ifname) == 0)
					return 0;

		exit(1);
	}

	if (what & W_DOWN) {
		MUST_ARGS(W_DOWN, 0);
		if (set_ip(ifname, NULL, 0, 1))
			exit(1);
		return 0;
	}

#ifdef __linux__
	if (what & W_TUNTAP) {
		MUST_ARGS(W_TUNTAP, 0);
		return taptun(ifname);
	}
#else
	if (what == W_GATEWAY) {
		struct in_addr gw;
		if (get_gateway(NULL, &gw)) {
			perror("gateway");
			exit(1);
		}
		printf("%s\n", inet_ntoa(gw));
		return 0;
	}
#endif

	if ((what & ~(W_BITS | W_ALL | W_QUIET | W_NO_VIRT)) == 0)
		what |= W_ADDRESS;

	if (ifname)
		return check_one(ifname, NULL, 0, what);

	struct ifaddrs *ifa;
	if (getifaddrs(&ifa)) {
		perror("getifaddrs");
		exit(1);
	}

	for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK))
			continue;

		unsigned up = p->ifa_flags & IFF_UP;
		if ((what & W_ALL) || up) {
			if ((what & W_NO_VIRT) == 0 || strncmp(p->ifa_name, VIRBR, sizeof(VIRBR) - 1) != 0)
				rc |= check_one(p->ifa_name, p->ifa_addr, up, what | W_GUESSED);
		}
	}

	return rc;
}
