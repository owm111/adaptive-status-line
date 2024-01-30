#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#ifdef X
#include <X11/Xlib.h>
#else /* X */
#define DefaultRootWindow(x) (-1)
#define Display void
#define XCloseDisplay(x) (-1)
#define XFlush(dpy) NULL
#define XOpenDisplay(x) NULL
#define XStoreName(x, y, z) (-1)
#endif /* X */

#define LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#ifdef X
#define XFLAG " [-x]"
#define XALLOWED 1
#else /* X */
#define XFLAG ""
#define XALLOWED 0
#endif /* X */
#define SEPARATOR "   "
#define INTERVAL 5
#define BATTERY_NAME "BAT0"
#define INTERFACE_NAME "wlp59s0"

static int done = 0;
static int x = 0;
static Display *dpy;
static char xbuf[4096];

static void
onsignal(int signum)
{
	if (signum != SIGUSR1)
		done = 1;
}

static int
wifi(FILE *stream)
{
	int rc;
	FILE *f;
	char ch;
	int linkquality;

	f = fopen("/sys/class/net/" INTERFACE_NAME "/operstate", "r");
	if (f == NULL)
		return 0;
	ch = fgetc(f);
	fclose(f);
	if (ch == EOF)
		return 0;
	if (ch != 'u') {
		return fprintf(stream, "wifi n/a");
	}
	f = fopen("/proc/net/wireless", "r");
	if (f == NULL)
		goto how;
	/* line 1: junk */
	rc = fscanf(f, "%*[^\n]\n");
	if (rc != 0)
		goto what;
	/* line 2: junk */
	rc = fscanf(f, "%*[^\n]\n");
	if (rc != 0)
		goto what;
	/* line 3: if: status linkquality linklevel linknoise ... */
	/* XXX this doesn't work if there is more than one interface! */
	rc = fscanf(f, INTERFACE_NAME ": %*d %d", &linkquality);
	if (rc == 0)
		goto what;
	fclose(f);
	return fprintf(stream, "wifi %d", 100 * linkquality / 70);
what:	fclose(f);
how:	return fprintf(stream, "wifi ???");
}

static int
disks(FILE *stream)
{
	int n;
	FILE *df;
	char ch;

	df = popen("df | awk '"
			"/^\\/dev\\/nvme/ {"
				"gsub(/\\/dev\\/|n.p./, \"\");"
				"total[$1] += $2;"
				"used[$1] += $3;"
			"}"
			"END {"
				"for (disk in total) {"
					"if (notfirst) printf \" \";"
					"notfirst = 1;"
					"sprintf(\"numfmt --to=iec-i %d\", 1024 * (total[disk] - used[disk])) | getline free;"
					"printf \"%s %d%% %s\", disk, 100 * used[disk] / total[disk], free;"
				"}"
			"}"
			"'", "r");
	if (df == NULL)
		return 0;
	/* XXX is this terribly inefficient? */
	for (ch = fgetc(df), n = 0; ch != EOF; ch = fgetc(df), n++)
		fputc(ch, stream);
	pclose(df);
	return n;
}

static int
mem(FILE *stream)
{
	int rc;
	FILE *meminfo;
	long unsigned int pct, total, free, available;

	meminfo = fopen("/proc/meminfo", "r");
	if (meminfo == NULL)
		return 0;
	rc = fscanf(meminfo, "MemTotal: %lu kB "
			"MemFree: %lu kB "
			"MemAvailable: %lu kB ",
			&total, &free, &available);
	fclose(meminfo);
	if (rc != 3)
		return 0;
	pct = 100lu * (total - available) / total;
	return fprintf(stream, "mem %lu%%", pct);
}

static int
load(FILE *stream)
{
	int rc;
	float load;
	FILE *loadavg;

	loadavg = fopen("/proc/loadavg", "r");
	if (loadavg == NULL)
		return 0;
	rc = fscanf(loadavg, "%f", &load);
	fclose(loadavg);
	if (rc != 1)
		return 0;
	return fprintf(stream, "load %.2f", load);
}

static int
alsa(FILE *stream)
{
	int rc;
	FILE *amixer;
	int pct;
	char ch;

	amixer = popen("amixer sget Master | awk '"
			"/Front Left:/ {gsub(/[%[\\]]/, \"\"); print $5, $6}"
			"'", "r");
	if (amixer == NULL)
		return 0;
	rc = fscanf(amixer, "%d o%c", &pct, &ch);
	pclose(amixer);
	if (rc != 2)
		return 0;
	if (ch == 'n')
		return fprintf(stream, "vol %d%%", pct);
	return fprintf(stream, "vol muted");
}

static char
batterychar(char ch)
{
	switch (ch) {
	case 'C':
		return '+';
	case 'D':
		return '-';
	case 'I':
		return 'o';
	case 'F':
		return '=';
	default: /* 'U' */
		return '?';
	}
}

static int
batteries(FILE *stream)
{
	int rc;
	FILE *f;
	int capacity;
	char ch;

	/* idea: use glob to find batteries */
	f = fopen("/sys/class/power_supply/" BATTERY_NAME "/capacity", "r");
	if (f == NULL)
		return 0;
	rc = fscanf(f, "%d", &capacity);
	fclose(f);
	if (rc != 1)
		return 0;
	f = fopen("/sys/class/power_supply/" BATTERY_NAME "/status", "r");
	if (f == NULL)
		return 0;
	ch = fgetc(f);
	fclose(f);
	if (ch == EOF)
		return 0;
	return fprintf(stream, "bat %c%d%%", batterychar(ch), capacity);
}

static int
datetime(FILE *stream)
{
	time_t now;
	char buffer[26];

	time(&now);
	ctime_r(&now, buffer);
	*strrchr(buffer, ':') = '\0';
	return fprintf(stream, "%s", buffer);
}

static const struct timespec sleepinterval = {
	.tv_sec = INTERVAL,
	.tv_nsec = 0,
};

static int (*const blocks[])(FILE *) = {
	wifi,
	disks,
	mem,
	load,
	alsa,
	batteries,
	datetime,
};

static int
printline(FILE *stream)
{
	unsigned int i;
	int rc, total;

	for (i = 0, total = rc = 0; i < LEN(blocks); i++) {
		if (rc != 0) {
			total += fprintf(stream, SEPARATOR);
		}
		total += rc = blocks[i](stream);
	}
	return total;
}

int
main(int argc, char **argv)
{
	int i;
	int rc;
	FILE *memstream;
	struct sigaction action = {
		.sa_handler = onsignal,
	};

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			fprintf(stderr, "astatus-0.0\n");
			return 0;
		} else if (strcmp(argv[i], "-1") == 0) {
			done = 1;
		} else if (XALLOWED && strcmp(argv[i], "-x") == 0) {
			x = 1;
		} else if (strcmp(argv[i], "-s") == 0) {
			x = 0;
		} else {
			fprintf(stderr, "usage: %s [-1] [-s]" XFLAG "\n",
					argv[0]);
			return 1;
		}
	}
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);
	if (x) {
		dpy = XOpenDisplay(NULL);
		if (dpy == NULL) {
			fprintf(stderr, "%s: XOpenDisplay: Failed to open "
					"display\n", argv[0]);
			return 1;
		}
	}
	do {
		if (x) {
			memstream = fmemopen(xbuf, sizeof(xbuf), "w");
			if (memstream == NULL) {
				perror("fmemopen");
				return 1;
			}
			printline(memstream);
			fclose(memstream);
			rc = XStoreName(dpy, DefaultRootWindow(dpy), xbuf);
			if (rc < 0) {
				fprintf(stderr, "%s: XStoreName: Allocation "
						"failed\n", argv[0]);
				return 1;
			}
			XFlush(dpy);
		} else {
			printline(stdout);
			printf("\n");
			fflush(stdout);
		}
		nanosleep(&sleepinterval, NULL);
	} while (!done);
	if (x) {
		xbuf[0] = '\0';
		rc = XStoreName(dpy, DefaultRootWindow(dpy), xbuf);
		if (rc < 0) {
			fprintf(stderr, "%s: XStoreName: Allocation "
					"failed\n", argv[0]);
			return 1;
		}
		rc = XCloseDisplay(dpy);
		if (rc < 0) {
			fprintf(stderr, "%s: XCloseDisplay: Failed to close "
					"display\n", argv[0]);
			return 1;
		}
	}
	return 0;
}
