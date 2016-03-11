#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PGSIZE 4096
#define CPUS 64
#define MAX_NEEDLE_SIZE 1024
#define OVERLAP 4096

#define MAX(x,y) ((x)>(y)?(x):(y))
#define ASIZE 256
#define XSIZE MAX_NEEDLE_SIZE
#define XOR_KEY 42

#define HEXDUMP 0
#define DEBUG 0

static pthread_mutex_t cs_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

struct thread_params {
	char *haystack;
	long haystack_size;
	char *needle;
	long needle_size;
	long ofs;
};

#if HEXDUMP
/* http://stackoverflow.com/a/7776146/973041 */
static void hex_dump(char *desc, void *addr, int len)
{
	int i;
	unsigned char buff[17];
	unsigned char *pc = (unsigned char*)addr;

	// Output description if given.
	if (desc != NULL)
		printf ("%s:\n", desc);

	if (len == 0) {
		printf("  ZERO LENGTH\n");
		return;
	}
	if (len < 0) {
		printf("  NEGATIVE LENGTH: %i\n",len);
		return;
	}

	// Process every byte in the data.
	for (i = 0; i < len; i++) {
		// Multiple of 16 means new line (with line offset).

		if ((i % 16) == 0) {
			// Just don't print ASCII for the zeroth line.
			if (i != 0)
				printf ("  %s\n", buff);

			// Output the offset.
			printf ("  %04x ", i);
		}

		// Now the hex code for the specific character.
		printf (" %02x", pc[i]);

		// And store a printable ASCII character for later.
		if ((pc[i] < 0x20) || (pc[i] > 0x7e))
			buff[i % 16] = '.';
		else
			buff[i % 16] = pc[i];
		buff[(i % 16) + 1] = '\0';
	}

	// Pad out last line if not exactly 16 characters.
	while ((i % 16) != 0) {
		printf ("   ");
		i++;
	}

	// And print the final ASCII bit.
	printf ("  %s\n", buff);
}
#endif

/* Boyer-Moore algorithm from
 * http://www-igm.univ-mlv.fr/~lecroq/string/node14.html
 * plus the XOR_KEY hack so that the needle doesn't exist anywhere in our
 * program's memory */
static void preBmBc(unsigned char *x, long m, long bmBc[])
{
	long i;

	for (i = 0; i < ASIZE; ++i)
		bmBc[i] = m;
	for (i = 0; i < m - 1; ++i)
		bmBc[x[i] ^ XOR_KEY] = m - i - 1;
}

static void suffixes(unsigned char *x, long m, long *suff)
{
	long f, g, i;

	f = 0;
	suff[m - 1] = m;
	g = m - 1;
	for (i = m - 2; i >= 0; --i) {
		if (i > g && suff[i + m - 1 - f] < i - g)
			suff[i] = suff[i + m - 1 - f];
		else {
			if (i < g)
				g = i;
			f = i;
			while (g >= 0 && x[g] == x[g + m - 1 - f])
				--g;
			suff[i] = f - g;
		}
	}
}

static void preBmGs(unsigned char *x, long m, long bmGs[])
{
	long i, j, suff[XSIZE];

	suffixes(x, m, suff);

	for (i = 0; i < m; ++i)
		bmGs[i] = m;
	j = 0;
	for (i = m - 1; i >= 0; --i)
		if (suff[i] == i + 1)
			for (; j < m - 1 - i; ++j)
				if (bmGs[j] == m)
					bmGs[j] = m - 1 - i;
	for (i = 0; i <= m - 2; ++i)
		bmGs[m - 1 - suff[i]] = m - 1 - i;
}

static void BM(unsigned char *x, long m, unsigned char *y, long n, void (*found)(unsigned char *, long, long), long ofs)
{
	long i, j, bmGs[XSIZE], bmBc[ASIZE];

	/* Preprocessing */
	preBmGs(x, m, bmGs);
	preBmBc(x, m, bmBc);

	/* Searching */
	j = 0;
	while (j <= n - m) {
		for (i = m - 1; i >= 0 && (x[i] ^ XOR_KEY) == y[i + j]; --i);
		if (i < 0) {
			found(y, j, ofs);
			j += bmGs[0];
		}
		else
			j += MAX(bmGs[i], bmBc[y[i + j]] - m + 1 + i);
	}
}

static void found(unsigned char *haystack, long pos, long ofs)
{
	pthread_mutex_lock( &cs_mutex );
	pos += ofs;
	printf("Found at %ld (page %ld, offset %08lx)\n", pos, pos / PGSIZE, pos % PGSIZE);
#if HEXDUMP
	hex_dump(NULL, (void *) ((long) (haystack + pos - ofs - 32)), 64);
#endif
	pthread_mutex_unlock( &cs_mutex );
}

static void *thread_main(void *arg)
{
	struct thread_params *params = arg;
	BM((unsigned char *)params->needle, params->needle_size, (unsigned char *)params->haystack, params->haystack_size, found, params->ofs);
	return 0;
}

static void pgrep(int cpu_count, char *haystack, long haystack_size, char *needle, long needle_size, long ofs)
{
	int i;
	struct thread_params params[CPUS];
	pthread_t threads[CPUS];
	long thread_mem_size;
	char *thread_haystack = haystack;

#if DEBUG
	fprintf(stderr, "thread_mem_size = %lx\n", thread_mem_size);
#endif

	if (haystack_size < 1024 * 1024)
		cpu_count = 1;
	thread_mem_size = haystack_size / cpu_count + OVERLAP;

	for (i = 0; i < cpu_count; i++) {
		if (thread_mem_size > haystack + haystack_size - thread_haystack)
			thread_mem_size = haystack + haystack_size - thread_haystack;
		params[i].haystack = thread_haystack;
		params[i].haystack_size = thread_mem_size;
		params[i].needle = needle;
		params[i].needle_size = needle_size;
		params[i].ofs = ofs + thread_haystack - haystack;
		thread_haystack += thread_mem_size - OVERLAP;
#if DEBUG
		fprintf(stderr, "starting thread ofs=%012lx size=%lx\n", params[i].ofs, params[i].haystack_size);
#endif
		pthread_create(&threads[i], NULL, thread_main, &params[i]);
	}

	for (i = 0; i < cpu_count; i++) {
		pthread_join(threads[i], NULL);
#if DEBUG
		fprintf(stderr, "finished thread ofs=%012lx size=%lx\n", params[i].mem - mem, params[i].mem_size);
#endif
	}

}

int main(int argc, char **argv)
{
	int cpu_count;
	int fd;
	int i;
	char needle[MAX_NEEDLE_SIZE];
	long mem_size;
	char *mem;
	char buf[4096];
	long start, end;

#if 0
	fd = open("tmp", O_RDONLY);
	needle[0] = 0;
	for (i = 0; i < atoi(argv[1]); i++)
		strcat(needle, argv[2]);
	for (i = 0; i < strlen(needle); i++)
		needle[i] ^= XOR_KEY;
	mem = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
	pgrep(1, mem, 4096, needle, strlen(needle), 0);

	return 0;
#endif

	if (argc < 3) {
		fprintf(stderr, "missing argument\n");
		return 1;
	}

	fd = open("/dev/allmem", O_RDONLY);
	cpu_count = sysconf(_SC_NPROCESSORS_CONF);
	needle[0] = 0;
	for (i = 0; i < atoi(argv[1]); i++)
		strcat(needle, argv[2]);

	for (i = 0; i < strlen(needle); i++)
		needle[i] ^= XOR_KEY;

	FILE *f = fopen("/proc/iomem", "r");
	while (fgets(buf, sizeof(buf), f) != NULL) {
		if (buf[0] == ' ')
			continue;
		if (strstr(buf, "PCI")) {
			fprintf(stderr, "Ignoring %s", buf);
			continue;
		}
		sscanf(buf, "%lx-%lx", &start, &end);
		mem_size = end - start + 1;
		mem = mmap(0, mem_size, PROT_READ, MAP_SHARED, fd, start);
		if (mem == MAP_FAILED) {
			fprintf(stderr, "Map failed for %s", buf);
			continue;
		}
#if DEBUG
		fprintf(stderr, "pgrep start=%lx %s", start, buf);
#endif
		pgrep(cpu_count, mem, mem_size, needle, strlen(needle), start);
	}
	fclose(f);

	return 0;
}
