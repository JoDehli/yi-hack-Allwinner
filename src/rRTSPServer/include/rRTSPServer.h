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
 * Dump h264 content from /dev/shm/fshare_frame_buffer and copy it to
 * a circular buffer.
 * Then send the circular buffer to live555.
 */

#ifndef _R_RTSP_SERVER_H
#define _R_RTSP_SERVER_H

//#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <getopt.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define BUFFER_SHM "fshare_frame_buf"
#define READ_LOCK_FILE "fshare_read_lock"
#define WRITE_LOCK_FILE "fshare_write_lock"

#define Y20GA 0
#define Y25GA 1
#define Y30QA 2

#define BUF_OFFSET_Y20GA 300
#define FRAME_HEADER_SIZE_Y20GA 22

#define BUF_OFFSET_Y25GA 300
#define FRAME_HEADER_SIZE_Y25GA 22

#define BUF_OFFSET_Y30QA 300
#define FRAME_HEADER_SIZE_Y30QA 22

#define MILLIS_10 10000
#define MILLIS_25 25000

#define RESOLUTION_NONE 0
#define RESOLUTION_LOW  360
#define RESOLUTION_HIGH 1080
#define RESOLUTION_BOTH 1440

#define TYPE_NONE 0
#define TYPE_LOW  360
#define TYPE_HIGH 1080
#define TYPE_AAC 65521

#define OUTPUT_BUFFER_SIZE_LOW  49152
#define OUTPUT_BUFFER_SIZE_HIGH 262144
#define OUTPUT_BUFFER_SIZE_AUDIO 12288

typedef struct
{
    unsigned char *buffer;                  // pointer to the base of the input buffer
    char filename[256];                     // name of the buffer file
    unsigned int size;                      // size of the buffer file
    unsigned int offset;                    // offset where stream starts
    unsigned char *read_index;              // read absolute index
} cb_input_buffer;

// Frame position inside the output buffer, needed to use DiscreteFramer instead Framer.
typedef struct
{
    unsigned char *ptr;                     // pointer to the frame start
    unsigned int counter;                   // frame counter
    unsigned int size;                      // frame size
} cb_output_frame;

typedef struct
{
    unsigned char *buffer;                  // pointer to the base of the output buffer
    unsigned int size;                      // size of the output buffer
    int type;                               // type of the stream in this buffer
    unsigned char *write_index;             // write absolute index
    cb_output_frame output_frame[42];       // array of frames that buffer contains 42 = SPS + PPS + iframe + GOP
    int output_frame_size;                  // number of frames that buffer contains
    unsigned int frame_read_index;          // index of the next frame to read
    unsigned int frame_write_index;         // index of the next frame to write
    pthread_mutex_t mutex;                  // mutex of the structure
} cb_output_buffer;

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

#endif
