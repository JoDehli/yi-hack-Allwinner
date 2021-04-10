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
 * Dump h264 content from /dev/shm/fshare_frame_buffer and copy it to
 * a circular buffer.
 * Then send the circular buffer to live555.
 */

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include "H264VideoFramedMemoryServerMediaSubsession.hh"
#include "WAVAudioFifoServerMediaSubsession.hh"
#include "WAVAudioFifoSource.hh"
#include "ADTSFromWAVAudioFifoServerMediaSubsession.hh"
#include "ADTSFromWAVAudioFifoSource.hh"
#include "StreamReplicator.hh"
#include "DummySink.hh"
#include "aLawAudioFilter.hh"

#include <getopt.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "rRTSPServer.h"

unsigned char IDR[]               = {0x65, 0xB8};
unsigned char NAL_START[]         = {0x00, 0x00, 0x00, 0x01};
unsigned char IDR_START[]         = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
unsigned char PFR_START[]         = {0x00, 0x00, 0x00, 0x01, 0x41};
unsigned char SPS_START[]         = {0x00, 0x00, 0x00, 0x01, 0x67};
unsigned char PPS_START[]         = {0x00, 0x00, 0x00, 0x01, 0x68};
unsigned char SPS_COMMON[]        = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00};
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

//unsigned char *addr;                      /* Pointer to shared memory region (header) */
int debug;                                  /* Set to 1 to debug this .c */
int resolution;
int audio;
int port;
int sps_timing_info;
//unsigned char *buf_start;

//unsigned char *output_buffer = NULL;
//u_int64_t output_buffer_size = 0;
cb_input_buffer input_buffer;
cb_output_buffer output_buffer_low;
cb_output_buffer output_buffer_high;

UsageEnvironment* env;

// To make the second and subsequent client for each stream reuse the same
// input stream as the first client (rather than playing the file from the
// start for each client), change the following "False" to "True":
Boolean reuseFirstSource = True;

// To stream *only* MPEG-1 or 2 video "I" frames
// (e.g., to reduce network bandwidth),
// change the following "False" to "True":
Boolean iFramesOnly = False;

long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // calculate milliseconds

    return milliseconds;
}

void s2cb_memcpy(cb_output_buffer *dest, unsigned char *src, size_t n)
{
    unsigned char *uc_dest = dest->write_index;

    if (uc_dest + n > dest->buffer + dest->size) {
        memcpy(uc_dest, src, dest->buffer + dest->size - uc_dest);
        memcpy(dest->buffer, src + (dest->buffer + dest->size - uc_dest), n - (dest->buffer + dest->size - uc_dest));
        dest->write_index = n + uc_dest - dest->size;
    } else {
        memcpy(uc_dest, src, n);
        dest->write_index += n;
    }
    if (dest->write_index == dest->buffer + dest->size) {
        dest->write_index = dest->buffer;
    }
}

void cb2cb_memcpy(cb_output_buffer *dest, cb_input_buffer *src, size_t n)
{
    unsigned char *uc_src = src->read_index;

    if (uc_src + n > src->buffer + src->size) {
        s2cb_memcpy(dest, uc_src, src->buffer + src->size - uc_src);
        s2cb_memcpy(dest, src->buffer + src->offset, n - (src->buffer + src->size - uc_src));
        src->read_index = src->offset + n + uc_src - src->size;
    } else {
        s2cb_memcpy(dest, uc_src, n);
        src->read_index += n;
    }
}

// The second argument is the circular buffer
int cb_memcmp(unsigned char *str1, unsigned char*str2, size_t n)
{
    int ret;

    if (str2 + n > input_buffer.buffer + input_buffer.size) {
        ret = memcmp(str1, str2, input_buffer.buffer + input_buffer.size - str2);
        if (ret != 0) return ret;
        ret = memcmp(str1 + (input_buffer.buffer + input_buffer.size - str2), input_buffer.buffer + input_buffer.offset, n - (input_buffer.buffer + input_buffer.size - str2));
    } else {
        ret = memcmp(str1, str2, n);
    }

    return ret;
}

/* Locate a string in the circular buffer */
unsigned char *cb_memmem(unsigned char *src, int src_len, unsigned char *what, int what_len)
{
    unsigned char *p;

    if (src_len >= 0) {
        p = (unsigned char*) memmem(src, src_len, what, what_len);
    } else {
        // From src to the end of the buffer
        p = (unsigned char*) memmem(src, input_buffer.buffer + input_buffer.size - src, what, what_len);
        if (p == NULL) {
            // And from the start of the buffer size src_len
            p = (unsigned char*) memmem(input_buffer.buffer + input_buffer.offset, src + src_len - (input_buffer.buffer + input_buffer.offset), what, what_len);
        }
    }
    return p;
}

unsigned char *cb_move(unsigned char *buf, int offset)
{
    buf += offset;
    if ((offset > 0) && (buf > input_buffer.buffer + input_buffer.size))
        buf -= (input_buffer.size - input_buffer.offset);
    if ((offset < 0) && (buf < input_buffer.buffer + input_buffer.offset))
        buf += (input_buffer.size - input_buffer.offset);

    return buf;
}

// The second argument is the circular buffer
void cb2s_memcpy(unsigned char *dest, unsigned char *src, size_t n)
{
    if (src + n > input_buffer.buffer + input_buffer.size) {
        memcpy(dest, src, input_buffer.buffer + input_buffer.size - src);
        memcpy(dest + (input_buffer.buffer + input_buffer.size - src), input_buffer.buffer + input_buffer.offset, n - (input_buffer.buffer + input_buffer.size - src));
    } else {
        memcpy(dest, src, n);
    }
}

void *capture(void *ptr)
{
    unsigned char *buf_idx_1, *buf_idx_2;
    unsigned char *buf_idx_w, *buf_idx_tmp;
    unsigned char *buf_idx_start = NULL;
    int buf_idx_diff;
    FILE *fFid;

    int frame_len = -1;
    int frame_res = -1;
    int frame_counter = -1;
    int frame_counter_last_valid_low = -1;
    int frame_counter_last_valid_high = -1;
    int frame_counter_invalid_low = 0;
    int frame_counter_invalid_high = 0;

    unsigned char frame_header[FRAME_HEADER_SIZE];

    int i;
    cb_output_buffer *cb_current;
    int write_enable = 0;
    int sps_sync = 0;
    int nal_is_sps = 0;

    // Opening an existing file
    fFid = fopen(input_buffer.filename, "r");
    if ( fFid == NULL ) {
        fprintf(stderr, "%lld: error - could not open file %s\n", current_timestamp(), input_buffer.filename);
        free(output_buffer_low.buffer);
        free(output_buffer_high.buffer);
        exit(EXIT_FAILURE);
    }

    // Map file to memory
    input_buffer.buffer = (unsigned char*) mmap(NULL, input_buffer.size, PROT_READ, MAP_SHARED, fileno(fFid), 0);
    if (input_buffer.buffer == MAP_FAILED) {
        fprintf(stderr, "%lld: error - mapping file %s\n", current_timestamp(), input_buffer.filename);
        fclose(fFid);
        free(output_buffer_low.buffer);
        free(output_buffer_high.buffer);
        exit(EXIT_FAILURE);
    }
    if (debug & 1) fprintf(stderr, "%lld: mapping file %s, size %d, to %08x\n", current_timestamp(), input_buffer.filename, input_buffer.size, (unsigned int) input_buffer.buffer);

    // Closing the file
    if (debug & 1) fprintf(stderr, "%lld: closing the file %s\n", current_timestamp(), input_buffer.filename);
    fclose(fFid) ;

    memcpy(&i, input_buffer.buffer + 16, sizeof(i));
    buf_idx_w = input_buffer.buffer + input_buffer.offset + i;
    buf_idx_1 = buf_idx_w;

    if (debug & 1) fprintf(stderr, "%lld: starting capture main loop\n", current_timestamp());

    // Infinite loop
    while (1) {
        memcpy(&i, input_buffer.buffer + 16, sizeof(i));
        buf_idx_w = input_buffer.buffer + input_buffer.offset + i;
//        if (debug & 1) fprintf(stderr, "buf_idx_w: %08x\n", (unsigned int) buf_idx_w);
        buf_idx_tmp = cb_memmem(buf_idx_1, buf_idx_w - buf_idx_1, NAL_START, sizeof(NAL_START));
        if (buf_idx_tmp == NULL) {
            usleep(MILLIS_25);
            continue;
        } else {
            buf_idx_1 = buf_idx_tmp;
        }
//        if (debug & 1) fprintf(stderr, "found buf_idx_1: %08x\n", (unsigned int) buf_idx_1);

        buf_idx_tmp = cb_memmem(buf_idx_1 + 1, buf_idx_w - (buf_idx_1 + 1), NAL_START, sizeof(NAL_START));
        if (buf_idx_tmp == NULL) {
            usleep(MILLIS_25);
            continue;
        } else {
            buf_idx_2 = buf_idx_tmp;
        }
//        if (debug & 1) fprintf(stderr, "found buf_idx_2: %08x\n", (unsigned int) buf_idx_2);

        if ((write_enable) && (sps_sync)) {
            if (frame_res == RESOLUTION_LOW) {
                cb_current = &output_buffer_low;
            } else if (frame_res == RESOLUTION_HIGH) {
                cb_current = &output_buffer_high;
            } else {
                cb_current = NULL;
            }

            if (cb_current != NULL) {
                if (debug & 1) fprintf(stderr, "%lld: frame_len: %d - cb_current->size: %d\n", current_timestamp(), frame_len, cb_current->size);
                if (frame_len > (signed) cb_current->size) {
                    fprintf(stderr, "%lld: error - frame size exceeds buffer size\n", current_timestamp());
                    sps_sync = 0;
                } else {
                    pthread_mutex_lock(&(cb_current->mutex));
                    input_buffer.read_index = buf_idx_start;

                    if (sps_timing_info) {
                        // Check if NALU is SPS
                        if (nal_is_sps == 1) {
                            if (frame_res == RESOLUTION_LOW) {
                                frame_len = sizeof(SPS_640X360_TI);
                            } else if (frame_res == RESOLUTION_HIGH) {
                                frame_len = sizeof(SPS_1920X1080_TI);
                            }
                        }
                    }
                    cb_current->output_frame[cb_current->frame_write_index].ptr = cb_current->write_index;
                    cb_current->output_frame[cb_current->frame_write_index].counter = frame_counter;
                    cb_current->output_frame[cb_current->frame_write_index].size = frame_len;
                    if (debug & 1) fprintf(stderr, "%lld: frame_len: %d - frame_counter: %d - resolution: %d\n", current_timestamp(), frame_len, frame_counter, frame_res);
                    if (debug & 1) fprintf(stderr, "%lld: frame_write_index: %d/%d\n", current_timestamp(), cb_current->frame_write_index, cb_current->output_frame_size);

                    if (sps_timing_info) {
                        // Overwrite SPS with one that contains timing info at 20 fps
                        if (nal_is_sps == 1) {
                            if (frame_res == RESOLUTION_LOW) {
                                s2cb_memcpy(cb_current, SPS_640X360_TI, sizeof(SPS_640X360_TI));
                            } else if (frame_res == RESOLUTION_HIGH) {
                                s2cb_memcpy(cb_current, SPS_1920X1080_TI, sizeof(SPS_1920X1080_TI));
                            }
                        } else {
                            cb2cb_memcpy(cb_current, &input_buffer, frame_len);
                        }
                    } else {
                        cb2cb_memcpy(cb_current, &input_buffer, frame_len);
                    }

                    cb_current->frame_write_index = (cb_current->frame_write_index + 1) % cb_current->output_frame_size;
                    pthread_mutex_unlock(&(cb_current->mutex));
                }
            }
        }

        nal_is_sps = 0;
        if (cb_memcmp(SPS_COMMON, buf_idx_1, sizeof(SPS_COMMON)) == 0) {
            // SPS frame
            nal_is_sps = 1;
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
            memcpy((unsigned char *) &frame_len, frame_header, 4);
            frame_len -= 6;                                                              // -6 only for SPS
            // Check if buf_idx_2 is greater than buf_idx_1 + frame_len
            buf_idx_diff = buf_idx_2 - buf_idx_1;
            if (buf_idx_diff < 0) buf_idx_diff += (input_buffer.size - input_buffer.offset);
            if (buf_idx_diff > frame_len) {
                frame_counter = (int) frame_header[18] + (int) frame_header[19] * 256;
                if ((frame_res == RESOLUTION_LOW) && ((frame_counter - frame_counter_last_valid_low > 20) ||
                            ((frame_counter < frame_counter_last_valid_low) && (frame_counter - frame_counter_last_valid_low > -65515)))) {

                    if (debug & 1) fprintf(stderr, "%lld: warning - incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), frame_counter, frame_counter_last_valid_low);
                    frame_counter_invalid_low++;
                    // Check if sync is lost
                    if (frame_counter_invalid_low > 40) {
                        if (debug & 1) fprintf(stderr, "%lld: error - sync lost\n", current_timestamp());
                        frame_counter_last_valid_low = frame_counter;
                        frame_counter_invalid_low = 0;
                    } else {
                        write_enable = 0;
                    }
                } else if ((frame_res == RESOLUTION_HIGH) && ((frame_counter - frame_counter_last_valid_high > 20) ||
                            ((frame_counter < frame_counter_last_valid_high) && (frame_counter - frame_counter_last_valid_high > -65515)))) {

                    if (debug & 1) fprintf(stderr, "%lld: warning - incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), frame_counter, frame_counter_last_valid_high);
                    frame_counter_invalid_high++;
                    // Check if sync is lost
                    if (frame_counter_invalid_high > 40) {
                        if (debug & 1) fprintf(stderr, "%lld: error - sync lost\n", current_timestamp());
                        frame_counter_last_valid_high = frame_counter;
                        frame_counter_invalid_high = 0;
                    } else {
                        write_enable = 0;
                    }
                } else {
                    if (frame_res == RESOLUTION_LOW) {
                        frame_counter_invalid_low = 0;
                        frame_counter_last_valid_low = frame_counter;
                    } else if (frame_res == RESOLUTION_HIGH) {
                        frame_counter_invalid_high = 0;
                        frame_counter_last_valid_high = frame_counter;
                    } else {
                        write_enable = 0;
                    }
                }
                if (debug & 1) fprintf(stderr, "%lld: SPS   detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                        current_timestamp(), frame_len, frame_counter,
                        (frame_res == RESOLUTION_LOW)? frame_counter_last_valid_low: frame_counter_last_valid_high, frame_res);

                buf_idx_start = buf_idx_1;
            } else {
                nal_is_sps = 0;
                write_enable = 0;
                if (debug & 1) fprintf(stderr, "%lld: warning - unexpected NALU header\n", current_timestamp());
            }
        } else if ((cb_memcmp(PPS_START, buf_idx_1, sizeof(PPS_START)) == 0) ||
                    (cb_memcmp(IDR_START, buf_idx_1, sizeof(IDR_START)) == 0) ||
                    (cb_memcmp(PFR_START, buf_idx_1, sizeof(PFR_START)) == 0)) {
            // PPS, IDR and PFR frames
            nal_is_sps = 0;
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
            memcpy((unsigned char *) &frame_len, frame_header, 4);
            // Check if buf_idx_2 is greater than buf_idx_1 + frame_len
            buf_idx_diff = buf_idx_2 - buf_idx_1;
            if (buf_idx_diff < 0) buf_idx_diff += (input_buffer.size - input_buffer.offset);
            if (buf_idx_diff > frame_len) {
                frame_counter = (int) frame_header[18] + (int) frame_header[19] * 256;
                if ((frame_res == RESOLUTION_LOW) && ((frame_counter - frame_counter_last_valid_low > 20) ||
                            ((frame_counter < frame_counter_last_valid_low) && (frame_counter - frame_counter_last_valid_low > -65515)))) {

                    if (debug & 1) fprintf(stderr, "%lld: warning - incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), frame_counter, frame_counter_last_valid_low);
                    frame_counter_invalid_low++;
                    // Check if sync is lost
                    if (frame_counter_invalid_low > 40) {
                        if (debug & 1) fprintf(stderr, "%lld: error - sync lost\n", current_timestamp());
                        frame_counter_last_valid_low = frame_counter;
                        frame_counter_invalid_low = 0;
                    } else {
                        write_enable = 0;
                    }
                } else if ((frame_res == RESOLUTION_HIGH) && ((frame_counter - frame_counter_last_valid_high > 20) ||
                            ((frame_counter < frame_counter_last_valid_high) && (frame_counter - frame_counter_last_valid_high > -65515)))) {

                    if (debug & 1) fprintf(stderr, "%lld: warning - incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                                current_timestamp(), frame_counter, frame_counter_last_valid_high);
                    frame_counter_invalid_high++;
                    // Check if sync is lost
                    if (frame_counter_invalid_high > 40) {
                        if (debug & 1) fprintf(stderr, "%lld: error - sync lost\n", current_timestamp());
                        frame_counter_last_valid_high = frame_counter;
                        frame_counter_invalid_high = 0;
                    } else {
                        write_enable = 0;
                    }
                } else {
                    if (frame_res == RESOLUTION_LOW) {
                        frame_counter_invalid_low = 0;
                        frame_counter_last_valid_low = frame_counter;
                    } else if (frame_res == RESOLUTION_HIGH) {
                        frame_counter_invalid_high = 0;
                        frame_counter_last_valid_high = frame_counter;
                    } else {
                        write_enable = 0;
                    }
                }
                if (debug & 1) fprintf(stderr, "%lld: frame detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                        current_timestamp(), frame_len, frame_counter,
                        (frame_res == RESOLUTION_LOW)? frame_counter_last_valid_low: frame_counter_last_valid_high, frame_res);

                buf_idx_start = buf_idx_1;
            } else {
                nal_is_sps = 0;
                write_enable = 0;
                if (debug & 1) fprintf(stderr, "%lld: warning - unexpected NALU header\n", current_timestamp());
            }
        } else {
            nal_is_sps = 0;
            write_enable = 0;
        }

        buf_idx_1 = buf_idx_2;
    }

    // Unreacheable path

    // Unmap file from memory
    if (munmap(input_buffer.buffer, input_buffer.size) == -1) {
        fprintf(stderr, "%lld: error - unmapping file\n", current_timestamp());
    } else {
        if (debug & 1) fprintf(stderr, "%lld: unmapping file %s, size %d, from %08x\n", current_timestamp(), BUFFER_FILE, input_buffer.size, (unsigned int) input_buffer.buffer);
    }

    return NULL;
}

StreamReplicator* startReplicatorStream(const char* inputAudioFileName, int convertToxLaw) {
    // Create a single WAVAudioFifo source that will be replicated for mutliple streams
    WAVAudioFifoSource* wavSource = WAVAudioFifoSource::createNew(*env, inputAudioFileName);
    if (wavSource == NULL) {
        *env << "Failed to create Fifo Source \n";
    }

    // Optionally convert to uLaw or aLaw pcm
    FramedSource* resultSource;
    if (convertToxLaw == WA_PCMA) {
        resultSource = aLawFromPCMAudioSource::createNew(*env, wavSource, 1/*little-endian*/);
    } else if (convertToxLaw == WA_PCMU) {
        resultSource = uLawFromPCMAudioSource::createNew(*env, wavSource, 1/*little-endian*/);
    } else {
        resultSource = EndianSwap16::createNew(*env, wavSource);
    }

    // Create and start the replicator that will be given to each subsession
    StreamReplicator* replicator = StreamReplicator::createNew(*env, resultSource);

    // Begin by creating an input stream from our replicator:
    FramedSource* source = replicator->createStreamReplica();

    // Then create a 'dummy sink' object to receive the replica stream:
    MediaSink* sink = DummySink::createNew(*env, "dummy");

    // Now, start playing, feeding the sink object from the source:
    sink->startPlaying(*source, NULL, NULL);

    return replicator;
}

StreamReplicator* startReplicatorStream(const char* inputAudioFileName) {
    // Create a single ADTSFromWAVAudioFifo source that will be replicated for mutliple streams
    ADTSFromWAVAudioFifoSource* adtsSource = ADTSFromWAVAudioFifoSource::createNew(*env, inputAudioFileName);
    if (adtsSource == NULL) {
        *env << "Failed to create Fifo Source \n";
    }

    // Create and start the replicator that will be given to each subsession
    StreamReplicator* replicator = StreamReplicator::createNew(*env, adtsSource);

    // Begin by creating an input stream from our replicator:
    FramedSource* source = replicator->createStreamReplica();

    // Then create a 'dummy sink' object to receive the replica stream:
    MediaSink* sink = DummySink::createNew(*env, "dummy");

    // Now, start playing, feeding the sink object from the source:
    sink->startPlaying(*source, NULL, NULL);

    return replicator;
}

static void announceStream(RTSPServer* rtspServer, ServerMediaSession* sms, char const* streamName, int audio)
{
    if (debug == 0) {
        char* url = rtspServer->rtspURL(sms);
        UsageEnvironment& env = rtspServer->envir();
        env << "\n\"" << streamName << "\" stream, from memory\n";
        if (audio == 1)
            env << "PCM audio enabled\n";
        else if (audio == 2)
            env << "AAC audio enabled\n";
        env << "Play this stream using the URL \"" << url << "\"\n";
        delete[] url;
    }
}

void print_usage(char *progname)
{
    fprintf(stderr, "\nUsage: %s [-r RES] [-p PORT] [-d]\n\n", progname);
    fprintf(stderr, "\t-r RES,   --resolution RES\n");
    fprintf(stderr, "\t\tset resolution: low, high or both (default high)\n");
    fprintf(stderr, "\t-a AUDIO, --audio AUDIO\n");
    fprintf(stderr, "\t\tset audio: yes, no, alaw, ulaw, pcm or aac (default yes)\n");
    fprintf(stderr, "\t-p PORT,  --port PORT\n");
    fprintf(stderr, "\t\tset TCP port (default 554)\n");
    fprintf(stderr, "\t-s,       --sti\n");
    fprintf(stderr, "\t\tdon't overwrite SPS timing info (default overwrite)\n");
    fprintf(stderr, "\t-d DEBUG, --debug DEBUG\n");
    fprintf(stderr, "\t\t0 none, 1 grabber, 2 rtsp library or 3 both\n");
    fprintf(stderr, "\t-h,       --help\n");
    fprintf(stderr, "\t\tprint this help\n");
}

int main(int argc, char** argv)
{
    char *str;
    int nm;
    char user[65];
    char pwd[65];
    int pth_ret;
    int c;
    char *endptr;

    pthread_t capture_thread;

    int convertToxLaw = WA_PCMU;
    char const* inputAudioFileName = "/tmp/audio_fifo";
    struct stat stat_buffer;

    // Setting default
    resolution = RESOLUTION_HIGH;
    audio = 1;
    port = 554;
    sps_timing_info = 1;
    debug = 0;

    while (1) {
        static struct option long_options[] =
        {
            {"resolution",  required_argument, 0, 'r'},
            {"audio",  required_argument, 0, 'a'},
            {"port",  required_argument, 0, 'p'},
            {"sti",  no_argument, 0, 's'},
            {"debug",  required_argument, 0, 'd'},
            {"help",  no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv, "r:a:p:sd:h",
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
            } else if (strcasecmp("both", optarg) == 0) {
                resolution = RESOLUTION_BOTH;
            }
            break;

        case 'a':
            if (strcasecmp("no", optarg) == 0) {
                audio = 0;
            } else if (strcasecmp("yes", optarg) == 0) {
                audio = 1;
                convertToxLaw = WA_PCMU;
            } else if (strcasecmp("alaw", optarg) == 0) {
                audio = 1;
                convertToxLaw = WA_PCMA;
            } else if (strcasecmp("ulaw", optarg) == 0) {
                audio = 1;
                convertToxLaw = WA_PCMU;
            } else if (strcasecmp("pcm", optarg) == 0) {
                audio = 1;
                convertToxLaw = WA_PCM;
            } else if (strcasecmp("aac", optarg) == 0) {
                audio = 2;
            }
            break;

        case 'p':
            errno = 0;    /* To distinguish success/failure after call */
            port = strtol(optarg, &endptr, 10);

            /* Check for various possible errors */
            if ((errno == ERANGE && (port == LONG_MAX || port == LONG_MIN)) || (errno != 0 && port == 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            if (endptr == optarg) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            break;

        case 's':
            sps_timing_info = 0;
            break;

        case 'd':
            errno = 0;    /* To distinguish success/failure after call */
            debug = strtol(optarg, &endptr, 10);

            /* Check for various possible errors */
            if ((errno == ERANGE && (debug == LONG_MAX || debug == LONG_MIN)) || (errno != 0 && debug == 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            if (endptr == optarg) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            if ((debug < 0) || (debug > 3)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
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

    // Get parameters from environment
    str = getenv("RRTSP_RES");
    if (str != NULL) {
        if (strcasecmp("low", str) == 0) {
            resolution = RESOLUTION_LOW;
        } else if (strcasecmp("high", str) == 0) {
            resolution = RESOLUTION_HIGH;
        } else if (strcasecmp("both", str) == 0) {
            resolution = RESOLUTION_BOTH;
        }
    }

    str = getenv("RRTSP_AUDIO");
    if (str != NULL) {
        if (strcasecmp("no", str) == 0) {
            audio = 0;
        } else if (strcasecmp("yes", str) == 0) {
            audio = 1;
            convertToxLaw = WA_PCMU;
        } else if (strcasecmp("alaw", str) == 0) {
            audio = 1;
            convertToxLaw = WA_PCMA;
        } else if (strcasecmp("ulaw", str) == 0) {
            audio = 1;
            convertToxLaw = WA_PCMU;
        } else if (strcasecmp("pcm", str) == 0) {
            audio = 1;
            convertToxLaw = WA_PCM;
        } else if (strcasecmp("aac", str) == 0) {
            audio = 2;
        }
    }

    str = getenv("RRTSP_PORT");
    if ((str != NULL) && (sscanf (str, "%i", &nm) == 1) && (nm >= 0)) {
        port = nm;
    }

    str = getenv("RRTSP_STI");
    if ((str != NULL) && (sscanf (str, "%i", &nm) == 1) && (nm >= 0) && (nm <= 1)) {
        sps_timing_info = nm;
    }

    str = getenv("RRTSP_DEBUG");
    if ((str != NULL) && (sscanf (str, "%i", &nm) == 1) && (nm >= 0) && (nm <= 3)) {
        debug = nm;
    }

    memset(user, 0, sizeof(user));
    str = getenv("RRTSP_USER");
    if ((str != NULL) && (strlen(str) < sizeof(user))) {
        strcpy(user, str);
    }

    memset(pwd, 0, sizeof(pwd));
    str = getenv("RRTSP_PWD");
    if ((str != NULL) && (strlen(str) < sizeof(pwd))) {
        strcpy(pwd, str);
    }

    // If fifo doesn't exist, disable audio
    if ((audio > 0) && (stat (inputAudioFileName, &stat_buffer) != 0)) {
        audio = 0;
    }

    setpriority(PRIO_PROCESS, 0, -10);

    // Fill input and output buffer struct
    strcpy(input_buffer.filename, BUFFER_FILE);
    input_buffer.size = BUF_SIZE;
    input_buffer.offset = BUF_OFFSET;

    output_buffer_low.resolution = RESOLUTION_LOW;
    output_buffer_low.size = OUTPUT_BUFFER_SIZE_LOW;
    output_buffer_low.buffer = (unsigned char *) malloc(OUTPUT_BUFFER_SIZE_LOW * sizeof(unsigned char));
    output_buffer_low.write_index = output_buffer_low.buffer;
    output_buffer_low.frame_read_index = 0;
    output_buffer_low.frame_write_index = 0;
    output_buffer_low.output_frame_size = sizeof(output_buffer_low.output_frame) / sizeof(output_buffer_low.output_frame[0]);
    if (output_buffer_low.buffer == NULL) {
        fprintf(stderr, "could not alloc memory\n");
        exit(EXIT_FAILURE);
    }
    output_buffer_low.output_frame[0].ptr = output_buffer_low.buffer;
    output_buffer_low.output_frame[0].counter = 0;
    output_buffer_low.output_frame[0].size = 0;

    output_buffer_high.resolution = RESOLUTION_HIGH;
    output_buffer_high.size = OUTPUT_BUFFER_SIZE_HIGH;
    output_buffer_high.buffer = (unsigned char *) malloc(OUTPUT_BUFFER_SIZE_HIGH * sizeof(unsigned char));
    output_buffer_high.write_index = output_buffer_high.buffer;
    output_buffer_high.frame_read_index = 0;
    output_buffer_high.frame_write_index = 0;
    output_buffer_high.output_frame_size = sizeof(output_buffer_high.output_frame) / sizeof(output_buffer_high.output_frame[0]);
    if (output_buffer_high.buffer == NULL) {
        fprintf(stderr, "could not alloc memory\n");
        exit(EXIT_FAILURE);
    }
    output_buffer_high.output_frame[0].ptr = output_buffer_high.buffer;
    output_buffer_high.output_frame[0].counter = 0;
    output_buffer_high.output_frame[0].size = 0;

    // Start capture thread
    if (pthread_mutex_init(&(output_buffer_low.mutex), NULL) != 0) { 
        *env << "Failed to create mutex\n";
        exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&(output_buffer_high.mutex), NULL) != 0) { 
        *env << "Failed to create mutex\n";
        exit(EXIT_FAILURE);
    }
    pth_ret = pthread_create(&capture_thread, NULL, capture, (void*) NULL);
    if (pth_ret != 0) {
        *env << "Failed to create capture thread\n";
        exit(EXIT_FAILURE);
    }
    pthread_detach(capture_thread);

    sleep(2);

    // Begin by setting up our usage environment:
    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);

    UserAuthenticationDatabase* authDB = NULL;

    if ((user[0] != '\0') && (pwd[0] != '\0')) {
        // To implement client access control to the RTSP server, do the following:
        authDB = new UserAuthenticationDatabase;
        authDB->addUserRecord(user, pwd);
        // Repeat the above with each <username>, <password> that you wish to allow
        // access to the server.
    }

    // Create the RTSP server:
    RTSPServer* rtspServer = RTSPServer::createNew(*env, port, authDB);
    if (rtspServer == NULL) {
        *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
        exit(1);
    }

    StreamReplicator* replicator = NULL;
    if (audio == 1) {
        // Create and start the replicator that will be given to each subsession
        replicator = startReplicatorStream(inputAudioFileName, convertToxLaw);
    } else if (audio == 2) {
        // Create and start the replicator that will be given to each subsession
        replicator = startReplicatorStream(inputAudioFileName);
    }

    char const* descriptionString = "Session streamed by \"rRTSPServer\"";

    // Set up each of the possible streams that can be served by the
    // RTSP server.  Each such stream is implemented using a
    // "ServerMediaSession" object, plus one or more
    // "ServerMediaSubsession" objects for each audio/video substream.

    // A H.264 video elementary stream:
    if ((resolution == RESOLUTION_HIGH) || (resolution == RESOLUTION_BOTH))
    {
        char const* streamName = "ch0_0.h264";

        // First, make sure that the RTPSinks' buffers will be large enough to handle the huge size of DV frames (as big as 288000).
        OutPacketBuffer::maxSize = 262144;

        ServerMediaSession* sms_high
            = ServerMediaSession::createNew(*env, streamName, streamName,
                                              descriptionString);
        sms_high->addSubsession(H264VideoFramedMemoryServerMediaSubsession
                                   ::createNew(*env, &output_buffer_high, reuseFirstSource));
        if (audio == 1) {
            sms_high->addSubsession(WAVAudioFifoServerMediaSubsession
                                       ::createNew(*env, replicator, reuseFirstSource, convertToxLaw));
        } else if (audio == 2) {
            sms_high->addSubsession(ADTSFromWAVAudioFifoServerMediaSubsession
                                       ::createNew(*env, replicator, reuseFirstSource));
        }
        rtspServer->addServerMediaSession(sms_high);

        announceStream(rtspServer, sms_high, streamName, audio);
    }

    // A H.264 video elementary stream:
    if ((resolution == RESOLUTION_LOW) || (resolution == RESOLUTION_BOTH))
    {
        char const* streamName = "ch0_1.h264";

        // First, make sure that the RTPSinks' buffers will be large enough to handle the huge size of DV frames (as big as 288000).
        OutPacketBuffer::maxSize = 262144;

        ServerMediaSession* sms_low
            = ServerMediaSession::createNew(*env, streamName, streamName,
                                              descriptionString);
        sms_low->addSubsession(H264VideoFramedMemoryServerMediaSubsession
                                   ::createNew(*env, &output_buffer_low, reuseFirstSource));
        if (audio == 1) {
            sms_low->addSubsession(WAVAudioFifoServerMediaSubsession
                                       ::createNew(*env, replicator, reuseFirstSource, convertToxLaw));
        } else if (audio == 2) {
            sms_low->addSubsession(ADTSFromWAVAudioFifoServerMediaSubsession
                                       ::createNew(*env, replicator, reuseFirstSource));
        }
        rtspServer->addServerMediaSession(sms_low);

        announceStream(rtspServer, sms_low, streamName, audio);
    }

    // A PCM audio elementary stream:
    if (audio != 0)
    {
        char const* streamName = "ch0_2.h264";

        // First, make sure that the RTPSinks' buffers will be large enough to handle the huge size of DV frames (as big as 288000).
        OutPacketBuffer::maxSize = 262144;

        ServerMediaSession* sms_audio
            = ServerMediaSession::createNew(*env, streamName, streamName,
                                              descriptionString);
        if (audio == 1) {
            sms_audio->addSubsession(WAVAudioFifoServerMediaSubsession
                                       ::createNew(*env, replicator, reuseFirstSource, convertToxLaw));
        } else if (audio == 2) {
            sms_audio->addSubsession(ADTSFromWAVAudioFifoServerMediaSubsession
                                       ::createNew(*env, replicator, reuseFirstSource));
        }
        rtspServer->addServerMediaSession(sms_audio);

        announceStream(rtspServer, sms_audio, streamName, audio);
    }

    // Also, attempt to create a HTTP server for RTSP-over-HTTP tunneling.
    // Try first with the default HTTP port (80), and then with the alternative HTTP
    // port numbers (8000 and 8080).
/*
    if (rtspServer->setUpTunnelingOverHTTP(80) || rtspServer->setUpTunnelingOverHTTP(8000) || rtspServer->setUpTunnelingOverHTTP(8080)) {
        *env << "\n(We use port " << rtspServer->httpServerPortNum() << " for optional RTSP-over-HTTP tunneling.)\n";
    } else {
        *env << "\n(RTSP-over-HTTP tunneling is not available.)\n";
    }
*/

    env->taskScheduler().doEventLoop(); // does not return

    pthread_mutex_destroy(&(output_buffer_low.mutex));
    pthread_mutex_destroy(&(output_buffer_high.mutex));

    // Free buffers
    free(output_buffer_low.buffer);
    free(output_buffer_high.buffer);

    return 0; // only to prevent compiler warning
}
