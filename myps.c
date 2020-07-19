#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>

static struct aproc {
	const char *cmd;
	int count;
	pid_t pid;
	unsigned long long time;
} *procs;
static int curproc;

static pid_t me;

static int readproc(pid_t pid, const char *file, char *buf, int len)
{
	char fname[32];
	int fd, n;

	*buf = 0;

	snprintf(fname, sizeof(fname), "/proc/%u/%s", pid, file);
	fd = open(fname, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);

	/* Zero length is not an error. Some files, like a kernel thread
	 * cmdline, are zero length.
	 */
	if (n < 0)
		return -1;

	buf[n] = 0;
	return n;
}

/* Note: /proc/pid/cmdline is limited to 4k */
static int readproccmdline(pid_t pid, char *buf, int len)
{
	int i, n = readproc(pid, "cmdline", buf, len);

	if (n > 0)
		for (i = 0; i < n - 1; ++i)
			if (buf[i] == 0)
				buf[i] = ' ';

	return n;
}

static unsigned long long readstarttime(pid_t pid)
{
	char buf[128];

	int n = readproc(pid, "stat", buf, sizeof(buf));
	if (n <= 0)
		return 0;

	// Sighhh... firefox creates a (Web Content) entry
	char *p = strchr(buf, ')');
	if (!p)
		return 0;

	for (int i = 0; i < 20; ++i) {
		p = strchr(p, ' ');
		if (!p)
			return 0;
		++p;
	}

	return strtol(p, NULL, 10);
}

static int add_proc(pid_t pid)
{
	static int maxproc;
	struct aproc *p;

	if (pid == me) return 0;

	char buf[0x1001];
	int n = readproccmdline(pid, buf, sizeof(buf));
	if (n <= 0) {
		if (n < 0)
			fprintf(stderr, "%d: readproc failed\n", pid);
		return 0;
	}

	unsigned long long starttime = readstarttime(pid);
	if (starttime == 0) {
		fprintf(stderr, "%d: readstarttime failed\n", pid);
		return 0;
	}

	/* /bin/sh is a special case */
	char *cmd = buf;
	if (strncmp(cmd, "/bin/sh", 7) == 0) {
		if (*(cmd + 7)) {
#if 1
			// This drops the /bin/sh
			cmd += 8;
#else
			// This keeps the /bin/sh
			char *ptr = strchr(cmd + 8, ' ');
			if (ptr) *ptr = 0;
#endif
		}
	} else {
		char *ptr = strchr(cmd, ' ');
		if (ptr) *ptr = 0;
	}

	p = procs;
	for (int i = 0; i < curproc; ++i, ++p)
		if (strcmp(cmd, p->cmd) == 0) {
			++p->count;
			if (pid < p->pid)
				p->pid = pid;
			if (starttime < p->time)
				p->time = starttime;
			return 0;
		}

	if (curproc >= maxproc) {
		maxproc += 10;
		procs = realloc(procs, maxproc * sizeof(struct aproc));
		if (!procs) {
			fputs("Out of memory!", stderr);
			exit(1);
		}
	}
	p = &procs[curproc++];
	p->cmd = strdup(cmd);
	if (!p->cmd) {
		fputs("Out of memory.", stderr);
		exit(1);
	}
	p->count = 1;
	p->pid = pid;
	p->time = starttime;
	return 0;
}

static int proc_cmp(const void *a, const void *b)
{
	const struct aproc *a1 = a;
	const struct aproc *b1 = b;

	if (a1->time == b1->time)
		return a1->pid < b1->pid ? -1 : 1;
	return ((struct aproc *)a)->time < ((struct aproc *)b)->time ? -1 : 1;
}

int do_match(const struct aproc *p, const char *match, int word)
{
	char *ptr = strstr(p->cmd, match);
	if (ptr == NULL)
		return 0;

	if (word) {
		if (!(ptr == p->cmd || *(ptr - 1) == '/'))
			return 0; // mismatch at start
		ptr += strlen(match);
		if (*ptr)
			return 0; // mismatch at end
	}

	if (p->count > 1)
		printf("%5d %s (%d)\n", p->pid, p->cmd, p->count);
	else
		printf("%5d %s\n", p->pid, p->cmd);

	return 1;
}

int main(int argc, char *argv[])
{
	int c, word = 0, rc = 0;
	const char *match = NULL;

	while ((c = getopt(argc, argv, "w")) != EOF)
		if (c == 'w')
			word = 1;

	if (optind < argc) {
		match = argv[optind];
		rc = 1;
	}

	me = getpid();

	DIR *dir = opendir("/proc");
	if (!dir) {
		perror("/proc");
		exit(1);
	}

	struct dirent *ent;
	while ((ent = readdir(dir))) {
		char *e;
		pid_t pid = strtol(ent->d_name, &e, 10);
		if (*e == 0)
			add_proc(pid);
	}

	qsort(procs, curproc, sizeof(struct aproc), proc_cmp);

	struct aproc *p;
	int i;
	for (p = procs, i = 0; i < curproc; ++i, ++p) {
		if (match) {
			if (do_match(p, match, word))
				rc = 0;
		} else if (p->count > 1)
			printf("%5d %s (%d)\n", p->pid, p->cmd, p->count);
		else
			printf("%5d %s\n", p->pid, p->cmd);
	}

	return rc;
}
