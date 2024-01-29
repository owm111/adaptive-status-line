#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#define LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define SEPARATOR "   "

enum result {OK, NO_OUTPUT};

static enum result
disks(FILE *stream)
{
	FILE *df;
	char ch;

	df = popen("df -h | sort | awk '"
			"/^\\/dev\\/nvme/ {"
				"gsub(/\\/dev\\/|n.p./, \"\");"
				"disks[$1] = $4;"
			"}"
			"END {"
				"for (disk in disks) {"
					"if (notfirst) printf \" \";"
					"notfirst = 1;"
					"printf \"%s %s\", disk, disks[disk];"
				"}"
			"}"
			"'", "r");
	if (df == NULL)
		return NO_OUTPUT;
	/* XXX is this terribly inefficient? */
	for (ch = fgetc(df); ch != EOF; ch = fgetc(df))
		fputc(ch, stream);
	pclose(df);
	return OK;
}

static enum result
mem(FILE *stream)
{
	int rc;
	FILE *meminfo;
	long unsigned int pct, total, free, available;

	meminfo = fopen("/proc/meminfo", "r");
	if (meminfo == NULL)
		return NO_OUTPUT;
	rc = fscanf(meminfo, "MemTotal: %lu kB "
			"MemFree: %lu kB "
			"MemAvailable: %lu kB ",
			&total, &free, &available);
	fclose(meminfo);
	if (rc != 3)
		return NO_OUTPUT;
	pct = 100lu * (total - available) / total;
	fprintf(stream, "mem %lu%%", pct);
	return OK;
}

static enum result
load(FILE *stream)
{
	int rc;
	float load;
	FILE *loadavg;

	loadavg = fopen("/proc/loadavg", "r");
	if (loadavg == NULL)
		return NO_OUTPUT;
	rc = fscanf(loadavg, "%f", &load);
	fclose(loadavg);
	if (rc != 1)
		return NO_OUTPUT;
	fprintf(stream, "load %.2f", load);
	return OK;
}

static enum result
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
		return NO_OUTPUT;
	rc = fscanf(amixer, "%d o%c", &pct, &ch);
	pclose(amixer);
	if (rc != 2)
		return NO_OUTPUT;
	if (ch == 'n')
		fprintf(stream, "vol %d%%", pct);
	else
		fprintf(stream, "vol muted");
	return OK;
}

static enum result
batteries(FILE *stream)
{
	int rc;
	FILE *f;
	int capacity;
	char ch;

	/* XXX /sys/class/power_supply */
}

static enum result
datetime(FILE *stream)
{
	time_t now;
	char buffer[26];

	time(&now);
	ctime_r(&now, buffer);
	*strrchr(buffer, ':') = '\0';
	fprintf(stream, "%s", buffer);
	return OK;
}

enum result (*blocks[])(FILE *) = {
	disks,
	mem,
	load,
	alsa,
	datetime,
};

int
main(void)
{
	unsigned int i;
	enum result rc = NO_OUTPUT;

	for (i = 0; i < LEN(blocks); i++) {
		if (rc != NO_OUTPUT) {
			fprintf(stdout, SEPARATOR);
		}
		rc = blocks[i](stdout);
	}
	return 0;
}
