/*
 * Stream MJPEG from UVC cameras
 *
 * Copyright 2012 Reading Makerspace Ltd
 * Authors:
 *  Tom Stoeveken
 *  Gary Fletcher
 *  Barnaby Shearer
 *
 * Licence GPLv2
 */

#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/mman.h>

const char* huffman_table = "\
\xFF\xC4\x01\xA2\x00\x00\x01\x05\x01\x01\x01\x01\
\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x01\x02\
\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x01\x00\x03\
\x01\x01\x01\x01\x01\x01\x01\x01\x01\x00\x00\x00\
\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\
\x0A\x0B\x10\x00\x02\x01\x03\x03\x02\x04\x03\x05\
\x05\x04\x04\x00\x00\x01\x7D\x01\x02\x03\x00\x04\
\x11\x05\x12\x21\x31\x41\x06\x13\x51\x61\x07\x22\
\x71\x14\x32\x81\x91\xA1\x08\x23\x42\xB1\xC1\x15\
\x52\xD1\xF0\x24\x33\x62\x72\x82\x09\x0A\x16\x17\
\x18\x19\x1A\x25\x26\x27\x28\x29\x2A\x34\x35\x36\
\x37\x38\x39\x3A\x43\x44\x45\x46\x47\x48\x49\x4A\
\x53\x54\x55\x56\x57\x58\x59\x5A\x63\x64\x65\x66\
\x67\x68\x69\x6A\x73\x74\x75\x76\x77\x78\x79\x7A\
\x83\x84\x85\x86\x87\x88\x89\x8A\x92\x93\x94\x95\
\x96\x97\x98\x99\x9A\xA2\xA3\xA4\xA5\xA6\xA7\xA8\
\xA9\xAA\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\xC2\
\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xD2\xD3\xD4\xD5\
\xD6\xD7\xD8\xD9\xDA\xE1\xE2\xE3\xE4\xE5\xE6\xE7\
\xE8\xE9\xEA\xF1\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9\
\xFA\x11\x00\x02\x01\x02\x04\x04\x03\x04\x07\x05\
\x04\x04\x00\x01\x02\x77\x00\x01\x02\x03\x11\x04\
\x05\x21\x31\x06\x12\x41\x51\x07\x61\x71\x13\x22\
\x32\x81\x08\x14\x42\x91\xA1\xB1\xC1\x09\x23\x33\
\x52\xF0\x15\x62\x72\xD1\x0A\x16\x24\x34\xE1\x25\
\xF1\x17\x18\x19\x1A\x26\x27\x28\x29\x2A\x35\x36\
\x37\x38\x39\x3A\x43\x44\x45\x46\x47\x48\x49\x4A\
\x53\x54\x55\x56\x57\x58\x59\x5A\x63\x64\x65\x66\
\x67\x68\x69\x6A\x73\x74\x75\x76\x77\x78\x79\x7A\
\x82\x83\x84\x85\x86\x87\x88\x89\x8A\x92\x93\x94\
\x95\x96\x97\x98\x99\x9A\xA2\xA3\xA4\xA5\xA6\xA7\
\xA8\xA9\xAA\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\
\xC2\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xD2\xD3\xD4\
\xD5\xD6\xD7\xD8\xD9\xDA\xE2\xE3\xE4\xE5\xE6\xE7\
\xE8\xE9\xEA\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9\xFA\
";

#define BOUNDARY "uvc_stream"
#define BUFFERS 1

#define CHECK(x) do { \
    int retval = (x); \
    if (retval < 0) { \
        fprintf(stderr, "Runtime error: %s returned %d at %s:%d\n", #x, retval, __FILE__, __LINE__); \
        exit(retval); \
    } \
} while (0)

int stop=0;
int socketd;
pthread_mutex_t db = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  db_update = PTHREAD_COND_INITIALIZER;
unsigned char *g_buf = NULL;
int g_size = 0;
struct timeval g_tv;
int fps = 5;

struct vdIn {
    int fd;
    char *videodevice;
    struct v4l2_buffer buf;
    void *mem[BUFFERS];
    unsigned char *tmpbuffer;
    unsigned char *framebuffer;
    int isstreaming;
    int framesizeIn;
};

struct vdIn *videoIn;

int open_videodev(
    char *device,
    int width,
    int height,
    int fps
) {
    int i;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers rb;

    videoIn->videodevice = (char *) calloc(1, 16 * sizeof(char));
    snprintf(videoIn->videodevice, 12, "%s", device);
    CHECK(videoIn->fd = open(videoIn->videodevice, O_RDWR));

    memset(&fmt, 0, sizeof(struct v4l2_format));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    CHECK(ioctl(videoIn->fd, VIDIOC_S_FMT, &fmt));

    struct v4l2_streamparm *setfps;
    setfps = (struct v4l2_streamparm *) calloc(1, sizeof(struct v4l2_streamparm));
    memset(setfps, 0, sizeof(struct v4l2_streamparm));
    setfps->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    setfps->parm.capture.timeperframe.numerator = 1;
    setfps->parm.capture.timeperframe.denominator = fps;
    CHECK(ioctl(videoIn->fd, VIDIOC_S_PARM, setfps));

    memset(&rb, 0, sizeof(struct v4l2_requestbuffers));
    rb.count = BUFFERS;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    CHECK(ioctl(videoIn->fd, VIDIOC_REQBUFS, &rb));

    for (i = 0; i < BUFFERS; i++) {
        memset(&videoIn->buf, 0, sizeof(struct v4l2_buffer));
        videoIn->buf.index = i;
        videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        videoIn->buf.memory = V4L2_MEMORY_MMAP;
        CHECK(ioctl(videoIn->fd, VIDIOC_QUERYBUF, &videoIn->buf));

        videoIn->mem[i] = mmap(0, videoIn->buf.length, PROT_READ, MAP_SHARED, videoIn->fd,videoIn->buf.m.offset);
        if (videoIn->mem[i] == MAP_FAILED) {
            printf("Unable to map buffer (%d)\n", errno);
        }
    }

    for (i = 0; i < BUFFERS; ++i) {
        memset(&videoIn->buf, 0, sizeof(struct v4l2_buffer));
        videoIn->buf.index = i;
        videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        videoIn->buf.memory = V4L2_MEMORY_MMAP;
        CHECK(ioctl(videoIn->fd, VIDIOC_QBUF, &videoIn->buf));
    }

    videoIn->framesizeIn = (width * height << 1);
    videoIn->tmpbuffer = (unsigned char *) calloc(1, (size_t) videoIn->framesizeIn);
    videoIn->framebuffer = (unsigned char *) calloc(1, (size_t) width * (height + 8) * 2);
    return videoIn->fd;
}

int close_videodev() {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (videoIn->isstreaming) {
        CHECK(ioctl(videoIn->fd, VIDIOC_STREAMOFF, &type));
        videoIn->isstreaming = 0;
    }
    if (videoIn->tmpbuffer) {
        free(videoIn->tmpbuffer);
    }
    videoIn->tmpbuffer = NULL;
    free(videoIn->framebuffer);
    videoIn->framebuffer = NULL;
    free(videoIn->videodevice);
    videoIn->videodevice = NULL;

    return 0;
}

void write_pic(int fd, unsigned char *buf, int size) {
    unsigned char *ptdeb, *ptcur = buf;
    int sizein;
    ptdeb = ptcur = buf;
    while (((ptcur[0] << 8) | ptcur[1]) != 0xffc0) {
        ptcur++;
    }
    sizein = ptcur - ptdeb;
    (void)(1 + write(fd, buf, sizein));
    (void)(1 + write(fd, huffman_table, 420));
    (void)(1 + write(fd, ptcur, size - sizein));
}


void *client_thread( void *arg ) {
    int fd = *((int *)arg);
    fd_set fds;
    unsigned char *frame = (unsigned char *)calloc(1, (size_t)videoIn->framesizeIn);
    int ok = 1, frame_size=0;
    char buffer[1024] = {0};
    struct timeval to;

    if (arg!=NULL) free(arg); else exit(1);

    to.tv_sec  = 5;
    to.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    if( select(fd+1, &fds, NULL, NULL, &to) <= 0) {
        close(fd);
        free(frame);
        return NULL;
    }

    sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
        "Server: UVC Streamer\r\n" \
        "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n" \
        "Cache-Control: no-cache\r\n" \
        "Cache-Control: private\r\n" \
        "Pragma: no-cache\r\n" \
        "\r\n" \
        "--" BOUNDARY "\n"
    );
    (void)(1 + write(fd, buffer, strlen(buffer)));
    while ( ok >= 0 && !stop ) {

        pthread_cond_wait(&db_update, &db);

        frame_size = g_size;
        memcpy(frame, g_buf, frame_size);

        pthread_mutex_unlock( &db );

        sprintf(buffer, "Content-type: image/jpeg\n");
        ok = ( write(fd, buffer, strlen(buffer)) >= 0)?1:0;
        if( ok < 0 ) break;

        sprintf(buffer, "X-StartTime: %lu\n\n", g_tv.tv_sec*1000 + g_tv.tv_usec/1000);
        ok = ( write(fd, buffer, strlen(buffer)) >= 0)?1:0;
        if( ok < 0 ) break;

        write_pic(fd, frame, frame_size);

        sprintf(buffer, "\n--" BOUNDARY "\n");
        ok = ( write(fd, buffer, strlen(buffer)) >= 0)?1:0;
        if( ok < 0 ) break;
    }
    close(fd);
    free(frame);

    return NULL;
}

void *cam_thread() {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct timeval tv;
    int frameno = 0;

    while( !stop ) {

        if (!videoIn->isstreaming) {
            CHECK(ioctl(videoIn->fd, VIDIOC_STREAMON, &type));
            videoIn->isstreaming = 1;
        }

        memset(&videoIn->buf, 0, sizeof(struct v4l2_buffer));
        videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        videoIn->buf.memory = V4L2_MEMORY_MMAP;
        CHECK(ioctl(videoIn->fd, VIDIOC_DQBUF, &videoIn->buf));
        gettimeofday(&tv, NULL);
        if(++frameno > 30/fps) {
            memcpy(videoIn->tmpbuffer, videoIn->mem[videoIn->buf.index], videoIn->buf.bytesused);
        }
        CHECK(ioctl(videoIn->fd, VIDIOC_QBUF, &videoIn->buf));

        if(frameno > 30/fps) {
            pthread_mutex_lock( &db );
            g_tv.tv_sec = tv.tv_sec;
            g_tv.tv_usec = tv.tv_usec;
            g_size = videoIn->buf.bytesused;
            memcpy(g_buf, videoIn->tmpbuffer, videoIn->buf.bytesused);
            pthread_cond_broadcast(&db_update);
            pthread_mutex_unlock( &db );
            frameno = 0;
        }
    }
    return 0;
}

void signal_handler() {
    stop = 1;

    fprintf(stderr, "shutdown...\n");
    usleep(1000*1000);
    close_videodev();
    free(videoIn);
    CHECK(close (socketd));
    pthread_cond_destroy(&db_update);
    pthread_mutex_destroy(&db);
    exit(0);
}

int main(int argc, char *argv[]) {
    struct sockaddr_in addr;
    pthread_t client, cam;
    char *dev = "/dev/video0";
    int on = 1, dodaemon = 0, opt;
    int width=640, height=480, stream_port=htons(8080);

    static struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"width", required_argument, 0, 'w'},
        {"height", required_argument, 0, 'h'},
        {"fps", required_argument, 0, 'f'},
        {"port", required_argument, 0, 'p'},
        {"background", no_argument, 0, 'b'},
        {0, 0, 0, 0}
    };

    while((opt = getopt_long_only(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                dev = strdup(optarg);
                break;
            case 'w':
                width = atoi(optarg);
                break;
            case 'h':
                height = atoi(optarg);
                break;
            case 'f':
                fps=atoi(optarg);
                break;
            case 'p':
                stream_port=htons(atoi(optarg));
                break;
            case 'b':
                dodaemon=1;
                break;
            default:
                fprintf(stderr, "Usage: %s\n" \
                    " [-d | --device ]......: video device to open (your camera)\n" \
                    " [-w | --width ].......: default 640\n" \
                    " [-h | --height ]......: default 480\n" \
                    " [-f | --fps ].........: frames per second\n" \
                    " [-p | --port ]........: TCP-port for the server\n" \
                    " [-b | --background]...: fork to the background, daemon mode\n",
                    argv[0]
                );
                return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);

    if (dodaemon) CHECK(daemon(0, 0));

    videoIn = (struct vdIn*) calloc(1, sizeof(struct vdIn));
    fprintf(stderr, "http://localhost:%i/\n", ntohs(stream_port));

    open_videodev(dev, width, height, 30); //Always record at 30fps to minimize tearing

    CHECK(socketd = socket(PF_INET, SOCK_STREAM, 0));
    CHECK(setsockopt(socketd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = stream_port;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(bind(socketd, (struct sockaddr*)&addr, sizeof(addr)));

    CHECK(listen(socketd, 10));

    videoIn->tmpbuffer = (unsigned char *) calloc(1, (size_t)videoIn->framesizeIn);
    g_buf = (unsigned char *) calloc(1, (size_t)videoIn->framesizeIn);

    pthread_create(&cam, 0, cam_thread, NULL);
    pthread_detach(cam);

    while (1) {
        int *pfd = (int *)calloc(1, sizeof(int));
        *pfd = accept(socketd, 0, 0);
        pthread_create(&client, NULL, &client_thread, pfd);
        pthread_detach(client);
    }
    return 0;
}
