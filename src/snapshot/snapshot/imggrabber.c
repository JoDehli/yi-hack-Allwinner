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
 * Read the last h264 i-frame from the buffer and convert it using libavcodec
 * and libjpeg.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <getopt.h>

#ifdef HAVE_AV_CONFIG_H
#undef HAVE_AV_CONFIG_H
#endif

#include "libavcodec/avcodec.h"

#include "convert2jpg.h"
#include "add_water.h"

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

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define BUFFER_SHM "fshare_frame_buf"
#define READ_LOCK_FILE "fshare_read_lock"
#define WRITE_LOCK_FILE "fshare_write_lock"
#define FF_INPUT_BUFFER_PADDING_SIZE 32

#define RESOLUTION_LOW  360
#define RESOLUTION_HIGH 1080

#define RESOLUTION_FHD  1080
#define RESOLUTION_3K   1296

#define PATH_RES_LOW  "/home/yi-hack/etc/wm_res/low/wm_540p_"
#define PATH_RES_HIGH "/home/yi-hack/etc/wm_res/high/wm_540p_"

#define W_LOW 640
#define H_LOW 360
#define W_FHD 1920
#define H_FHD 1080
#define W_3K 2304
#define H_3K 1296

typedef struct {
    int sps_addr;
    int sps_len;
    int pps_addr;
    int pps_len;
    int vps_addr;
    int vps_len;
    int idr_addr;
    int idr_len;
} frame;

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
int res;
int debug;

unsigned char *addr;

#ifdef USE_SEMAPHORE
sem_t *sem_fshare_read_lock = SEM_FAILED;
sem_t *sem_fshare_write_lock = SEM_FAILED;
#endif

unsigned char *cb_move(unsigned char *buf, int offset)
{
    buf += offset;
    if ((offset > 0) && (buf > addr + buf_size))
        buf -= (buf_size - buf_offset);
    if ((offset < 0) && (buf < addr + buf_offset))
        buf += (buf_size - buf_offset);

    return buf;
}

void *cb_memcpy(void * dest, const void * src, size_t n)
{
    unsigned char *uc_src = (unsigned char *) src;
    unsigned char *uc_dest = (unsigned char *) dest;

    if (uc_src + n > addr + buf_size) {
        memcpy(uc_dest, uc_src, addr + buf_size - uc_src);
        memcpy(uc_dest + (addr + buf_size - uc_src), addr + buf_offset, n - (addr + buf_size - uc_src));
    } else {
        memcpy(uc_dest, src, n);
    }
    return dest;
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

int frame_decode(unsigned char *outbuffer, unsigned char *p, int length, int h26x)
{
    AVCodec *codec;
    AVCodecContext *c= NULL;
    AVFrame *picture;
    int got_picture, len;
    FILE *fOut;
    uint8_t *inbuf;
    AVPacket avpkt;
    int i, j, size;

//////////////////////////////////////////////////////////
//                    Reading H264                      //
//////////////////////////////////////////////////////////

    if (debug) fprintf(stderr, "Starting decode\n");

    av_init_packet(&avpkt);

    if (h26x == 4) {
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            if (debug) fprintf(stderr, "Codec h264 not found\n");
            return -2;
        }
    } else {
        codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        if (!codec) {
            if (debug) fprintf(stderr, "Codec hevc not found\n");
            return -2;
        }
    }

    c = avcodec_alloc_context3(codec);
    picture = av_frame_alloc();

    if((codec->capabilities) & AV_CODEC_CAP_TRUNCATED)
        (c->flags) |= AV_CODEC_FLAG_TRUNCATED;

    if (avcodec_open2(c, codec, NULL) < 0) {
        if (debug) fprintf(stderr, "Could not open codec h264\n");
        av_free(c);
        return -2;
    }

    // inbuf is already allocated in the main function
    inbuf = p;
    memset(inbuf + length, 0, FF_INPUT_BUFFER_PADDING_SIZE);

    // Get only 1 frame
    memcpy(inbuf, p, length);
    avpkt.size = length;
    avpkt.data = inbuf;

    // Decode frame
    if (debug) fprintf(stderr, "Decode frame\n");
    if (c->codec_type == AVMEDIA_TYPE_VIDEO ||
         c->codec_type == AVMEDIA_TYPE_AUDIO) {

        len = avcodec_send_packet(c, &avpkt);
        if (len < 0 && len != AVERROR(EAGAIN) && len != AVERROR_EOF) {
            if (debug) fprintf(stderr, "Error decoding frame\n");
            return -2;
        } else {
            if (len >= 0)
                avpkt.size = 0;
            len = avcodec_receive_frame(c, picture);
            if (len >= 0)
                got_picture = 1;
        }
    }
    if(!got_picture) {
        if (debug) fprintf(stderr, "No input frame\n");
        av_frame_free(&picture);
        avcodec_close(c);
        av_free(c);
        return -2;
    }

    if (debug) fprintf(stderr, "Writing yuv buffer\n");
    memset(outbuffer, 0x80, c->width * c->height * 3 / 2);
    memcpy(outbuffer, picture->data[0], c->width * c->height);
    for(i=0; i<c->height/2; i++) {
        for(j=0; j<c->width/2; j++) {
            outbuffer[c->width * c->height + c->width * i +  2 * j] = *(picture->data[1] + i * picture->linesize[1] + j);
            outbuffer[c->width * c->height + c->width * i +  2 * j + 1] = *(picture->data[2] + i * picture->linesize[2] + j);
        }
    }

    // Clean memory
    if (debug) fprintf(stderr, "Cleaning ffmpeg memory\n");
    av_frame_free(&picture);
    avcodec_close(c);
    av_free(c);

    return 0;
}

int add_watermark(char *buffer, int w_res, int h_res)
{
    char path_res[1024];
    FILE *fBuf;
    WaterMarkInfo WM_info;

    if (w_res != W_LOW) {
        strcpy(path_res, PATH_RES_HIGH);
    } else {
        strcpy(path_res, PATH_RES_LOW);
    }

    if (WMInit(&WM_info, path_res) < 0) {
        fprintf(stderr, "water mark init error\n");
        return -1;
    } else {
        if (w_res != W_LOW) {
            AddWM(&WM_info, w_res, h_res, buffer,
                buffer + w_res*h_res, w_res-460, h_res-40, NULL);
        } else {
            AddWM(&WM_info, w_res, h_res, buffer,
                buffer + w_res*h_res, w_res-230, h_res-20, NULL);
        }
        WMRelease(&WM_info);
    }

    return 0;
}

void usage(char *prog_name)
{
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "\t-m, --model MODEL       Set model: \"y20ga\", \"y25ga\" or \"y30qa\" (default \"y20ga\")\n");
    fprintf(stderr, "\t-r, --res RES           Set resolution: \"low\" or \"high\" (default \"high\")\n");
    fprintf(stderr, "\t-w, --watermark         Add watermark to image\n");
    fprintf(stderr, "\t-d, --debug             Enable debug\n");
    fprintf(stderr, "\t-h, --help              Show this help\n");
}

int main(int argc, char **argv)
{
    FILE *fFS;
    int fshm;
    uint32_t offset, length;

    unsigned char *buf_idx, *buf_idx_cur, *buf_idx_end;
    unsigned char *bufferh26x, *bufferyuv;
    int watermark = 0;
    int model_high_res;
    int width, height;

    int i, c;

    struct frame_header fh, fhs, fhp, fhv, fhi;
    unsigned char *fhs_addr, *fhp_addr, *fhv_addr, *fhi_addr;

    buf_offset = BUF_OFFSET_Y20GA;
    frame_header_size = FRAME_HEADER_SIZE_Y20GA;
    res = RESOLUTION_HIGH;
    model_high_res = RESOLUTION_FHD;
    width = W_FHD;
    height = H_FHD;
    debug = 0;

    while (1) {
        static struct option long_options[] = {
            {"model",     required_argument, 0, 'm'},
            {"res",       required_argument, 0, 'r'},
            {"watermark", no_argument,       0, 'w'},
            {"debug",     no_argument,       0, 'd'},
            {"help",      no_argument,       0, 'h'},
            {0,           0,                 0,  0 }
        };

        int option_index = 0;
        c = getopt_long(argc, argv, "m:r:wdh",
            long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'm':
                if (strcasecmp("y20ga", optarg) == 0) {
                    buf_offset = BUF_OFFSET_Y20GA;
                    frame_header_size = FRAME_HEADER_SIZE_Y20GA;
                    model_high_res = RESOLUTION_FHD;
                } else if (strcasecmp("y25ga", optarg) == 0) {
                    buf_offset = BUF_OFFSET_Y25GA;
                    frame_header_size = FRAME_HEADER_SIZE_Y25GA;
                    model_high_res = RESOLUTION_FHD;
                } else if (strcasecmp("y30qa", optarg) == 0) {
                    buf_offset = BUF_OFFSET_Y30QA;
                    frame_header_size = FRAME_HEADER_SIZE_Y30QA;
                    model_high_res = RESOLUTION_FHD;
                }
                break;

            case 'r':
                if (strcasecmp("low", optarg) == 0)
                    res = RESOLUTION_LOW;
                else
                    res = RESOLUTION_HIGH;
                break;

            case 'w':
                watermark = 1;
                break;

            case 'd':
                debug = 1;
                break;

            case 'h':
            default:
                usage(argv[0]);
                exit(-1);
                break;
        }
    }

    if (debug) fprintf(stderr, "Starting program\n");

    // Check if snapshot is disabled
    if (access("/tmp/snapshot.disabled", F_OK ) == 0 ) {
        fprintf(stderr, "Snapshot is disabled\n");
        return 0;
    }

    if (res == RESOLUTION_LOW) {
        width = W_LOW;
        height = H_LOW;
    } else {
        if (model_high_res == RESOLUTION_FHD) {
            width = W_FHD;
            height = H_FHD;
        } else {
            width = W_3K;
            height = H_3K;
        }
    }

    fFS = fopen(BUFFER_FILE, "r");
    if ( fFS == NULL ) {
        fprintf(stderr, "Could not get size of %s\n", BUFFER_FILE);
        exit(-2);
    }
    fseek(fFS, 0, SEEK_END);
    buf_size = ftell(fFS);
    fclose(fFS);
    if (debug) fprintf(stderr, "The size of the buffer is %d\n", buf_size);

#ifdef USE_SEMAPHORE
    if (sem_fshare_open() != 0) {
        fprintf(stderr, "Could not open semaphores\n") ;
        exit(-3);
    }
#endif

    // Opening an existing file
    fshm = shm_open(BUFFER_SHM, O_RDWR, 0);
    if (fshm == -1) {
        fprintf(stderr, "Could not open file %s\n", BUFFER_FILE) ;
        exit(-4);
    }

    // Map file to memory
    addr = (unsigned char*) mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fshm, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "Error mapping file %s\n", BUFFER_FILE);
        close(fshm);
        exit(-5);
    }
    if (debug) fprintf(stderr, "Mapping file %s, size %d, to %08x\n", BUFFER_FILE, buf_size, addr);

    // Closing the file
    close(fshm);

    fhs.len = 0;
    fhp.len = 0;
    fhv.len = 0;
    fhi.len = 0;
    fhs_addr = NULL;
    fhp_addr = NULL;
    fhv_addr = NULL;
    fhi_addr = NULL;

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

        buf_idx_cur = buf_idx;

        while (buf_idx_cur != buf_idx_end) {
            cb2s_headercpy((unsigned char *) &fh, buf_idx_cur, frame_header_size);
            // Check the len
            if (fh.len > buf_size - buf_offset - frame_header_size) {
                fhs_addr = NULL;
                break;
            }
            if (((res == RESOLUTION_LOW) && (fh.type & 0x0800)) || ((res == RESOLUTION_HIGH) && (fh.type & 0x0400))) {
                if (fh.type & 0x0002) {
                    memcpy((unsigned char *) &fhs, (unsigned char *) &fh, sizeof(struct frame_header));
                    fhs_addr = buf_idx_cur;
                } else if (fh.type & 0x0004) {
                    memcpy((unsigned char *) &fhp, (unsigned char *) &fh, sizeof(struct frame_header));
                    fhp_addr = buf_idx_cur;
                } else if (fh.type & 0x0008) {
                    memcpy((unsigned char *) &fhv, (unsigned char *) &fh, sizeof(struct frame_header));
                    fhv_addr = buf_idx_cur;
                } else if (fh.type & 0x0001) {
                    memcpy((unsigned char *) &fhi, (unsigned char *) &fh, sizeof(struct frame_header));
                    fhi_addr = buf_idx_cur;
                }
            }
            buf_idx_cur = cb_move(buf_idx_cur, fh.len + frame_header_size);
        }

#ifdef USE_SEMAPHORE
        sem_write_unlock();
#endif
        if (fhs_addr != NULL) break;
        usleep(10000);
    }

    // Remove headers
    if (fhv_addr != NULL) fhv_addr = cb_move(fhv_addr, frame_header_size);
    fhs_addr = cb_move(fhs_addr, frame_header_size + 6);
    fhs.len -= 6;
    fhp_addr = cb_move(fhp_addr, frame_header_size);
    fhi_addr = cb_move(fhi_addr, frame_header_size);

    // Add FF_INPUT_BUFFER_PADDING_SIZE to make the size compatible with ffmpeg conversion
    bufferh26x = (unsigned char *) malloc(fhv.len + fhs.len + fhp.len + fhi.len + FF_INPUT_BUFFER_PADDING_SIZE);
    if (bufferh26x == NULL) {
        fprintf(stderr, "Unable to allocate memory\n");
        exit(-6);
    }

    bufferyuv = (unsigned char *) malloc(width * height * 3 / 2);
    if (bufferyuv == NULL) {
        fprintf(stderr, "Unable to allocate memory\n");
        exit(-7);
    }

    if (fhv_addr != NULL) {
        cb_memcpy(bufferh26x, fhv_addr, fhv.len);
    }
    cb_memcpy(bufferh26x + fhv.len, fhs_addr, fhs.len);
    cb_memcpy(bufferh26x + fhv.len + fhs.len, fhp_addr, fhp.len);
    cb_memcpy(bufferh26x + fhv.len + fhs.len + fhp.len, fhi_addr, fhi.len);

    if (fhv_addr == NULL) {
        if (debug) fprintf(stderr, "Decoding h264 frame\n");
        if(frame_decode(bufferyuv, bufferh26x, fhs.len + fhp.len + fhi.len, 4) < 0) {
            fprintf(stderr, "Error decoding h264 frame\n");
            exit(-8);
        }
    } else {
        if (debug) fprintf(stderr, "Decoding h265 frame\n");
        if(frame_decode(bufferyuv, bufferh26x, fhv.len + fhs.len + fhp.len + fhi.len, 5) < 0) {
            fprintf(stderr, "Error decoding h265 frame\n");
            exit(-8);
        }
    }
    free(bufferh26x);

    if (watermark) {
        if (debug) fprintf(stderr, "Adding watermark\n");
        if (add_watermark(bufferyuv, width, height) < 0) {
            fprintf(stderr, "Error adding watermark\n");
            exit(-9);
        }
    }

    if (debug) fprintf(stderr, "Encoding jpeg image\n");
    if(YUVtoJPG("stdout", bufferyuv, width, height, width, height) < 0) {
        fprintf(stderr, "Error encoding jpeg file\n");
        exit(-10);
    }

    free(bufferyuv);

    // Unmap file from memory
    if (munmap(addr, buf_size) == -1) {
        fprintf(stderr, "Error munmapping file\n");
    } else {
        if (debug) fprintf(stderr, "Unmapping file %s, size %d, from %08x\n", BUFFER_FILE, buf_size, addr);
    }

#ifdef USE_SEMAPHORE
    sem_fshare_close();
#endif

    return 0;
}
