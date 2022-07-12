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

struct conn_io {
    int sock;

    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;

    quiche_conn *conn;
};

GMainLoop *loop;
GstElement* pipeline;
gboolean use_dgram = TRUE;
static GMutex s_mutex;
static GMutex *mutex = &s_mutex;

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
            fprintf(stderr, "done writing\n");
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

        fprintf(stderr, "udp sent %zd bytes\n", sent);
    }
}

static gboolean recv_cb (GIOChannel *channel, GIOCondition condition, gpointer data) {
    g_mutex_lock(mutex);
    static bool req_sent = false;

    struct conn_io *conn_io = data;

    static uint8_t buf[65535];

    while (1) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_addr_len);

        ssize_t read = recvfrom(conn_io->sock, buf, sizeof(buf), 0,
                                (struct sockaddr *) &peer_addr,
                                &peer_addr_len);

        if (read < 0) {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                fprintf(stderr, "recv would block\n");
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
            fprintf(stderr, "failed to process packet\n");
            continue;
        }

        fprintf(stderr, "recv %zd bytes\n", done);
    }

    fprintf(stderr, "done reading\n");

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
        if (use_dgram) {
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
        if (use_dgram && quiche_conn_is_readable(conn_io->conn)) {
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
            printf("gst push recv %ld bytes\n", recv_len);
            gstreamer_receive_push_buffer(pipeline, buf, recv_len);
            //printf("RECV from server: %s", buf);
        }
        else {
            uint64_t s = 0;
            quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);
            while (quiche_stream_iter_next(readable, &s)) {
                fprintf(stderr, "stream %" PRIu64 " is readable\n", s);
                bool fin = false;
                ssize_t recv_len = quiche_conn_stream_recv(conn_io->conn, s, buf, sizeof(buf), &fin);
                printf("recv %ld bytes on stream id %ld\n", recv_len, s);
                if (recv_len < 0) {
                    break;
                }
                if (fin) {
                    printf("gst push recv %ld bytes\n", recv_len);
                    gstreamer_receive_push_buffer(pipeline, buf, recv_len);
                }
                else {
                    printf("--------error, frame size exceeds mtu?--------\n");
                }
            }
            quiche_stream_iter_free(readable);
        }
    }

    flush_egress(conn_io);
    g_mutex_unlock(mutex);
    return TRUE;
}


int main(int argc, char *argv[]) {
    const char *host = argv[1];
    const char *port = argv[2];
    use_dgram = (int) argv[3][0] - (int) '0';
    printf("use_dgram %d\n", use_dgram);

    g_mutex_init(mutex);
    const struct addrinfo hints = {
        .ai_family = PF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP
    };

    quiche_enable_debug_logging(debug_log, NULL);

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
    quiche_config_set_initial_max_data(config, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(config, 10000000);
    quiche_config_set_initial_max_stream_data_uni(config, 1000000);
    quiche_config_set_initial_max_streams_bidi(config, 50000);
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


    gpointer data = conn_io;
    GIOChannel* channel = g_io_channel_unix_new(sock);
    //guint source = g_io_add_watch(channel, G_IO_IN, (GIOFunc) recv_cb, data);
    g_io_add_watch(channel, G_IO_IN, (GIOFunc) recv_cb, data);
    g_io_channel_unref(channel);

    //char pipelineStr[] = "appsrc name=src ! application/x-rtp ! rtpjitterbuffer ! rtph264depay ! decodebin ! videoconvert ! clocksync ! y4menc ! filesink location=recv.yuv";
    char pipelineStr[] = "appsrc name=src ! application/x-rtp ! rtpjitterbuffer ! queue ! rtph264depay ! h264parse ! avdec_h264 ! matroskamux ! filesink location=recv.mp4";
    pipeline = gstreamer_receive_create_pipeline(pipelineStr);
    gstreamer_receive_start_pipeline(pipeline);

    flush_egress(conn_io);

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);


    freeaddrinfo(peer);
    quiche_conn_free(conn);
    quiche_config_free(config);

    return 0;
}
