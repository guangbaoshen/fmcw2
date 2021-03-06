/*
 * Copyright 2012 Jared Boone <jared@sharebrained.com>
 * Copyright 2013-2014 Benjamin Vernoux <titanmkd@gmail.com>
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <hackrf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#ifndef bool
typedef int bool;
#define true 1
#define false 0
#endif

#ifdef _WIN32
#include <windows.h>

#ifdef _MSC_VER

#ifdef _WIN64
typedef int64_t ssize_t;
#else
typedef int32_t ssize_t;
#endif

#define strtoull _strtoui64
#define snprintf _snprintf

int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
		tmp -= 11644473600000000Ui64;
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

#endif
#endif

#if defined(__GNUC__)
#include <unistd.h>
#include <sys/time.h>
#endif

#include <signal.h>

#define FILE_VERSION (1)

#define FD_BUFFER_SIZE (8*1024)

#define FREQ_ONE_MHZ (1000000ull)
#define WRITE_BUFFER_SIZE (50*1024*1024)

volatile static char fwrite_buffer[WRITE_BUFFER_SIZE];
volatile static int fb_start = 0, fb_end = 0;
pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

#if defined _WIN32
	#define sleep(a) Sleep( (a*1000) )
#endif

static int buf_add(const uint8_t *s, int l) {
    //Add l bytes to buffer,
    //returns number of bytes left over
    int left = l;
    int fb = fb_end;
    int start = fb_start;
    while ( left > 0 ) {
        int fb_next = (fb + 1);
        if (fb_next >= WRITE_BUFFER_SIZE) {
            fb_next -= WRITE_BUFFER_SIZE;
        }
        if (fb_next == start) {
            //Reached end, acquire mutex and check if reader has made more room
            start = fb_start;
            //No more room, abort
            if (fb_next == start) {
                return left;
            }
        }
        fwrite_buffer[fb] = s[l-left];
        fb = fb_next;
        left--;
    }
    fb_end = fb;
    return left;
}

static int buf_size(void) {
    int bytes = fb_end - fb_start;
    if (bytes < 0) {
        bytes += WRITE_BUFFER_SIZE;
    }
    return bytes;
}

static int buf_get(uint8_t *dest, int max_bytes) {
    //Get max_bytes bytes from the buffer
    //Returns number of bytes read
    //pthread_mutex_lock(&buf_mutex);
    int bytes = fb_end - fb_start;
    if (bytes < 0) {
        bytes += WRITE_BUFFER_SIZE;
    }
    if (bytes > max_bytes) {
        bytes = max_bytes;
    }
    int i = 0;
    while ( bytes > 0) {
        dest[i++] = fwrite_buffer[fb_start];
        fb_start = (fb_start + 1) % WRITE_BUFFER_SIZE;
        bytes--;
    }
    return i;
}

static int buf_init(void)
{
    int ret = 0;
    ret = pthread_cond_init(&cond, NULL);
    if (ret != 0) {
        return ret;
    }

    ret = pthread_mutex_init(&buf_mutex, NULL);
    if (ret != 0) {
        pthread_cond_destroy(&cond);
        return ret;
    }

    ret = pthread_mutex_init(&writer_mutex, NULL);
    if (ret != 0) {
        pthread_cond_destroy(&cond);
        pthread_mutex_destroy(&buf_mutex);
        return ret;
    }
    return 0;

}

static float
TimevalDiff(const struct timeval *a, const struct timeval *b)
{
   return (a->tv_sec - b->tv_sec) + 1e-6f * (a->tv_usec - b->tv_usec);
}

int parse_u64(char* s, uint64_t* const value) {
	uint_fast8_t base = 10;
	char* s_end;
	uint64_t u64_value;

	if( strlen(s) > 2 ) {
		if( s[0] == '0' ) {
			if( (s[1] == 'x') || (s[1] == 'X') ) {
				base = 16;
				s += 2;
			} else if( (s[1] == 'b') || (s[1] == 'B') ) {
				base = 2;
				s += 2;
			}
		}
	}

	s_end = s;
	u64_value = strtoull(s, &s_end, base);
	if( (s != s_end) && (*s_end == 0) ) {
		*value = u64_value;
		return HACKRF_SUCCESS;
	} else {
		return HACKRF_ERROR_INVALID_PARAM;
	}
}

int parse_u32(char* s, uint32_t* const value) {
	uint_fast8_t base = 10;
	char* s_end;
	uint64_t ulong_value;

	if( strlen(s) > 2 ) {
		if( s[0] == '0' ) {
			if( (s[1] == 'x') || (s[1] == 'X') ) {
				base = 16;
				s += 2;
			} else if( (s[1] == 'b') || (s[1] == 'B') ) {
				base = 2;
				s += 2;
			}
		}
	}

	s_end = s;
	ulong_value = strtoul(s, &s_end, base);
	if( (s != s_end) && (*s_end == 0) ) {
		*value = (uint32_t)ulong_value;
		return HACKRF_SUCCESS;
	} else {
		return HACKRF_ERROR_INVALID_PARAM;
	}
}

volatile bool do_exit = false;

FILE* fd = NULL;
volatile uint32_t byte_count = 0;

bool limit_num_samples = false;
size_t bytes_to_xfer = 0;

struct timeval time_start;
struct timeval t_start;

volatile int thread_exit = 0;
volatile int thread_done = 0;

static void write_header(FILE *fd, double sample_rate, double f0, double bw, double tsweep, int delay, int flags) {
    char magic[] = "FMCW";
    int version = FILE_VERSION;
    //magic, version, header size, sample_rate, f0, bw, tsweep, delay, flags
    int header_length = 4+4+4+ 8+8+8+8+4+4;
    fwrite(magic, 1, 4, fd);
    fwrite(&version, 4, 1, fd);
    fwrite(&header_length, 4, 1, fd);
    fwrite(&sample_rate, 8, 1, fd);
    fwrite(&f0, 8, 1, fd);
    fwrite(&bw, 8, 1, fd);
    fwrite(&tsweep, 8, 1, fd);
    fwrite(&delay, 4, 1, fd);
    fwrite(&flags, 4, 1, fd);
}

static void* write_thread(void* arg) {
    uint8_t *fd_buf = malloc(WRITE_BUFFER_SIZE);
    if (!fd_buf) {
        printf("malloc failed\n");
        return 0;
    }
    int bytes_to_write;
    FILE *fout = (FILE*)arg;
    int wrote;
    while( !thread_exit ) {
        pthread_mutex_lock(&writer_mutex);
        //Wait until we get something to write
        while ( !(bytes_to_write = buf_get(fd_buf, WRITE_BUFFER_SIZE)) ) {
            pthread_cond_wait(&cond, &writer_mutex);
            if (thread_exit) {
                break;
            }
        }
        pthread_mutex_unlock(&writer_mutex);
        int written = 0;
        wrote = 0;
        while (written < bytes_to_write) {
            wrote = fwrite(&fd_buf[written], 1, bytes_to_write - wrote, fout);
            written += wrote;
        }
    }
    thread_done = 1;
    pthread_exit(NULL);
    return 0;
}

int rx_callback(hackrf_transfer* transfer) {
	size_t bytes_to_write;

	if( fd != NULL )
	{
		ssize_t bytes_left;
		byte_count += transfer->valid_length;
		bytes_to_write = transfer->valid_length;
		if (limit_num_samples) {
			if (bytes_to_write >= bytes_to_xfer) {
				bytes_to_write = bytes_to_xfer;
			}
			bytes_to_xfer -= bytes_to_write;
		}

        while ( (bytes_left = buf_add(transfer->buffer, bytes_to_write)) ) {
            printf("Buffer full\n");
        }

        //Signal to writer
        pthread_mutex_lock(&writer_mutex);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&writer_mutex);

        if ((bytes_left != 0)
                || (limit_num_samples && (bytes_to_xfer == 0))) {
            return -1;
        } else {
            return 0;
        }
	} else {
		return -1;
	}
}

static void usage() {
	printf("Usage:\n");
	printf("\t-r <filename> # Receive data into file.\n");
	printf("\t[-f freq_hz] # Sweep start frequency in Hz.\n");
	printf("\t[-b freq_hz] # Sweep bandwidth in Hz.\n");
	printf("\t[-t seconds] # Sweep length in seconds\n");
	printf("\t[-g 0<=x<=63] # MCP4022 gain setting.\n");
	printf("\t[-c x] # ADC clock divider. ADC clock = 204e6/(2*x).\n");
	printf("\t[-d clks] # Sweep delay in refernce clock cycles (Default 30 MHz)\n");
}

static hackrf_device* device = NULL;

#ifdef _MSC_VER
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stdout, "Caught signal %d\n", signum);
		do_exit = true;
		return TRUE;
	}
	return FALSE;
}
#else
void sigint_callback_handler(int signum)
{
	fprintf(stdout, "Caught signal %d\n", signum);
	do_exit = true;
}
#endif

#define PATH_FILE_MAX_LEN (FILENAME_MAX)
#define DATE_TIME_MAX_LEN (32)

int main(int argc, char** argv) {
	int opt;
	const char* path = NULL;
	int result;
	int exit_code = EXIT_SUCCESS;
	struct timeval t_end;
	float time_diff;

    /* Default parameters */
    double f0 = 5.6e9;
    double bw = 200e6;
    double tsweep = 1.0e-3;
    int delay = 1800;
    int mcp_gain = 0;
    int clk_divider = 20;

	while( (opt = getopt(argc, argv, "b:d:f:t:r:g:c:")) != EOF )
	{
		result = HACKRF_SUCCESS;
		switch( opt )
		{

		case 'r':
			path = optarg;
			break;

		case 'b':
			bw = atof(optarg);
            if (bw <= 0) {
                result = HACKRF_ERROR_INVALID_PARAM;
            }
			break;

		case 'f':
			f0 = atof(optarg);
            if (f0 <= 0) {
                result = HACKRF_ERROR_INVALID_PARAM;
            }
			break;

		case 't':
			tsweep = atof(optarg);
            if (tsweep < 0) {
                result = HACKRF_ERROR_INVALID_PARAM;
            }
			break;

		case 'd':
            delay = (int)strtol(optarg, (char **)NULL, 10);
            if (delay < 0) {
                result = HACKRF_ERROR_INVALID_PARAM;
            }
			break;

		case 'g':
            mcp_gain = (int)strtol(optarg, (char **)NULL, 10);
            if (mcp_gain < 0 || mcp_gain > 63) {
                result = HACKRF_ERROR_INVALID_PARAM;
            }
			break;

		case 'c':
            clk_divider = (int)strtol(optarg, (char **)NULL, 10);
            if (clk_divider <= 0) {
                result = HACKRF_ERROR_INVALID_PARAM;
            }
			break;

		default:
			printf("unknown argument '-%c %s'\n", opt, optarg);
			usage();
			return EXIT_FAILURE;
		}

		if( result != HACKRF_SUCCESS ) {
			printf("argument error: '-%c %s' %s (%d)\n", opt, optarg, hackrf_error_name(result), result);
			usage();
			return EXIT_FAILURE;
		}
	}


    if( path == NULL) {
        printf("No filename given");
        usage();
        return EXIT_FAILURE;
	}

    //Create thread for writing to file
    if (buf_init()) {
        printf("buf_init failed\n");
        return -1;
    }

	result = hackrf_init();
	if( result != HACKRF_SUCCESS ) {
		printf("hackrf_init() failed: %s (%d)\n", hackrf_error_name(result), result);
		usage();
		return EXIT_FAILURE;
	}

	result = hackrf_open_by_serial(NULL, &device);
	if( result != HACKRF_SUCCESS ) {
		printf("hackrf_open() failed: %s (%d)\n", hackrf_error_name(result), result);
		usage();
		return EXIT_FAILURE;
	}

    fd = fopen(path, "wb");
    if( fd == NULL ) {
        printf("Failed to open file: %s\n", path);
        return EXIT_FAILURE;
    }

    /* Change fd buffer to have bigger one to store or read data on/to HDD */
    result = setvbuf(fd , NULL , _IOFBF , FD_BUFFER_SIZE);
    if( result != 0 ) {
        printf("setvbuf() failed: %d\n", result);
        usage();
        return EXIT_FAILURE;
    }

    pthread_t writer;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&writer, &attr, write_thread, fd)) {
        printf("pthread_create failed\n");
        return -1;
    }


#ifdef _MSC_VER
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#else
	signal(SIGINT, &sigint_callback_handler);
	signal(SIGILL, &sigint_callback_handler);
	signal(SIGFPE, &sigint_callback_handler);
	signal(SIGSEGV, &sigint_callback_handler);
	signal(SIGTERM, &sigint_callback_handler);
	signal(SIGABRT, &sigint_callback_handler);
#endif

    result = hackrf_set_mcp(device, mcp_gain);
	if( result != HACKRF_SUCCESS ) {
		printf("hackrf_set_mcp() failed: %s (%d)\n", hackrf_error_name(result), result);
		return EXIT_FAILURE;
	}

    result = hackrf_set_sweep(device, f0, bw, tsweep, delay);
	if( result != HACKRF_SUCCESS ) {
		printf("hackrf_set_sweep() failed: %s (%d)\n", hackrf_error_name(result), result);
		return EXIT_FAILURE;
	}

    double sample_rate = 204e6/(2*clk_divider);
    result = hackrf_set_clock_divider(device, clk_divider);
    write_header(fd, sample_rate, f0, bw, tsweep, delay, 0);

    result = hackrf_start_rx(device, rx_callback, NULL);

	if( result != HACKRF_SUCCESS ) {
		printf("hackrf_start_rx() failed: %s (%d)\n", hackrf_error_name(result), result);
		usage();
		return EXIT_FAILURE;
	}

	gettimeofday(&t_start, NULL);
	gettimeofday(&time_start, NULL);

	printf("Stop with Ctrl-C\n");
	while( (hackrf_is_streaming(device) == HACKRF_TRUE) &&
			(do_exit == false) )
	{
		uint32_t byte_count_now;
		struct timeval time_now;
		float time_difference, rate;
		sleep(1);

		gettimeofday(&time_now, NULL);

		byte_count_now = byte_count;
		byte_count = 0;

		time_difference = TimevalDiff(&time_now, &time_start);
		rate = (float)byte_count_now / time_difference;
		printf("%4.1f MiB / %5.3f sec = %4.1f MiB/second\n",
				(byte_count_now / 1e6f), time_difference, (rate / 1e6f) );

		time_start = time_now;

		if (byte_count_now == 0) {
			exit_code = EXIT_FAILURE;
			printf("\nCouldn't transfer any bytes for one second.\n");
			break;
		}
	}

	result = hackrf_is_streaming(device);
	if (do_exit)
	{
		printf("\nUser cancel, exiting...\n");
	} else {
		printf("\nExiting... hackrf_is_streaming() result: %s (%d)\n", hackrf_error_name(result), result);
	}

	gettimeofday(&t_end, NULL);
	time_diff = TimevalDiff(&t_end, &t_start);
	printf("Total time: %5.5f s\n", time_diff);

	if(device != NULL)
	{
        result = hackrf_stop_rx(device);
        if( result != HACKRF_SUCCESS ) {
            printf("hackrf_stop_rx() failed: %s (%d)\n", hackrf_error_name(result), result);
        }else {
            printf("hackrf_stop_rx() done\n");
        }

		result = hackrf_close(device);
		if( result != HACKRF_SUCCESS )
		{
			printf("hackrf_close() failed: %s (%d)\n", hackrf_error_name(result), result);
		}else {
			printf("hackrf_close() done\n");
		}

		hackrf_exit();
		printf("hackrf_exit() done\n");
	}

    while ( buf_size() != 0 ) {
        sleep(1);
    }

    while ( !thread_done ) {
        thread_exit = 1;
        pthread_mutex_lock(&writer_mutex);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&writer_mutex);
    }

	if(fd != NULL)
	{
		fclose(fd);
		fd = NULL;
		printf("fclose(fd) done\n");
	}
	printf("exit\n");
	return exit_code;
}
