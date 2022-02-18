/*
 * korad.c
 *
 * Copyright 2022 Franz Brauße <fb@paxle.org>
 *
 * SPDX: WTFPL
 */

#define _GNU_SOURCE 1

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define DIE(code,...) do { fprintf(stderr, __VA_ARGS__); exit(code); } while (0)

static FILE *f;
static char *line;
static size_t sz;
static ssize_t rd;

static void * kmalloc(size_t n)
{
	return n ? malloc(n) : NULL;
}

static void kfree(void *ptr)
{
	if (ptr)
		free(ptr);
}

static void * krealloc(void *ptr, size_t sz)
{
	if (!sz) {
		kfree(ptr);
		return NULL;
	}
	return realloc(ptr, sz);
}

static void send(long wait_ns, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	putc('\n', f);
	for (struct timespec rem = { 0, wait_ns };
	     (errno = 0, nanosleep(&rem, &rem) == -1) && errno == EINTR;);
	if (errno)
		perror("nanosleep"), exit(2);
}

static char * recv(const char *fmt)
{
	rd = getline(&line, &sz, f);
	if (rd <= 0)
		DIE(2,"error reading %s output\n",fmt);
	while (rd && line[rd-1] == '\n')
		line[--rd] = '\0';
	return line;
}

#define _comm(fmt, ...) (send(0, fmt, __VA_ARGS__), recv(fmt))
#define comm(...) _comm(__VA_ARGS__, 0)

#define CSI	"\x1b["
#define RED	CSI "91m"
#define GREEN	CSI "92m"
#define MAGENTA	CSI "95m"
#define CYAN	CSI "96m"
#define RESET	CSI "0m"

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyACM0";

	const char *iset = NULL, *uset = NULL, *out = NULL, *ocp = NULL;
	const char *save = NULL, *rest = NULL;
	int print_status = 0, print_version = 0, force = 0;

	for (int opt; (opt = getopt(argc, argv, ":fD:hsI:U:S:R:o:O:v")) != -1;)
		switch (opt) {
		case 'D': dev = optarg; break;
		case 'I': iset = optarg; break;
		case 'U': uset = optarg; break;
		case 'o': out = optarg; break;
		case 'O': ocp = optarg; break;
		case 'S': save = optarg; break;
		case 'R': rest = optarg; break;
		case 's': print_status = 1; break;
		case 'v': print_version = 1; break;
		case 'f': force = 1; break;
		case 'h':
			printf("\
usage: %s [-OPTS]\n\
\n\
Options [defaults]:\n\
  -f        force usage of device even if the version does not match\n\
  -s        print status\n\
  -v        print version information\n\
  -h        print this help message\n\
  -D DEV    use device path DEV [%s]\n\
  -I x.xxx  set maximum output current in Ampere\n\
  -U xx.xx  set maximum output voltage in Volt\n\
  -o {0|1}  turn output off or on\n\
  -O {0|1}  turn over-current protection off or on\n\
  -S {1-5}  store current U/I settings in memory slot\n\
  -R {1-5}  restore U/I settings from memory slot\n\
\n\
Written by Franz Brauße <fb@paxle.org>\n\
", argv[0], dev);
			exit(0);
		case ':':
			DIE(1,"error: option '-%c' requires a parameter\n",
			    optopt);
		case '?':
			DIE(1,"error: unknown option '-%c'\n",optopt);
		}

	int fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd == -1)
		perror(dev), exit(1);

	f = fdopen(fd, "a+");
	if (!f)
		perror("fdopen"), exit(2);

	char *id = strdup(comm("*IDN?"));
	if (print_version)
		printf("device identified as: %s\n", id);

	const char *toks[4];
	toks[0] = strtok(line, " ");
	toks[1] = strtok(NULL, " ");
	toks[2] = strtok(NULL, " ");
	toks[3] = strtok(NULL, " ");
	if (!force && (strcmp(toks[0], "KORAD") || strcmp(toks[1], "KD3005P") ||
	               strcmp(toks[2], "V6.6") || strncmp(toks[3], "SN:", 3)))
		DIE(1,"error: device identified as '%s'. Unknown, aborting.\n",
		    id);
	free(id);

	if (iset)
		send(50e6, "ISET1:%s", iset);
	if (uset)
		send(50e6, "VSET1:%s", uset);
	if (out)
		send(50e6, "OUT%s", out);
	if (ocp)
		send(50e6, "OCP%s", ocp);
	if (save)
		send(50e6, "SAV%s", save);
	if (rest)
		send(50e6, "RCL%s", rest);

	if (print_status) {
		const char *on, *off, *ufmt = "", *ifmt = "", *reset = "";
		if (isatty(STDOUT_FILENO)) {
			on    = GREEN   "on"  RESET;
			off   = RED     "off" RESET;
			ufmt  = MAGENTA;
			ifmt  = CYAN;
			reset = RESET;
		} else {
			on    =         "on";
			off   =         "off";
		}

		unsigned char status = *comm("STATUS?");
		int cv_mode     =  status & 0x01; /* otherwise: cc mode */
		int ocp_enabled =  status & 0x20; /* undocumented */
		int out_enabled =  status & 0x40;
		printf("constant %s%s%s mode, ocp %s, output %s (0x%02hhx)",
		       cv_mode ? ufmt : ifmt,
		       cv_mode ? "voltage" : "current",
		       reset,
		       ocp_enabled ? on : off,
		       out_enabled ? on : off,
		       status);
		printf(", set to %s%s%sV", ufmt, comm("VSET1?"), reset);
		printf(" / %s%s%sA", ifmt, comm("ISET1?"), reset);
		printf(", actual output: %s%s%sV", ufmt, comm("VOUT1?"), reset);
		printf(" / %s%s%sA", ifmt, comm("IOUT1?"), reset);
		printf("\n");
	}
}
