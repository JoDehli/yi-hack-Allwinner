/*
 * Copyright (c) 2021 roleo.
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
 * Dump h264 content from /dev/shm/fshare_frame_buffer to stdout
 */

#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <getopt.h>

#define BUF_OFFSET 300
#define BUF_SIZE 1786156
#define FRAME_HEADER_SIZE 22

#define MILLIS_25 25000

#define RESOLUTION_NONE 0
#define RESOLUTION_LOW  360
#define RESOLUTION_HIGH 1080

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"

unsigned char IDR[]               = {0x65, 0xB8};
unsigned char NAL_START[]         = {0x00, 0x00, 0x00, 0x01};
unsigned char IDR_START[]         = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
unsigned char PFR_START[]         = {0x00, 0x00, 0x00, 0x01, 0x41};
unsigned char SPS_START[]         = {0x00, 0x00, 0x00, 0x01, 0x67};
unsigned char PPS_START[]         = {0x00, 0x00, 0x00, 0x01, 0x68};
unsigned char SPS_640X360[]       = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                       0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                       0x01, 0x01, 0x02};
// As above but with timing info at 20 fps
unsigned char SPS_640X360_TI[]    = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                       0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                       0x01, 0x01, 0x40, 0x00, 0x00, 0x7D, 0x00, 0x00,
                                       0x13, 0x88, 0x21};
unsigned char SPS_1920X1080[]     = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                       0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                       0x04, 0x04, 0x04, 0x08};
// As above but with timing info at 20 fps
unsigned char SPS_1920X1080_TI[]  = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                       0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                       0x04, 0x04, 0x05, 0x00, 0x00, 0x03, 0x01, 0xF4,
                                       0x00, 0x00, 0x4E, 0x20, 0x84};

unsigned char *addr;                      /* Pointer to shared memory region (header) */
int resolution;
int sps_timing_info;
int debug;

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
        p = (unsigned char*) memmem(src, addr + BUF_SIZE - src, what, what_len);
        if (p == NULL) {
            // And from the start of the buffer size src_len
            p = (unsigned char*) memmem(addr + BUF_OFFSET, src + src_len - (addr + BUF_OFFSET), what, what_len);
        }
    }
    return p;
}

unsigned char *cb_move(unsigned char *buf, int offset)
{
    buf += offset;
    if ((offset > 0) && (buf > addr + BUF_SIZE))
        buf -= (BUF_SIZE - BUF_OFFSET);
    if ((offset < 0) && (buf < addr + BUF_OFFSET))
        buf += (BUF_SIZE - BUF_OFFSET);

    return buf;
}

// The second argument is the circular buffer
int cb_memcmp(unsigned char *str1, unsigned char *str2, size_t n)
{
    int ret;

    if (str2 + n > addr + BUF_SIZE) {
        ret = memcmp(str1, str2, addr + BUF_SIZE - str2);
        if (ret != 0) return ret;
        ret = memcmp(str1 + (addr + BUF_SIZE - str2), addr + BUF_OFFSET, n - (addr + BUF_SIZE - str2));
    } else {
        ret = memcmp(str1, str2, n);
    }

    return ret;
}

// The second argument is the circular buffer
void cb2s_memcpy(unsigned char *dest, unsigned char *src, size_t n)
{
    if (src + n > addr + BUF_SIZE) {
        memcpy(dest, src, addr + BUF_SIZE - src);
        memcpy(dest + (addr + BUF_SIZE - src), addr + BUF_OFFSET, n - (addr + BUF_SIZE - src));
    } else {
        memcpy(dest, src, n);
    }
}

void print_usage(char *progname)
{
    fprintf(stderr, "\nUsage: %s [-r RES] [-d]\n\n", progname);
    fprintf(stderr, "\t-r RES, --resolution RES\n");
    fprintf(stderr, "\t\tset resolution: LOW or HIGH (default HIGH)\n");
    fprintf(stderr, "\t-s, --sti\n");
    fprintf(stderr, "\t\tdon't overwrite SPS timing info (default overwrite)\n");
    fprintf(stderr, "\t-d, --debug\n");
    fprintf(stderr, "\t\tenable debug\n");
}

int main(int argc, char **argv) {
    unsigned char *buf_idx_1, *buf_idx_2;
    unsigned char *buf_idx_w, *buf_idx_tmp;
    unsigned char *buf_idx_start = NULL;
    unsigned char *sps_addr;
    int sps_len;
    int buf_idx_diff;
    FILE *fFid;

    int frame_res, frame_len, frame_counter = -1;
    int frame_counter_last_valid = -1;
    int frame_counter_invalid = 0;

    unsigned char frame_header[FRAME_HEADER_SIZE];

    int i, c;
    int write_enable = 0;
    int sps_sync = 0;

    resolution = RESOLUTION_HIGH;
    sps_timing_info = 1;
    debug = 0;

    while (1) {
        static struct option long_options[] =
        {
            {"resolution",  required_argument, 0, 'r'},
            {"debug",  no_argument, 0, 'd'},
            {"sti",  no_argument, 0, 's'},
            {"help",  no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv, "r:dsh",
                         long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c) {
        case 'r':
            if (strcasecmp("low", optarg) == 0) {
                resolution = RESOLUTION_LOW;
            } else if (strcasecmp("high", optarg) == 0) {
                resolution = RESOLUTION_HIGH;
            }
            break;

        case 's':
            sps_timing_info = 0;
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

    sps_addr = SPS_1920X1080;
    sps_len = sizeof(SPS_1920X1080);
    if (resolution == RESOLUTION_LOW) {
        sps_addr = SPS_640X360;
        sps_len = sizeof(SPS_640X360);
    } else if (resolution == RESOLUTION_HIGH) {
        sps_addr = SPS_1920X1080;
        sps_len = sizeof(SPS_1920X1080);
    }

    // Opening an existing file
    fFid = fopen(BUFFER_FILE, "r") ;
    if ( fFid == NULL ) {
        fprintf(stderr, "error - could not open file %s\n", BUFFER_FILE) ;
        return -1;
    }

    // Map file to memory
    addr = (unsigned char*) mmap(NULL, BUF_SIZE, PROT_READ, MAP_SHARED, fileno(fFid), 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "error - mapping file %s\n", BUFFER_FILE);
        fclose(fFid);
        return -2;
    }
    if (debug) fprintf(stderr, "mapping file %s, size %d, to %08x\n", BUFFER_FILE, BUF_SIZE, (unsigned int) addr);

    // Closing the file
    if (debug) fprintf(stderr, "closing the file %s\n", BUFFER_FILE) ;
    fclose(fFid) ;

    memcpy(&i, addr + 16, sizeof(i));
    buf_idx_w = addr + BUF_OFFSET + i;
    buf_idx_1 = buf_idx_w;

    if (debug) fprintf(stderr, "starting capture main loop\n");

    // Infinite loop
    while (1) {
        memcpy(&i, addr + 16, sizeof(i));
        buf_idx_w = addr + BUF_OFFSET + i;
//        if (debug) fprintf(stderr, "buf_idx_w: %08x\n", (unsigned int) buf_idx_w);
        buf_idx_tmp = cb_memmem(buf_idx_1, buf_idx_w - buf_idx_1, NAL_START, sizeof(NAL_START));
        if (buf_idx_tmp == NULL) {
            usleep(MILLIS_25);
            continue;
        } else {
            buf_idx_1 = buf_idx_tmp;
        }
//        if (debug) fprintf(stderr, "found buf_idx_1: %08x\n", (unsigned int) buf_idx_1);

        buf_idx_tmp = cb_memmem(buf_idx_1 + 1, buf_idx_w - (buf_idx_1 + 1), NAL_START, sizeof(NAL_START));
        if (buf_idx_tmp == NULL) {
            usleep(MILLIS_25);
            continue;
        } else {
            buf_idx_2 = buf_idx_tmp;
        }
//        if (debug) fprintf(stderr, "found buf_idx_2: %08x\n", (unsigned int) buf_idx_2);

        if ((write_enable) && (sps_sync)) {
            if (sps_timing_info) {
                if (cb_memcmp(SPS_640X360, buf_idx_start, sizeof(SPS_640X360)) == 0) {
                    fwrite(SPS_640X360_TI, 1, sizeof(SPS_640X360_TI), stdout);
                } else if (cb_memcmp(SPS_1920X1080, buf_idx_start, sizeof(SPS_1920X1080)) == 0) {
                    fwrite(SPS_1920X1080_TI, 1, sizeof(SPS_1920X1080_TI), stdout);
                }
            } else {
                if (buf_idx_start + frame_len > addr + BUF_SIZE) {
                    fwrite(buf_idx_start, 1, addr + BUF_SIZE - buf_idx_start, stdout);
                    fwrite(addr + BUF_OFFSET, 1, frame_len - (addr + BUF_SIZE - buf_idx_start), stdout);
                } else {
                    fwrite(buf_idx_start, 1, frame_len, stdout);
                }
            }
        }

        if (cb_memcmp(sps_addr, buf_idx_1, sps_len) == 0) {
            // SPS frame
            write_enable = 1;
            sps_sync = 1;
            buf_idx_1 = cb_move(buf_idx_1, - (6 + FRAME_HEADER_SIZE));
            cb2s_memcpy(frame_header, buf_idx_1, FRAME_HEADER_SIZE);
            buf_idx_1 = cb_move(buf_idx_1, 6 + FRAME_HEADER_SIZE);
            if (frame_header[17] == 8) {
                frame_res = RESOLUTION_LOW;
            } else if (frame_header[17] == 4) {
                frame_res = RESOLUTION_HIGH;
            } else {
                frame_res = RESOLUTION_NONE;
            }
            if (frame_res == resolution) {
                memcpy((unsigned char *) &frame_len, frame_header, 4);
                frame_len -= 6;                                                              // -6 only for SPS
                // Check if buf_idx_2 is greater than buf_idx_1 + frame_len
                buf_idx_diff = buf_idx_2 - buf_idx_1;
                if (buf_idx_diff < 0) buf_idx_diff += (BUF_SIZE - BUF_OFFSET);
                if (buf_idx_diff > frame_len) {
                    frame_counter = (int) frame_header[18] + (int) frame_header[19] * 256;
                    if ((frame_counter - frame_counter_last_valid > 20) ||
                                ((frame_counter < frame_counter_last_valid) && (frame_counter - frame_counter_last_valid > -65515))) {

                        if (debug) fprintf(stderr, "%lld: warning - incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                                    current_timestamp(), frame_counter, frame_counter_last_valid);
                        frame_counter_invalid++;
                        // Check if sync is lost
                        if (frame_counter_invalid > 40) {
                            if (debug) fprintf(stderr, "%lld: error - sync lost\n", current_timestamp());
                            frame_counter_last_valid = frame_counter;
                            frame_counter_invalid = 0;
                        } else {
                            write_enable = 0;
                        }
                    } else {
                        frame_counter_invalid = 0;
                        frame_counter_last_valid = frame_counter;
                    }
                } else {
                    write_enable = 0;
                }
                if (debug) fprintf(stderr, "%lld: SPS   detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                            current_timestamp(), frame_len, frame_counter,
                            frame_counter_last_valid, frame_res);

                buf_idx_start = buf_idx_1;
            } else {
                write_enable = 0;
                if (debug & 1) fprintf(stderr, "%lld: warning - unexpected NALU header\n", current_timestamp());
            }
        } else if ((cb_memcmp(PPS_START, buf_idx_1, sizeof(PPS_START)) == 0) ||
                    (cb_memcmp(IDR_START, buf_idx_1, sizeof(IDR_START)) == 0) ||
                    (cb_memcmp(PFR_START, buf_idx_1, sizeof(PFR_START)) == 0)) {
            // PPS, IDR and PFR frames
            write_enable = 1;
            buf_idx_1 = cb_move(buf_idx_1, -FRAME_HEADER_SIZE);
            cb2s_memcpy(frame_header, buf_idx_1, FRAME_HEADER_SIZE);
            buf_idx_1 = cb_move(buf_idx_1, FRAME_HEADER_SIZE);
            if (frame_header[17] == 8) {
                frame_res = RESOLUTION_LOW;
            } else if (frame_header[17] == 4) {
                frame_res = RESOLUTION_HIGH;
            } else {
                frame_res = RESOLUTION_NONE;
            }
            if (frame_res == resolution) {
                memcpy((unsigned char *) &frame_len, frame_header, 4);
                // Check if buf_idx_2 is greater than buf_idx_1 + frame_len
                buf_idx_diff = buf_idx_2 - buf_idx_1;
                if (buf_idx_diff < 0) buf_idx_diff += (BUF_SIZE - BUF_OFFSET);
                if (buf_idx_diff > frame_len) {
                    frame_counter = (int) frame_header[18] + (int) frame_header[19] * 256;
                    if ((frame_counter - frame_counter_last_valid > 20) ||
                                ((frame_counter < frame_counter_last_valid) && (frame_counter - frame_counter_last_valid > -65515))) {

                        if (debug) fprintf(stderr, "%lld: warning - incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                                    current_timestamp(), frame_counter, frame_counter_last_valid);
                        frame_counter_invalid++;
                        // Check if sync is lost
                        if (frame_counter_invalid > 40) {
                            if (debug) fprintf(stderr, "%lld: error - sync lost\n", current_timestamp());
                            frame_counter_last_valid = frame_counter;
                            frame_counter_invalid = 0;
                        } else {
                            write_enable = 0;
                        }
                    } else {
                        frame_counter_invalid = 0;
                        frame_counter_last_valid = frame_counter;
                    }
                } else {
                    write_enable = 0;
                }
                if (debug) fprintf(stderr, "%lld: frame detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                            current_timestamp(), frame_len, frame_counter,
                            frame_counter_last_valid, frame_res);

                buf_idx_start = buf_idx_1;
            } else {
                write_enable = 0;
                if (debug & 1) fprintf(stderr, "%lld: warning - unexpected NALU header\n", current_timestamp());
            }
        } else {
            write_enable = 0;
        }

        buf_idx_1 = buf_idx_2;
    }

    // Unreacheable path

    // Unmap file from memory
    if (munmap(addr, BUF_SIZE) == -1) {
        if (debug) fprintf(stderr, "error - unmapping file");
    } else {
        if (debug) fprintf(stderr, "unmapping file %s, size %d, from %08x\n", BUFFER_FILE, BUF_SIZE, (unsigned int) addr);
    }

    return 0;
}
