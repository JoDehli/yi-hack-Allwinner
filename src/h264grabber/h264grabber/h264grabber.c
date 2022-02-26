/*
 * Copyright (c) 2022 roleo.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Dump h264 and aac content from /dev/shm/fshare_frame_buffer to stdout or fifo
 */

#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>

//#define USE_SEMAPHORE 1
#ifdef USE_SEMAPHORE
#include <semaphore.h>
#endif

#define BUF_OFFSET_Y20GA 300
#define FRAME_HEADER_SIZE_Y20GA 22

#define BUF_OFFSET_Y25GA 300
#define FRAME_HEADER_SIZE_Y25GA 22

#define BUF_OFFSET_Y30QA 300
#define FRAME_HEADER_SIZE_Y30QA 22

#define RESOLUTION_NONE 0
#define RESOLUTION_LOW  360
#define RESOLUTION_HIGH 1080
#define RESOLUTION_BOTH 1440

#define TYPE_NONE 0
#define TYPE_LOW  360
#define TYPE_HIGH 1080
#define TYPE_AAC 65521

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define BUFFER_SHM "fshare_frame_buf"
#define READ_LOCK_FILE "fshare_read_lock"
#define WRITE_LOCK_FILE "fshare_write_lock"

#define FIFO_NAME_LOW "/tmp/h264_low_fifo"
#define FIFO_NAME_HIGH "/tmp/h264_high_fifo"
#define FIFO_NAME_AAC  "/tmp/aac_audio_fifo"

struct __attribute__((__packed__)) frame_header {
    uint32_t len;
    uint32_t counter;
    uint32_t time;
    uint16_t type;
    uint16_t stream_counter;
};

struct __attribute__((__packed__)) frame_header_22 {
    uint32_t len;
    uint32_t counter;
    uint32_t u1;
    uint32_t time;
    uint16_t type;
    uint16_t stream_counter;
    uint16_t u4;
};

int buf_offset;
int buf_size;
int frame_header_size;

unsigned char IDR4[]                = {0x65, 0xB8};
unsigned char NALx_START[]          = {0x00, 0x00, 0x00, 0x01};
unsigned char IDR4_START[]          = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
unsigned char PFR4_START[]          = {0x00, 0x00, 0x00, 0x01, 0x41};
unsigned char SPS4_START[]          = {0x00, 0x00, 0x00, 0x01, 0x67};
unsigned char PPS4_START[]          = {0x00, 0x00, 0x00, 0x01, 0x68};
unsigned char SPS4_640X360[]        = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                       0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                       0x01, 0x01, 0x02};
unsigned char SPS4_640X360_TI[]     = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                       0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                       0x01, 0x01, 0x40, 0x00, 0x00, 0x7D, 0x00, 0x00,
                                       0x13, 0x88, 0x21};
unsigned char SPS4_1920X1080[]      = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                       0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                       0x04, 0x04, 0x04, 0x08};
unsigned char SPS4_1920X1080_TI[]   = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                       0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                       0x04, 0x04, 0x05, 0x00, 0x00, 0x03, 0x01, 0xF4,
                                       0x00, 0x00, 0x4E, 0x20, 0x84};

unsigned char *addr;                      /* Pointer to shared memory region (header) */
int resolution;
int sps_timing_info;
int fifo;
int debug;

#ifdef USE_SEMAPHORE
sem_t *sem_fshare_read_lock = SEM_FAILED;
sem_t *sem_fshare_write_lock = SEM_FAILED;
#endif

long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // calculate milliseconds

    return milliseconds;
}

/* Locate a string in the circular buffer */
unsigned char *cb_memmem(unsigned char *src, int src_len, unsigned char *what, int what_len)
{
    unsigned char *p;

    if (src_len >= 0) {
        p = (unsigned char*) memmem(src, src_len, what, what_len);
    } else {
        // From src to the end of the buffer
        p = (unsigned char*) memmem(src, addr + buf_size - src, what, what_len);
        if (p == NULL) {
            // And from the start of the buffer size src_len
            p = (unsigned char*) memmem(addr + buf_offset, src + src_len - (addr + buf_offset), what, what_len);
        }
    }
    return p;
}

unsigned char *cb_move(unsigned char *buf, int offset)
{
    buf += offset;
    if ((offset > 0) && (buf > addr + buf_size))
        buf -= (buf_size - buf_offset);
    if ((offset < 0) && (buf < addr + buf_offset))
        buf += (buf_size - buf_offset);

    return buf;
}

// The second argument is the circular buffer
int cb_memcmp(unsigned char *str1, unsigned char *str2, size_t n)
{
    int ret;

    if (str2 + n > addr + buf_size) {
        ret = memcmp(str1, str2, addr + buf_size - str2);
        if (ret != 0) return ret;
        ret = memcmp(str1 + (addr + buf_size - str2), addr + buf_offset, n - (addr + buf_size - str2));
    } else {
        ret = memcmp(str1, str2, n);
    }

    return ret;
}

// The second argument is the circular buffer
void cb2s_memcpy(unsigned char *dest, unsigned char *src, size_t n)
{
    if (src + n > addr + buf_size) {
        memcpy(dest, src, addr + buf_size - src);
        memcpy(dest + (addr + buf_size - src), addr + buf_offset, n - (addr + buf_size - src));
    } else {
        memcpy(dest, src, n);
    }
}

// The second argument is the circular buffer
void cb2s_headercpy(unsigned char *dest, unsigned char *src, size_t n)
{
    struct frame_header *fh = (struct frame_header *) dest;
    struct frame_header_22 fh22;
    unsigned char *fp = NULL;

    if (n == sizeof(fh22)) {
        fp = (unsigned char *) &fh22;
    }
    if (fp == NULL) return;

    if (src + n > addr + buf_size) {
        memcpy(fp, src, addr + buf_size - src);
        memcpy(fp + (addr + buf_size - src), addr + buf_offset, n - (addr + buf_size - src));
    } else {
        memcpy(fp, src, n);
    }
    if (n == sizeof(fh22)) {
        fh->len = fh22.len;
        fh->counter = fh22.counter;
        fh->time = fh22.time;
        fh->type = fh22.type;
        fh->stream_counter = fh22.stream_counter;
    }
}

#ifdef USE_SEMAPHORE
int sem_fshare_open()
{
    sem_fshare_read_lock = sem_open(READ_LOCK_FILE, O_RDWR);
    if (sem_fshare_read_lock == SEM_FAILED) {
        fprintf(stderr, "error opening %s\n", READ_LOCK_FILE);
        return -1;
    }
    sem_fshare_write_lock = sem_open(WRITE_LOCK_FILE, O_RDWR);
    if (sem_fshare_write_lock == SEM_FAILED) {
        fprintf(stderr, "error opening %s\n", WRITE_LOCK_FILE);
        return -2;
    }
    return 0;
}

void sem_fshare_close()

{
    if (sem_fshare_write_lock != SEM_FAILED) {
        sem_close(sem_fshare_write_lock);
        sem_fshare_write_lock = SEM_FAILED;
    }
    if (sem_fshare_read_lock != SEM_FAILED) {
        sem_close(sem_fshare_read_lock);
        sem_fshare_read_lock = SEM_FAILED;
    }
    return;
}

void sem_write_lock()
{
    int wl, ret = 0;
    int *fshare_frame_buf_start = (int *) addr;

    while (ret == 0) {
        sem_wait(sem_fshare_read_lock);
        wl = *fshare_frame_buf_start;
        if (wl == 0) {
            ret = 1;
        } else {
            sem_post(sem_fshare_read_lock);
            usleep(1000);
        }
    }
    return;
}

void sem_write_unlock()
{
    sem_post(sem_fshare_read_lock);
    return;
}
#endif

void sigpipe_handler(int unused)
{
    // Do nothing
}

void* unlock_fifo_thread(void *data)
{
    int fd;
    char *fifo_name = data;
    unsigned char buffer_fifo[1024];

    fd = open(fifo_name, O_RDONLY);
    read(fd, buffer_fifo, 1024);
    close(fd);

    return NULL;
}

void print_usage(char *progname)
{
    fprintf(stderr, "\nUsage: %s [-m MODEL] [-r RES] [-s] [-f] [-d]\n\n", progname);
    fprintf(stderr, "\t-m MODEL, --model MODEL\n");
    fprintf(stderr, "\t\tset model: y20ga, y25ga or y30qa (default y20ga)\n");
    fprintf(stderr, "\t-r RES, --resolution RES\n");
    fprintf(stderr, "\t\tset resolution: LOW or HIGH (default HIGH)\n");
    fprintf(stderr, "\t-s, --sti\n");
    fprintf(stderr, "\t\tdon't overwrite SPS timing info (default overwrite)\n");
    fprintf(stderr, "\t-f, --fifo\n");
    fprintf(stderr, "\t\tenable fifo output\n");
    fprintf(stderr, "\t-d, --debug\n");
    fprintf(stderr, "\t\tenable debug\n");
}

int main(int argc, char **argv) {
    unsigned char *buf_idx, *buf_idx_cur, *buf_idx_end, *buf_idx_end_prev;
    unsigned char *buf_idx_start = NULL;
    FILE *fFS, *fOut, *fOutLow = NULL, *fOutHigh = NULL, *fOutAac = NULL;
    int fshm;
    mode_t mode = 0755;

    int frame_type = TYPE_NONE;
    int frame_len = 0;
    int frame_counter = -1;
    int frame_counter_last_valid_low = -1;
    int frame_counter_last_valid_high = -1;
    int frame_counter_last_valid_audio = -1;

    int i, n, c;
    int write_enable = 0;
    int frame_sync = 0;

    struct frame_header fhs[10];
    unsigned char* fhs_addr[10];
    uint32_t last_counter;

    resolution = RESOLUTION_HIGH;
    sps_timing_info = 1;
    fifo = 0;
    debug = 0;

    buf_offset = BUF_OFFSET_Y20GA;
    frame_header_size = FRAME_HEADER_SIZE_Y20GA;

    while (1) {
        static struct option long_options[] =
        {
            {"model",  required_argument, 0, 'm'},
            {"resolution",  required_argument, 0, 'r'},
            {"sti",  no_argument, 0, 's'},
            {"fifo",  no_argument, 0, 'f'},
            {"debug",  no_argument, 0, 'd'},
            {"help",  no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv, "m:r:fsdh",
                         long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c) {
        case 'm':
            if (strcasecmp("y20ga", optarg) == 0) {
                buf_offset = BUF_OFFSET_Y20GA;
                frame_header_size = FRAME_HEADER_SIZE_Y20GA;
            } else if (strcasecmp("y25ga", optarg) == 0) {
                buf_offset = BUF_OFFSET_Y25GA;
                frame_header_size = FRAME_HEADER_SIZE_Y25GA;
            } else if (strcasecmp("y30qa", optarg) == 0) {
                buf_offset = BUF_OFFSET_Y30QA;
                frame_header_size = FRAME_HEADER_SIZE_Y30QA;
            }
            break;

        case 'r':
            if (strcasecmp("low", optarg) == 0) {
                resolution = RESOLUTION_LOW;
            } else if (strcasecmp("high", optarg) == 0) {
                resolution = RESOLUTION_HIGH;
            } else if (strcasecmp("both", optarg) == 0) {
                resolution = RESOLUTION_BOTH;
            }
            break;

        case 's':
            sps_timing_info = 0;
            break;

        case 'f':
            fprintf (stderr, "using fifo as output\n");
            fifo = 1;
            break;

        case 'd':
            fprintf (stderr, "debug on\n");
            debug = 1;
            break;

        case 'h':
            print_usage(argv[0]);
            return -1;
            break;

        case '?':
            /* getopt_long already printed an error message. */
            break;

        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    if ((fifo == 0) && (resolution == RESOLUTION_BOTH)) {
        fprintf(stderr, "Both resolution are not supported with output to stdout\n");
        fprintf(stderr, "Use fifo or run two processes\n");
        return -1;
    }

    fFS = fopen(BUFFER_FILE, "r");
    if ( fFS == NULL ) {
        fprintf(stderr, "could not get size of %s\n", BUFFER_FILE);
        return -1;
    }
    fseek(fFS, 0, SEEK_END);
    buf_size = ftell(fFS);
    fclose(fFS);
    if (debug) fprintf(stderr, "the size of the buffer is %d\n", buf_size);

#ifdef USE_SEMAPHORE
    if (sem_fshare_open() != 0) {
        fprintf(stderr, "error - could not open semaphores\n") ;
        return -2;
    }
#endif

    // Opening an existing file
    fshm = shm_open(BUFFER_SHM, O_RDWR, 0);
    if ( fshm == -1 ) {
        fprintf(stderr, "error - could not open file %s\n", BUFFER_FILE) ;
        return -2;
    }

    // Map file to memory
    addr = (unsigned char*) mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fshm, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "error - mapping file %s\n", BUFFER_FILE);
        close(fshm);
        return -3;
    }
    if (debug) fprintf(stderr, "mapping file %s, size %d, to %08x\n", BUFFER_FILE, buf_size, (unsigned int) addr);

    // Closing the file
    if (debug) fprintf(stderr, "closing the file %s\n", BUFFER_FILE) ;
    close(fshm);

    // Opening/setting output file
    if (fifo == 0) {
        char stdoutbuf[262144];

        if (setvbuf(stdout, stdoutbuf, _IOFBF, sizeof(stdoutbuf)) != 0) {
            fprintf(stderr, "Error setting stdout buffer\n");
        }
        if (resolution == RESOLUTION_LOW) {
            fOutLow = stdout;
        } else if (resolution == RESOLUTION_HIGH) {
            fOutHigh = stdout;
        }
    } else {
        pthread_t unlock_low_thread, unlock_high_thread, unlock_aac_thread;

        sigaction(SIGPIPE, &(struct sigaction){{sigpipe_handler}}, NULL);

        if ((resolution == RESOLUTION_LOW) || (resolution == RESOLUTION_BOTH)) {
            unlink(FIFO_NAME_LOW);
            if (mkfifo(FIFO_NAME_LOW, mode) < 0) {
                fprintf(stderr, "mkfifo failed for file %s\n", FIFO_NAME_LOW);
                return -4;
            }
            if(pthread_create(&unlock_low_thread, NULL, unlock_fifo_thread, (void *) FIFO_NAME_LOW)) {
                fprintf(stderr, "Error creating thread\n");
                return -4;
            }
            pthread_detach(unlock_low_thread);
            fOutLow = fopen(FIFO_NAME_LOW, "w");
            if (fOutLow == NULL) {
                fprintf(stderr, "Error opening fifo %s\n", FIFO_NAME_LOW);
                return -4;
            }
        }
        if ((resolution == RESOLUTION_HIGH) || (resolution == RESOLUTION_BOTH)) {
            unlink(FIFO_NAME_HIGH);
            if (mkfifo(FIFO_NAME_HIGH, mode) < 0) {
                fprintf(stderr, "mkfifo failed for file %s\n", FIFO_NAME_HIGH);
                return -4;
            }
            if(pthread_create(&unlock_high_thread, NULL, unlock_fifo_thread, (void *) FIFO_NAME_HIGH)) {
                fprintf(stderr, "Error creating thread\n");
                return -4;
            }
            pthread_detach(unlock_high_thread);
            fOutHigh = fopen(FIFO_NAME_HIGH, "w");
            if (fOutHigh == NULL) {
                fprintf(stderr, "Error opening fifo %s\n", FIFO_NAME_HIGH);
                return -4;
            }
        }

        unlink(FIFO_NAME_AAC);
        if (mkfifo(FIFO_NAME_AAC, mode) < 0) {
            fprintf(stderr, "mkfifo failed for file %s\n", FIFO_NAME_AAC);
            return -4;
        }
        if(pthread_create(&unlock_aac_thread, NULL, unlock_fifo_thread, (void *) FIFO_NAME_AAC)) {
            fprintf(stderr, "Error creating thread\n");
            return -4;
        }
        pthread_detach(unlock_aac_thread);
        fOutAac = fopen(FIFO_NAME_AAC, "w");
        if (fOutAac == NULL) {
            fprintf(stderr, "Error opening fifo %s\n", FIFO_NAME_AAC);
            return -4;
        }

        fprintf(stderr, "fifo started\n");
    }

#ifdef USE_SEMAPHORE
    sem_write_lock();
#endif
    memcpy(&i, addr + 16, sizeof(i));
    buf_idx = addr + buf_offset + i;
    buf_idx_cur = buf_idx;
    memcpy(&i, addr + 4, sizeof(i));
    buf_idx_end = buf_idx + i;
    if (buf_idx_end >= addr + buf_size) buf_idx_end -= (buf_size - buf_offset);
    buf_idx_end_prev = buf_idx_end;
#ifdef USE_SEMAPHORE
    sem_write_unlock();
#endif
    last_counter = 0;

    if (debug) fprintf(stderr, "starting capture main loop\n");

    // Infinite loop
    while (1) {
#ifdef USE_SEMAPHORE
        sem_write_lock();
#endif
        memcpy(&i, addr + 16, sizeof(i));
        buf_idx = addr + buf_offset + i;
        memcpy(&i, addr + 4, sizeof(i));
        buf_idx_end = buf_idx + i;
        if (buf_idx_end >= addr + buf_size) buf_idx_end -= (buf_size - buf_offset);
        // Check if the header is ok
        memcpy(&i, addr + 12, sizeof(i));
        if (buf_idx_end != addr + buf_offset + i) {
            usleep(1000);
            continue;
        }

        if (buf_idx_end == buf_idx_end_prev) {
#ifdef USE_SEMAPHORE
            sem_write_unlock();
#endif
            usleep(10000);
            continue;
        }

        buf_idx_cur = buf_idx_end_prev;
        frame_sync = 1;
        i = 0;

        while (buf_idx_cur != buf_idx_end) {
            cb2s_headercpy((unsigned char *) &fhs[i], buf_idx_cur, frame_header_size);
            // Check the len
            if (fhs[i].len > buf_size - buf_offset - frame_header_size) {
                frame_sync = 0;
                break;
            }
            fhs_addr[i] = buf_idx_cur;
            buf_idx_cur = cb_move(buf_idx_cur, fhs[i].len + frame_header_size);
            i++;
            // Check if the sync is lost
            if (i == 10) {
                frame_sync = 0;
                break;
            }
        }

#ifdef USE_SEMAPHORE
        sem_write_unlock();
#endif

        if (frame_sync == 0) {
            buf_idx_end_prev = buf_idx_end;
            continue;
        }

        n = i;
        // Ignore last frame, it could be corrupted
        if (n > 1) {
            buf_idx_end_prev = fhs_addr[n - 1];
            n--;
        } else {
            continue;
        }

        n = i;
        if (n > 0) {
            if (fhs[0].counter != last_counter + 1) {
                fprintf(stderr, "%lld: warning - %d frame(s) lost\n",
                            current_timestamp(), fhs[0].counter - (last_counter + 1));
            }
            last_counter = fhs[n - 1].counter;
        }

        for (i = 0; i < n; i++) {
            buf_idx_cur = fhs_addr[i];
            frame_len = fhs[i].len;
            // If SPS
            if (fhs[i].type & 0x0002) {
                buf_idx_cur = cb_move(buf_idx_cur, frame_header_size + 6);
                frame_len -= 6;
            } else {
                buf_idx_cur = cb_move(buf_idx_cur, frame_header_size);
            }

            write_enable = 1;
            frame_counter = fhs[i].stream_counter;
            if (fhs[i].type & 0x0800) {
                frame_type = TYPE_LOW;
            } else if (fhs[i].type & 0x0400) {
                frame_type = TYPE_HIGH;
            } else if (fhs[i].type & 0x0100) {
                frame_type = TYPE_AAC;
            } else {
                frame_type = TYPE_NONE;
            }
            if ((frame_type == TYPE_LOW) && (resolution != RESOLUTION_HIGH)) {
                if ((65536 + frame_counter - frame_counter_last_valid_low) % 65536 > 1) {

                    if (debug) fprintf(stderr, "%lld: warning - %d low res frame(s) lost - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), (65536 + frame_counter - frame_counter_last_valid_low - 1) % 65536, frame_counter, frame_counter_last_valid_low);
                    frame_counter_last_valid_low = frame_counter;
                } else {
                    if (debug) {
                        if (fhs[i].type & 0x0002) {
                            fprintf(stderr, "%lld: SPS   detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                                    current_timestamp(), frame_len, frame_counter,
                                    frame_counter_last_valid_low, frame_type);
                        } else {
                            fprintf(stderr, "%lld: frame detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                                    current_timestamp(), frame_len, frame_counter,
                                    frame_counter_last_valid_low, frame_type);
                        }
                    }
                    frame_counter_last_valid_low = frame_counter;
                }

                buf_idx_start = buf_idx_cur;
            } else if ((frame_type == TYPE_HIGH) && (resolution != RESOLUTION_LOW)) {
                if ((65536 + frame_counter - frame_counter_last_valid_high) % 65536 > 1) {

                    if (debug) fprintf(stderr, "%lld: warning - %d high res frame(s) lost - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), (65536 + frame_counter - frame_counter_last_valid_high - 1) % 65536, frame_counter, frame_counter_last_valid_high);
                    frame_counter_last_valid_high = frame_counter;
                } else {
                    if (debug) {
                        if (fhs[i].type & 0x0002) {
                            fprintf(stderr, "%lld: SPS   detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                                    current_timestamp(), frame_len, frame_counter,
                                    frame_counter_last_valid_high, frame_type);
                        } else {
                            fprintf(stderr, "%lld: frame detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                                    current_timestamp(), frame_len, frame_counter,
                                    frame_counter_last_valid_high, frame_type);
                        }
                    }
                    frame_counter_last_valid_high = frame_counter;
                }

                buf_idx_start = buf_idx_cur;
            } else if (frame_type == TYPE_AAC) {
                if ((65536 + frame_counter - frame_counter_last_valid_audio) % 65536 > 1) {
                    if (debug) fprintf(stderr, "%lld: warning - %d AAC frame(s) lost - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), (65536 + frame_counter - frame_counter_last_valid_audio - 1) % 65536, frame_counter, frame_counter_last_valid_audio);
                    frame_counter_last_valid_audio = frame_counter;
                } else {
                    if (debug) fprintf(stderr, "%lld: frame detected - frame_len: %d - frame_counter: %d - audio AAC\n",
                                current_timestamp(), frame_len, fhs[i].stream_counter);

                    frame_counter_last_valid_audio = frame_counter;
                }
                buf_idx_start = buf_idx_cur;
            } else {
                write_enable = 0;
            }

            // Send the frame to the ouput buffer
            if (write_enable) {
                if ((frame_type == TYPE_LOW) && (resolution != RESOLUTION_HIGH)) {
                    fOut = fOutLow;
                } else if ((frame_type == TYPE_HIGH) && (resolution != RESOLUTION_LOW)) {
                    fOut = fOutHigh;
                } else if (frame_type == TYPE_AAC) {
                    fOut = fOutAac;
                } else {
                    fOut = NULL;
                }
                if (fOut != NULL) {
                    if (sps_timing_info) {
                        // Overwrite SPS with one that contains timing info at 20 fps
                        if (fhs[i].type & 0x0002) {
                            if (frame_type == TYPE_LOW) {
                                fwrite(SPS4_640X360_TI, 1, sizeof(SPS4_640X360_TI), fOut);
                            } else if (frame_type == TYPE_HIGH) {
                                fwrite(SPS4_1920X1080_TI, 1, sizeof(SPS4_1920X1080_TI), fOut);
                            }
                        } else {
                            if (buf_idx_start + frame_len > addr + buf_size) {
                                fwrite(buf_idx_start, 1, addr + buf_size - buf_idx_start, fOut);
                                fwrite(addr + buf_offset, 1, frame_len - (addr + buf_size - buf_idx_start), fOut);
                            } else {
                                fwrite(buf_idx_start, 1, frame_len, fOut);
                            }
                        }
                    } else {
                        if (buf_idx_start + frame_len > addr + buf_size) {
                            fwrite(buf_idx_start, 1, addr + buf_size - buf_idx_start, fOut);
                            fwrite(addr + buf_offset, 1, frame_len - (addr + buf_size - buf_idx_start), fOut);
                        } else {
                            fwrite(buf_idx_start, 1, frame_len, fOut);
                        }
                    }
                    if (debug) fprintf(stderr, "%lld: writing frame, length %d\n", current_timestamp(), frame_len);
                }
            }
        }

        usleep(25000);
    }

    // Unreacheable path

    // Unmap file from memory
    if (munmap(addr, buf_size) == -1) {
        if (debug) fprintf(stderr, "error - unmapping file");
    } else {
        if (debug) fprintf(stderr, "unmapping file %s, size %d, from %08x\n", BUFFER_FILE, buf_size, (unsigned int) addr);
    }

#ifdef USE_SEMAPHORE
    sem_fshare_close();
#endif

    if (fifo == 1) {
        if ((resolution == RESOLUTION_LOW) || (resolution == RESOLUTION_BOTH)) {
            fclose(fOutLow);
            unlink(FIFO_NAME_LOW);
        }
        if ((resolution == RESOLUTION_HIGH) || (resolution == RESOLUTION_BOTH)) {
            fclose(fOutHigh);
            unlink(FIFO_NAME_HIGH);
        }

        fclose(fOutAac);
        unlink(FIFO_NAME_AAC);
    }

    return 0;
}
