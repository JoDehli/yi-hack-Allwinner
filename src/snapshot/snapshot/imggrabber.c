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
 * The position of the frame is written in /tmp/iframe.idx
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

#define BUF_OFFSET_Y20GA 300
#define BUF_OFFSET_Y25GA 300
#define BUF_OFFSET_Y30QA 300

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define I_FILE "/tmp/iframe.idx"
#define FF_INPUT_BUFFER_PADDING_SIZE 32

#define RESOLUTION_NONE 0
#define RESOLUTION_LOW  360
#define RESOLUTION_HIGH 1080
#define RESOLUTION_BOTH 1440

#define RESOLUTION_FHD  1080
#define RESOLUTION_3K   1296

#define PATH_RES_LOW  "/home/yi-hack/etc/wm_res/low/wm_540p_"
#define PATH_RES_HIGH "/home/yi-hack/etc/wm_res/high/wm_540p_"

#define W_LOW 640
#define H_LOW 360
#define W_FHD 1920
#define H_FHD 1080

typedef struct {
    int sps_addr;
    int sps_len;
    int pps_addr;
    int pps_len;
    int idr_addr;
    int idr_len;
} frame;

int buf_offset;
int buf_size;
int res;
int debug;

unsigned char *addr;

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

int frame_decode(unsigned char *outbuffer, unsigned char *p, int length)
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

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        if (debug) fprintf(stderr, "Codec h264 not found\n");
        return -2;
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
    fprintf(stderr, "\t-h, --help              Show this help\n");
}

int main(int argc, char **argv)
{
    FILE *fFS, *fIdx, *fBuf;
    uint32_t offset, length;
    frame hl_frame[2];
    int hl_frame_index;
    unsigned char *bufferh264, *bufferyuv;
    int watermark = 0;
    int width, height;

    int c;

    buf_offset = BUF_OFFSET_Y20GA;
    res = RESOLUTION_HIGH;
    width = W_FHD;
    height = H_FHD;
    debug = 1;

    while (1) {
        static struct option long_options[] = {
            {"model",     required_argument, 0, 'm'},
            {"res",       required_argument, 0, 'r'},
            {"watermark", no_argument,       0, 'w'},
            {"help",      no_argument,       0, 'h'},
            {0,           0,                 0,  0 }
        };

        int option_index = 0;
        c = getopt_long(argc, argv, "m:r:wh",
            long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'm':
                if (strcasecmp("y20ga", optarg) == 0) {
                    buf_offset = BUF_OFFSET_Y20GA;
                } else if (strcasecmp("y25ga", optarg) == 0) {
                    buf_offset = BUF_OFFSET_Y25GA;
                } else if (strcasecmp("y30qa", optarg) == 0) {
                    buf_offset = BUF_OFFSET_Y30QA;
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
        hl_frame_index = 1;
    } else {
        width = W_FHD;
        height = H_FHD;
        hl_frame_index = 0;
    }

    fFS = fopen(BUFFER_FILE, "r");
    if ( fFS == NULL ) {
        fprintf(stderr, "could not get size of %s\n", BUFFER_FILE);
        exit(-1);
    }
    fseek(fFS, 0, SEEK_END);
    buf_size = ftell(fFS);
    fclose(fFS);
    if (debug) fprintf(stderr, "The size of the buffer is %d\n", buf_size);

    fIdx = fopen(I_FILE, "r");
    if ( fIdx == NULL ) {
        fprintf(stderr, "Could not open file %s\n", I_FILE);
        exit(-2);
    }
    if (fread(hl_frame, 1, 2 * sizeof(frame), fIdx) != 2 * sizeof(frame)) {
        fprintf(stderr, "Error reading file %s\n", I_FILE);
        exit(-3);
    }

    fBuf = fopen(BUFFER_FILE, "r") ;
    if (fBuf == NULL) {
        fprintf(stderr, "Could not open file %s\n", BUFFER_FILE);
        exit(-4);
    }

    // Map file to memory
    addr = (unsigned char*) mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fileno(fBuf), 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "Error mapping file %s\n", BUFFER_FILE);
        exit(-5);
    }
    if (debug) fprintf(stderr, "Mapping file %s, size %d, to %08x\n", BUFFER_FILE, buf_size, addr);

    // Closing the file
    fclose(fBuf) ;

    // Add FF_INPUT_BUFFER_PADDING_SIZE to make the size compatible with ffmpeg conversion
    bufferh264 = (unsigned char *) malloc(hl_frame[hl_frame_index].sps_len + hl_frame[hl_frame_index].pps_len + hl_frame[hl_frame_index].idr_len + FF_INPUT_BUFFER_PADDING_SIZE);
    if (bufferh264 == NULL) {
        fprintf(stderr, "Unable to allocate memory\n");
        exit(-6);
    }

    bufferyuv = (unsigned char *) malloc(width * height * 3 / 2);
    if (bufferyuv == NULL) {
        fprintf(stderr, "Unable to allocate memory\n");
        exit(-7);
    }

    cb_memcpy(bufferh264, addr + hl_frame[hl_frame_index].sps_addr, hl_frame[hl_frame_index].sps_len);
    cb_memcpy(bufferh264 + hl_frame[hl_frame_index].sps_len, addr + hl_frame[hl_frame_index].pps_addr, hl_frame[hl_frame_index].pps_len);
    cb_memcpy(bufferh264 + hl_frame[hl_frame_index].sps_len + hl_frame[hl_frame_index].pps_len, addr + hl_frame[hl_frame_index].idr_addr, hl_frame[hl_frame_index].idr_len);

    if (debug) fprintf(stderr, "Decoding h264 frame\n");
    if(frame_decode(bufferyuv, bufferh264, hl_frame[hl_frame_index].sps_len + hl_frame[hl_frame_index].pps_len + hl_frame[hl_frame_index].idr_len) < 0) {
        fprintf(stderr, "Error decoding h264 frame\n");
        exit(-8);
    }
    free(bufferh264);

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

    return 0;
}
