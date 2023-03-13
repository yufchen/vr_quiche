// Copyright (C) 2018-2019, Cloudflare, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

//#include <ev.h>
#include <gio/gio.h>
#include <glib.h>

#include <quiche.h>
#include "gstsink.h"

#define LOCAL_CONN_ID_LEN 16

#define MAX_DATAGRAM_SIZE 1350

#define MAIN_WINDOW_HEIGHT 600
#define MAIN_WINDOW_WIDTH 800

#define APP_SYNTHETIC_DATA 0
#define APP_H264_DATA 1

struct conn_io {
    int sock;

    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;

    quiche_conn *conn;
};


/* Global variables */
static int gl_use_dgram = -1;
static int gl_if_debug = 1;
static int gl_app_type = -1;
static int gl_num_pipeline = -1;
static int gl_num_streams = -1;
static int gl_num_urgency = -1;
static int gl_app_syn_period_new_stream = -1;
static int gl_urgency_step = -1;

GstElement* gl_pipeline;
SampleHandlerUserData *gl_pipeline_infos = NULL; //decode info
static GMutex *gl_mutex = NULL;
bool gl_if_init = false;




long getcurTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long curtime = tv.tv_sec *1000000 + tv.tv_usec;
    return curtime;
}

static void debug_log(const char *line, void *argp) {
    fprintf(stderr, "%s\n", line);
}

static void flush_egress(struct conn_io *conn_io) {
    static uint8_t out[MAX_DATAGRAM_SIZE];

    quiche_send_info send_info;

    while (1) {
        ssize_t written = quiche_conn_send(conn_io->conn, out, sizeof(out),
                                           &send_info);

        if (written == QUICHE_ERR_DONE) {
            //fprintf(stderr, "done writing\n");
            break;
        }

        if (written < 0) {
            fprintf(stderr, "failed to create packet: %zd\n", written);
            return;
        }

        ssize_t sent = sendto(conn_io->sock, out, written, 0,
                              (struct sockaddr *) &send_info.to,
                              send_info.to_len);

        if (sent != written) {
            perror("failed to send");
            return;
        }

        //fprintf(stderr, "%ld, sent %zd bytes\n", getcurTime(), sent);
    }
}

static gboolean recv_cb (GIOChannel *channel, GIOCondition condition, gpointer data) {
    if (gl_app_type == APP_H264_DATA) {
        g_mutex_lock(gl_mutex);
    }
    static bool req_sent = false;

    struct conn_io *conn_io = data;

    static uint8_t buf[65535];
//    if (gl_app_type == APP_H264_DATA) {
//        static char data_buf[40][500000]; //max number of concurrent streams
//        static int data_buf_pos[40] = {0};
//        static int data_buf_cur_avail = 0;
//        static int sid_to_data_buf[1000000] = {0};
//        if (!gl_if_init) {
//            memset(data_buf_pos, 0, sizeof(data_buf_pos));
//            memset(sid_to_data_buf, -1, sizeof(sid_to_data_buf));
//            gl_if_init = true;
//        }
//    }

    while (1) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_addr_len);

        ssize_t read = recvfrom(conn_io->sock, buf, sizeof(buf), 0,
                                (struct sockaddr *) &peer_addr,
                                &peer_addr_len);

        if (read < 0) {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                //fprintf(stderr, "recv would block\n");
                break;
            }

            perror("failed to read");
            return FALSE;
        }

        quiche_recv_info recv_info = {
                (struct sockaddr *) &peer_addr,
                peer_addr_len,

                (struct sockaddr *) &conn_io->local_addr,
                conn_io->local_addr_len,
        };

        ssize_t done = quiche_conn_recv(conn_io->conn, buf, read, &recv_info);

        if (done < 0) {
            fprintf(stderr, "failed to process packet %ld \n", done);
            continue;
        }

        //fprintf(stderr, "conn recv %zd bytes\n", done);
    }

    //fprintf(stderr, "%ld done reading\n", getcurTime());

    if (quiche_conn_is_closed(conn_io->conn)) {
        fprintf(stderr, "connection closed\n");
        return FALSE;
    }

    if (quiche_conn_is_established(conn_io->conn) && !req_sent) {
        const uint8_t *app_proto;
        size_t app_proto_len;

        quiche_conn_application_proto(conn_io->conn, &app_proto, &app_proto_len);

        fprintf(stderr, "connection established: %.*s\n", (int) app_proto_len, app_proto);

        const static uint8_t r[] = "Hello world\n";
        if (gl_use_dgram) {
            int size = 0;
            size = quiche_conn_dgram_send(conn_io->conn, (uint8_t *) r, sizeof(r));
            fprintf(stdout, "sent hello request dgram: %d\n", size);
        }
        else {
            int size = 0;
            size = quiche_conn_stream_send(conn_io->conn, 4, (uint8_t *) r, sizeof(r), true);
            fprintf(stdout, "sent hello request stream: %d\n", size);
        }
        req_sent = true;
    }

    if (quiche_conn_is_established(conn_io->conn)) {
        if (gl_use_dgram && quiche_conn_is_readable(conn_io->conn)) {
            //no dgram
            ssize_t recv_len = quiche_conn_dgram_recv(conn_io->conn, buf, sizeof(buf));
            if (recv_len < 0) {
                return FALSE;
            }
//            if (memcmp(buf, "eos", 3) == 0) {
//                printf("get eos, end\n");
//                gstreamer_receive_stop_pipeline(pipeline);
//                gstreamer_receive_destroy_pipeline(pipeline);
//                return FALSE;
//            }
            //printf("gst push recv %ld bytes\n", recv_len);
            //fprintf(stderr, "gst push recv %ld bytes\n", recv_len);
            //gstreamer_receive_push_buffer(pipeline, buf, recv_len);
            //printf("RECV from server: %s", buf);
        }
        else {
            uint64_t s = 0;
            quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);
            static int total_size = 0;
            //fprintf(stderr, "%ld start iter stream \n", getcurTime());


            while (quiche_stream_iter_next(readable, &s)) {
                //fprintf(stderr, "%ld, stream %" PRIu64 " is readable\n", getcurTime(), s);
                bool fin = false;
                ssize_t recv_len = quiche_conn_stream_recv(conn_io->conn, s, buf, sizeof(buf), &fin);
                total_size += recv_len;
                if (gl_if_debug) {
                    fprintf(stderr, "%ld stream_recv %ld bytes on stream id %ld, total size: %d\n", getcurTime(),
                            recv_len, s, total_size);
                }
                if (recv_len < 0 && recv_len != QUICHE_ERR_STREAM_RESET) {
                    fprintf(stderr, "other errors, recv len < 0\n");
                    continue;
                }

                /*if (gl_app_type == APP_H264_DATA) {
                    //if recv eos
                    if (memcmp(buf, "eos", 3) == 0) {
                        printf("get eos from srv\n");
                        gstreamer_receive_stop_pipeline(gl_pipeline);
                        break;
                    }

                    //push the recvd bytes back to the data buffer
                    int sid = ((s - 1) / 4 - 1); //<1000000 sid_to_bufid
                    if (sid_to_data_buf[sid] == -1) {
                        //this is a new stream
                        if (recv_len == QUICHE_ERR_STREAM_RESET) {
                            fprintf(stderr, "reset on non-existing stream\n");
                            continue;
                        }
                        int loop = 0;
                        while (data_buf_pos[data_buf_cur_avail] != 0) {
                            data_buf_cur_avail = (data_buf_cur_avail + 1) % 40;
                            loop += 1;
                            if (loop > 35) {
                                fprintf(stderr, "error, exceed concurrent streams\n");
                                exit(1);
                            }
                        }
                        sid_to_data_buf[sid] = data_buf_cur_avail;
                        data_buf_cur_avail = (data_buf_cur_avail + 1) % 40;
                    }

                    int buffer_id = sid_to_data_buf[sid];
                    //int buffer_id = ((s - 1) / 4 - 1) % 40;
                    //fprintf(stderr, "push to data buffer %d, cur pos %d, next pos %ld\n", buffer_id, data_buf_pos[buffer_id], data_buf_pos[buffer_id] + recv_len);
                    if (recv_len != QUICHE_ERR_STREAM_RESET) {
                        memcpy(data_buf[buffer_id] + data_buf_pos[buffer_id], buf, recv_len);
                        data_buf_pos[buffer_id] += recv_len;
                    }
                    if (fin || recv_len == QUICHE_ERR_STREAM_RESET) {
                        uint64_t pipelineId = (s - 9) % (4 * gl_num_pipeline) / 4;
                        frame_cnt += 1;
                        fprintf(stderr, "gst push recv %d bytes on pipeline %lu, stream id %ld\n",
                                data_buf_pos[buffer_id], pipelineId, s);
                        gstreamer_receive_push_buffer(gl_pipeline, data_buf[buffer_id], data_buf_pos[buffer_id],
                                                      pipelineId);
                        data_buf_pos[buffer_id] = 0;
                    }
                }*/
                // send back an ack in application layer.
//                static const uint8_t echo[] = "echo\n";
//                if (quiche_conn_stream_send(conn_io->conn, 8, echo, sizeof(echo), false) < sizeof(echo)) {
//                    fprintf(stderr, "fail to echo back.\n");
//                }
                //flush_egress(conn_io);
            }
            //fprintf(stderr, "%ld end iter stream \n", getcurTime());
            quiche_stream_iter_free(readable);
        }
    }

    flush_egress(conn_io);
    if (gl_app_type == APP_H264_DATA) {
        g_mutex_unlock(gl_mutex);
    }
    return TRUE;
}

void generate_pipeline_str(char * ppl_str){
    /* calculate the windows size for each stream*/
    int pos = 0;
    int small_window_height = 0;
    int small_window_width = 0;
    int gap_pixels = 0;
    if (gl_num_pipeline > 1) {
        gap_pixels = 10;
        small_window_height = (int) ((MAIN_WINDOW_HEIGHT - (gl_num_pipeline - 2) * gap_pixels) / (gl_num_pipeline - 1));
        small_window_width = (int) (small_window_height * MAIN_WINDOW_WIDTH/ MAIN_WINDOW_HEIGHT);
    }


    //for main window
    pos = sprintf(ppl_str, "compositor name=comp sink_0::alpha=1 sink_0::xpos=0 sink_0::ypos=0 sink_0::width=%d sink_0::height=%d ", MAIN_WINDOW_WIDTH, MAIN_WINDOW_HEIGHT);

    //small window pos
    for(int i = 1; i < gl_num_pipeline; i++) {
        int x_pos = MAIN_WINDOW_WIDTH;
        int y_pos = (small_window_height + gap_pixels) * (i - 1);
        pos += sprintf(ppl_str + pos, "sink_%d::alpha=1 sink_%d::xpos=%d sink_%d::ypos=%d sink_%d::width=%d sink_%d::height=%d ",
                       i, i, x_pos, i, y_pos, i, small_window_width, i, small_window_height);
    }

    pos += sprintf(ppl_str + pos, "! queue2 ! videoconvert ! ximagesink ");
    //pos += sprintf(ppl_str + pos, "! videoconvert ! filesink location=./video_src/recv.mp4 ");
    //add appsrc
    for(int i = 0; i < gl_num_pipeline; i++) {
        pos += sprintf(ppl_str + pos, "%s ! videoconvert ! comp.sink_%d ",
                       gl_pipeline_infos[i].pipeline, i);
    }

    //check pipeline
    printf("-------pipeline------\n%s\n", ppl_str);
}


void generate_pipeline_str_save_to_file(char * ppl_str) {
    int pos = 0;
    for(int i = 0; i < gl_num_pipeline; i++) {
        pos += sprintf(ppl_str + pos, "%s ! videorate skip-to-first=true max-closing-segment-duplication-duration=10000000000 ! video/x-raw,framerate=25/1 ! matroskamux ! filesink location=./video_src/recv_%d.yuv sync=true ",
                       gl_pipeline_infos[i].pipeline, i);
    }
    //check pipeline
    printf("-------pipeline------\n%s\n", ppl_str);
}

void read_pipeline_conf(char* filename, SampleHandlerUserData * pipeline_infos, int num_pipeline) {
    FILE *fp = NULL;
    if((fp = fopen(filename,"r")) == NULL) {
        printf("conf not exist");
        return;
    }
    char strline[1024];
    for(int i = 0; i < num_pipeline; i++) {
        fgets(strline, 1024, fp);
        strline[strlen(strline) - 1] = '\0';
        strcpy(pipeline_infos[i].pipeline, strline);

        fgets(strline, 1024, fp);
        pipeline_infos[i].pipelineId = strtol(strline, NULL, 10);

        fgets(strline, 1024, fp);
        pipeline_infos[i].priority = strtol(strline, NULL, 10);

        fgets(strline, 1024, fp);
        pipeline_infos[i].deadline_ms = strtol(strline, NULL, 10);

        fgets(strline, 1024, fp);
        pipeline_infos[i].type = strtol(strline, NULL, 10);

        pipeline_infos[i].cur_stream_id = 9 + pipeline_infos[i].pipelineId * 4;
        printf("\n----pipe conf----\n %s\n id: %d prior: %d ddl: %d, type: %d\n",
               pipeline_infos[i].pipeline,
               pipeline_infos[i].pipelineId,
               pipeline_infos[i].priority,
               pipeline_infos[i].deadline_ms,
               pipeline_infos[i].type);
    }
    fclose(fp);
}

int main(int argc, char *argv[]) {
    /* read input setup */
    const char *host = argv[1];
    const char *port = argv[2];
    sscanf(argv[3], "%d", &gl_use_dgram);
    sscanf(argv[4], "%d", &gl_if_debug);
    sscanf(argv[5], "%d", &gl_app_type);
    if (gl_app_type == APP_SYNTHETIC_DATA) {
        fprintf(stdout, "Running synthetic data app\n");
    }
    else if (gl_app_type == APP_H264_DATA) {
        sscanf(argv[6], "%d", &gl_num_pipeline);
        gl_pipeline_infos = (SampleHandlerUserData *) malloc(gl_num_pipeline * sizeof(SampleHandlerUserData));
        read_pipeline_conf("decode_ppl.txt", gl_pipeline_infos, gl_num_pipeline);
        gl_mutex = (GMutex *) malloc(sizeof(GMutex));
        g_mutex_init(gl_mutex);
    }
    else {
        fprintf(stdout, "App is not defined\n");
        return 0;
    }



    /* init quic connection info*/
    const struct addrinfo hints = {
            .ai_family = PF_UNSPEC,
            .ai_socktype = SOCK_DGRAM,
            .ai_protocol = IPPROTO_UDP
    };

    //quiche_enable_debug_logging(debug_log, NULL);

    struct addrinfo *peer;
    if (getaddrinfo(host, port, &hints, &peer) != 0) {
        perror("failed to resolve host");
        return -1;
    }

    int sock = socket(peer->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("failed to create socket");
        return -1;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        perror("failed to make socket non-blocking");
        return -1;
    }

    quiche_config *config = quiche_config_new(0xbabababa);
    if (config == NULL) {
        fprintf(stderr, "failed to create config\n");
        return -1;
    }

    quiche_config_set_application_protos(config,
                                         (uint8_t *) "\x0ahq-interop\x05hq-29\x05hq-28\x05hq-27\x08http/0.9", 38);

    quiche_config_set_max_idle_timeout(config, 5000);
    quiche_config_set_max_recv_udp_payload_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(config, 1000000000);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(config, 1000000000);
    quiche_config_set_initial_max_stream_data_uni(config, 1000000000);
    quiche_config_set_initial_max_streams_bidi(config, 1000000000);
    quiche_config_set_initial_max_streams_uni(config, 100);
    quiche_config_set_disable_active_migration(config, true);
    quiche_config_enable_dgram(config, true, 1000, 1000);

    if (getenv("SSLKEYLOGFILE")) {
        quiche_config_log_keys(config);
    }

    uint8_t scid[LOCAL_CONN_ID_LEN];
    int rng = open("/dev/urandom", O_RDONLY);
    if (rng < 0) {
        perror("failed to open /dev/urandom");
        return -1;
    }

    ssize_t rand_len = read(rng, &scid, sizeof(scid));
    if (rand_len < 0) {
        perror("failed to create connection ID");
        return -1;
    }

    struct conn_io *conn_io = malloc(sizeof(*conn_io));
    if (conn_io == NULL) {
        fprintf(stderr, "failed to allocate connection IO\n");
        return -1;
    }

    conn_io->local_addr_len = sizeof(conn_io->local_addr);
    if (getsockname(sock, (struct sockaddr *)&conn_io->local_addr,
                    &conn_io->local_addr_len) != 0)
    {
        perror("failed to get local address of socket");
        return -1;
    };

    quiche_conn *conn = quiche_connect(host, (const uint8_t *) scid, sizeof(scid),
                                       (struct sockaddr *) &conn_io->local_addr,
                                       conn_io->local_addr_len,
                                       peer->ai_addr, peer->ai_addrlen, config);

    if (conn == NULL) {
        fprintf(stderr, "failed to create connection\n");
        return -1;
    }

    conn_io->sock = sock;
    conn_io->conn = conn;

    /* main thread waiting for recv IO event*/
    gpointer m_data = conn_io;
    GIOChannel* channel = g_io_channel_unix_new(sock);
    //guint source = g_io_add_watch(channel, G_IO_IN, (GIOFunc) recv_cb, data);
    g_io_add_watch(channel, G_IO_IN, (GIOFunc) recv_cb, m_data);
    g_io_channel_unref(channel);


    if (gl_app_type == APP_H264_DATA) {
        char pipelineStr[8000];
        //generate_pipeline_str(pipelineStr);
        generate_pipeline_str_save_to_file(pipelineStr);
        gl_pipeline = gstreamer_receive_create_pipeline(pipelineStr);
        gstreamer_receive_start_pipeline(gl_pipeline);
    }
    flush_egress(conn_io);

    gstreamer_receive_start_mainloop();

    //gstreamer_receive_destroy_pipeline(gl_pipeline);
    freeaddrinfo(peer);
    quiche_conn_free(conn);
    quiche_config_free(config);

    return 0;
}
