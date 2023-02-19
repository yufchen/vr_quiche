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
#include <sys/time.h>

#include <gio/gio.h>
#include <glib.h>

#include <uthash.h>

#include <quiche.h>

#include "gstsrc.h"
#define MAX_SEND_TIMES 4
#define MAX_SEND_SIZE 1350*4

#define LOCAL_CONN_ID_LEN 16

#define MAX_DATAGRAM_SIZE 1350

#define GET_BIT(x, bit) ((x >> bit) & 1)

#define MAX_TOKEN_LEN \
    sizeof("quiche") - 1 + \
    sizeof(struct sockaddr_storage) + \
    QUICHE_MAX_CONN_ID_LEN

#define APP_SYNTHETIC_DATA 0
#define APP_H264_DATA 1
#define APP_SYNTHETIC_DATA_PERIOD 2
#define APP_SYNTHETIC_DATA_STATIC_SCHEDULE 3
#define APP_SYNTHETIC_DATA_PERIOD_IF_NEW_STREAM 1
#define SYNTHETIC_DATA_LEN 1000000



/* struct definition*/
struct connections {
    int sock;
    struct sockaddr *local_addr;
    socklen_t local_addr_len;
    struct conn_io *h;
};

struct conn_io {
    int sock;
    uint8_t cid[LOCAL_CONN_ID_LEN];
    quiche_conn *conn;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;

    UT_hash_handle hh;
};

int gl_debug_total_size = 0;

/* Global variables */
GMainLoop *gl_gstreamer_send_main_loop = NULL;
SampleHandlerUserData *gl_pipeline_infos = NULL;
static int gl_use_dgram = -1;
static int gl_if_debug = 1;
static int gl_app_type = -1;
static int gl_num_pipeline = -1;
static int gl_num_streams = -1;
static int gl_num_urgency = -1;
static int gl_app_syn_period_new_stream = -1;
static int gl_urgency_step = -1;


static quiche_config *gl_config = NULL; //quic config
static struct connections *gl_conns = NULL; //!!! when multiple connections, now there is only 1
static GMutex *gl_mutex = NULL;
static GThread **gl_pipeline_threads = NULL;

struct conn_io * gl_recv_conn_io = NULL; //store client info for later use


long getcurTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long curtime = tv.tv_sec *1000000 + tv.tv_usec;
    return curtime;
}


static void debug_log(const char *line, void *argp) {
    fprintf(stderr, "%s\n", line);
}

static void flush_egress(struct conn_io *conn_io, bool is_recv) {
    static uint8_t out[MAX_DATAGRAM_SIZE];
    static int send_times = 0;
    static int send_size = 0;

    quiche_send_info send_info;
    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        if (is_recv) {
            send_times = 0;
            send_size = 0;
        }
        g_mutex_lock(gl_mutex);
    }
    while (1) {
        if (gl_app_type == APP_H264_DATA) {
            if (send_times > MAX_SEND_TIMES && send_size > MAX_SEND_SIZE) {
                break;
            }
        }
        ssize_t written = quiche_conn_send(conn_io->conn, out, sizeof(out), &send_info);

        if (written == QUICHE_ERR_DONE) {
            //fprintf(stderr, "%ld, flush egress done writing\n", getcurTime());
            break;
        }

        if (written < 0) {
            fprintf(stderr, "%ld, flush egress failed to create packet: %zd\n", getcurTime(), written);
            return;
        }


        ssize_t sent = sendto(conn_io->sock, out, written, 0,
                              (struct sockaddr *) &send_info.to,
                              send_info.to_len);

        gl_debug_total_size += sent;
        if (gl_if_debug == 1) {
            fprintf(stderr,
                    "%ld, flush egress written size: %zd bytes, sent size: %zd bytes; total: %d bytes, cnt %d\n",
                    getcurTime(), written, sent, gl_debug_total_size, send_times);
        }
        if (sent != written) {
            //perror("flush egress failed to send");
            break;
        }

        send_times += 1;
        send_size += sent;
    }
    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        g_mutex_unlock(gl_mutex);
    }
}

static void mint_token(const uint8_t *dcid, size_t dcid_len,
                       struct sockaddr_storage *addr, socklen_t addr_len,
                       uint8_t *token, size_t *token_len) {
    memcpy(token, "quiche", sizeof("quiche") - 1);
    memcpy(token + sizeof("quiche") - 1, addr, addr_len);
    memcpy(token + sizeof("quiche") - 1 + addr_len, dcid, dcid_len);

    *token_len = sizeof("quiche") - 1 + addr_len + dcid_len;
}

static bool validate_token(const uint8_t *token, size_t token_len,
                           struct sockaddr_storage *addr, socklen_t addr_len,
                           uint8_t *odcid, size_t *odcid_len) {
    if ((token_len < sizeof("quiche") - 1) ||
         memcmp(token, "quiche", sizeof("quiche") - 1)) {
        return false;
    }

    token += sizeof("quiche") - 1;
    token_len -= sizeof("quiche") - 1;

    if ((token_len < addr_len) || memcmp(token, addr, addr_len)) {
        return false;
    }

    token += addr_len;
    token_len -= addr_len;

    if (*odcid_len < token_len) {
        return false;
    }

    memcpy(odcid, token, token_len);
    *odcid_len = token_len;

    return true;
}

static uint8_t *gen_cid(uint8_t *cid, size_t cid_len) {
    int rng = open("/dev/urandom", O_RDONLY);
    if (rng < 0) {
        perror("failed to open /dev/urandom");
        return NULL;
    }

    ssize_t rand_len = read(rng, cid, cid_len);
    if (rand_len < 0) {
        perror("failed to create connection ID");
        return NULL;
    }

    return cid;
}

static struct conn_io *create_conn(uint8_t *scid, size_t scid_len,
                                   uint8_t *odcid, size_t odcid_len,
                                   struct sockaddr *local_addr,
                                   socklen_t local_addr_len,
                                   struct sockaddr_storage *peer_addr,
                                   socklen_t peer_addr_len)
{
    struct conn_io *conn_io = calloc(1, sizeof(*conn_io));
    if (conn_io == NULL) {
        fprintf(stderr, "failed to allocate connection IO\n");
        return NULL;
    }

    if (scid_len != LOCAL_CONN_ID_LEN) {
        fprintf(stderr, "failed, scid length too short\n");
    }

    memcpy(conn_io->cid, scid, LOCAL_CONN_ID_LEN);

    quiche_conn *conn = quiche_accept(conn_io->cid, LOCAL_CONN_ID_LEN,
                                      odcid, odcid_len,
                                      local_addr,
                                      local_addr_len,
                                      (struct sockaddr *) peer_addr,
                                      peer_addr_len,
                                      gl_config);

    if (conn == NULL) {
        fprintf(stderr, "failed to create connection\n");
        return NULL;
    }

    conn_io->sock = gl_conns->sock;
    conn_io->conn = conn;

    memcpy(&conn_io->peer_addr, peer_addr, peer_addr_len);
    conn_io->peer_addr_len = peer_addr_len;

    HASH_ADD(hh, gl_conns->h, cid, LOCAL_CONN_ID_LEN, conn_io);

    fprintf(stderr, "new connection\n");

    return conn_io;
}

void pipeline_th_call(gpointer data) {
    if (gl_app_type == APP_H264_DATA) {
        SampleHandlerUserData *rtp_stream_info = (SampleHandlerUserData *) data;
        //g_print(rtp_stream_info->pipeline);
        if (rtp_stream_info->pipelineId == 1) {
            //printf("sleep on %d %d\n",rtp_stream_info->pipelineId, rtp_stream_info->priority);
            //int rand_sleep_ms = rand() % 2000 + 10;
            int rand_sleep_ms = 2000;
            usleep(rand_sleep_ms * 1000);
        }

        GstElement *mypipeline = gstreamer_send_create_pipeline(rtp_stream_info->pipeline);
        gstreamer_send_start_pipeline(mypipeline, rtp_stream_info);

        rtp_stream_info->gstreamer_send_main_loop = g_main_loop_new(NULL, FALSE);
        g_main_loop_run(rtp_stream_info->gstreamer_send_main_loop);

        gstreamer_send_destroy_pipeline(mypipeline);

        fprintf(stderr, "Hello, World!\n");
    }
    else if (gl_app_type == APP_SYNTHETIC_DATA_PERIOD) {
        //send data every 1000 ms, send data 100 times max
        static int sleep_ms = 1000;
        static uint8_t foo_buffer[SYNTHETIC_DATA_LEN];
        int data_len_per_stream = SYNTHETIC_DATA_LEN / gl_num_streams;
        int cur_stream_id = 9;
        for (int k = 1; k <= 100; k++) {
            g_mutex_lock(gl_mutex);
            for (int i = 0; i < gl_num_streams; i++) {
                if (gl_app_syn_period_new_stream == APP_SYNTHETIC_DATA_PERIOD_IF_NEW_STREAM) {
                    int size = quiche_conn_stream_send(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, data_len_per_stream, true);
                    if (gl_if_debug) {
                        fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d\n", getcurTime(), size,
                                data_len_per_stream, cur_stream_id);
                    }
                    cur_stream_id += 4;
                }
                else {
                    //send in the same streams
                    int size;
                    if (k != 100) {
                        size = quiche_conn_stream_send(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, data_len_per_stream, false);
                    }
                    else {
                        size = quiche_conn_stream_send(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, data_len_per_stream, true);
                    }
                    if (gl_if_debug) {
                        fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d\n", getcurTime(), size,
                                data_len_per_stream, cur_stream_id);
                    }
                    cur_stream_id += 4;
                }
            }
            g_mutex_unlock(gl_mutex);

            flush_egress(gl_recv_conn_io, false);
            usleep(sleep_ms * 1000);

            if (gl_app_syn_period_new_stream != APP_SYNTHETIC_DATA_PERIOD_IF_NEW_STREAM) {
                cur_stream_id = 9;
            }
        }

    }

    else if (gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        //TODO: static schedule
        //10000 streams sends through
        //send data every 1000 ms, send data 100 sec
        static int sleep_ms = 1000;
        static uint8_t foo_buffer[SYNTHETIC_DATA_LEN];
        int data_len_per_stream = SYNTHETIC_DATA_LEN / gl_num_streams;
        int cur_stream_id = 9;
        int urgency = gl_num_urgency;
        for (int k = 1; k <= 100; k++) {
            g_mutex_lock(gl_mutex);
            for (int i = 0; i < gl_num_streams; i++) {
                //send in the same streams
                int size;
                if (k != 100) {
                    size = quiche_conn_stream_send_full(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, data_len_per_stream, false, 0, urgency, 0);
                }
                else {
                    size = quiche_conn_stream_send_full(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, data_len_per_stream, true, 0, urgency, 0);
                }

                if (gl_if_debug) {
                    fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d, static prior: %d\n", getcurTime(), size,
                            data_len_per_stream, cur_stream_id, urgency);
                }
                cur_stream_id += 4;
                urgency += 1;
            }
            g_mutex_unlock(gl_mutex);
            flush_egress(gl_recv_conn_io, false);
            usleep(sleep_ms * 1000);

            cur_stream_id = 9;
        }
    }
}

void start_th_pipelines() {
    if (gl_app_type == APP_H264_DATA) {
        //printf("current tid: %d\n", gettid());
        /* init multi thread, create thread for each pipeline
         * mutex is needed, only one thread can read/write to quiche API */
        gl_pipeline_threads = (GThread **) malloc(gl_num_pipeline * sizeof(GThread * ));
        GError *th_error = NULL;
        for (int i = 0; i < gl_num_pipeline; i++) {
            gl_pipeline_threads[i] = g_thread_try_new(NULL, (GThreadFunc) pipeline_th_call,
                                                      &gl_pipeline_infos[i], &th_error);
            if (gl_pipeline_threads[i] == NULL) {
                g_critical("Create thread error: %s\n", th_error->message);
                g_error_free(th_error);
            }
            printf("thread created %d\n", i);
        }
    }
    else if (gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        gl_pipeline_threads = (GThread **) malloc(1 * sizeof(GThread * ));
        GError *th_error = NULL;
        for (int i = 0; i < 1; i++) {
            gl_pipeline_threads[i] = g_thread_try_new(NULL, (GThreadFunc) pipeline_th_call,
                                                      &gl_pipeline_infos[i], &th_error);
            if (gl_pipeline_threads[i] == NULL) {
                g_critical("Create thread error: %s\n", th_error->message);
                g_error_free(th_error);
            }
            printf("thread created %d\n", i);
        }
    }
}

static gboolean recv_cb (GIOChannel *channel, GIOCondition condition, gpointer data) {
    //fprintf(stderr, "%ld, recv cb\n", getcurTime());
    static bool ready_to_send = false;
    static bool is_sending = false;

    struct conn_io *tmp, *conn_io = NULL;

    static uint8_t buf[65535];
    static uint8_t out[MAX_DATAGRAM_SIZE];

    while (1) {
        if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
            g_mutex_lock(gl_mutex);
        }

        //fprintf(stderr, "%ld, recv cb get lock\n", getcurTime());
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_addr_len);

        ssize_t read = recvfrom(gl_conns->sock, buf, sizeof(buf), 0,
                                (struct sockaddr *) &peer_addr,
                                &peer_addr_len);
        if (read < 0) {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                //fprintf(stderr, "recv would block\n");
                if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                    g_mutex_unlock(gl_mutex);
                }
                break;
            }

            perror("failed to read");
            if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                g_mutex_unlock(gl_mutex);
            }
            return FALSE;
        }

        uint8_t type;
        uint32_t version;

        uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
        size_t scid_len = sizeof(scid);

        uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
        size_t dcid_len = sizeof(dcid);

        uint8_t odcid[QUICHE_MAX_CONN_ID_LEN];
        size_t odcid_len = sizeof(odcid);

        uint8_t token[MAX_TOKEN_LEN];
        size_t token_len = sizeof(token);

        int rc = quiche_header_info(buf, read, LOCAL_CONN_ID_LEN, &version,
                                    &type, scid, &scid_len, dcid, &dcid_len,
                                    token, &token_len);
        if (rc < 0) {
            fprintf(stderr, "failed to parse header: %d\n", rc);
            if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                g_mutex_unlock(gl_mutex);
            }
            continue;
        }

        HASH_FIND(hh, gl_conns->h, dcid, dcid_len, conn_io);

        if (conn_io == NULL) {
            if (!quiche_version_is_supported(version)) {
                fprintf(stderr, "version negotiation\n");

                ssize_t written = quiche_negotiate_version(scid, scid_len,
                                                           dcid, dcid_len,
                                                           out, sizeof(out));

                if (written < 0) {
                    fprintf(stderr, "failed to create vneg packet: %zd\n",
                            written);
                    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                        g_mutex_unlock(gl_mutex);
                    }
                    continue;
                }

                ssize_t sent = sendto(gl_conns->sock, out, written, 0,
                                      (struct sockaddr *) &peer_addr,
                                      peer_addr_len);
                if (sent != written) {
                    perror("failed to send");
                    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                        g_mutex_unlock(gl_mutex);
                    }
                    continue;
                }
                //fprintf(stderr, "%ld, sent %zd bytes\n", getcurTime(), sent);
                if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                    g_mutex_unlock(gl_mutex);
                }
                continue;
            }

            if (token_len == 0) {
                fprintf(stderr, "stateless retry\n");

                mint_token(dcid, dcid_len, &peer_addr, peer_addr_len,
                           token, &token_len);

                uint8_t new_cid[LOCAL_CONN_ID_LEN];

                if (gen_cid(new_cid, LOCAL_CONN_ID_LEN) == NULL) {
                    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                        g_mutex_unlock(gl_mutex);
                    }
                    continue;
                }

                ssize_t written = quiche_retry(scid, scid_len,
                                               dcid, dcid_len,
                                               new_cid, LOCAL_CONN_ID_LEN,
                                               token, token_len,
                                               version, out, sizeof(out));

                if (written < 0) {
                    fprintf(stderr, "failed to create retry packet: %zd\n",
                            written);
                    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                        g_mutex_unlock(gl_mutex);
                    }
                    continue;
                }

                ssize_t sent = sendto(gl_conns->sock, out, written, 0,
                                      (struct sockaddr *) &peer_addr,
                                      peer_addr_len);
                if (sent != written) {
                    perror("failed to send");
                    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                        g_mutex_unlock(gl_mutex);
                    }
                    continue;
                }
                //fprintf(stderr, "%ld, sent %zd bytes\n", getcurTime(), sent);
                if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                    g_mutex_unlock(gl_mutex);
                }
                continue;
            }


            if (!validate_token(token, token_len, &peer_addr, peer_addr_len,
                               odcid, &odcid_len)) {
                fprintf(stderr, "invalid address validation token\n");
                if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                    g_mutex_unlock(gl_mutex);
                }
                continue;
            }

            conn_io = create_conn(dcid, dcid_len, odcid, odcid_len,
                                  gl_conns->local_addr, gl_conns->local_addr_len,
                                  &peer_addr, peer_addr_len);

            if (conn_io == NULL) {
                if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                    g_mutex_unlock(gl_mutex);
                }
                continue;
            }
        }

        quiche_recv_info recv_info = {
            (struct sockaddr *)&peer_addr,
            peer_addr_len,

            gl_conns->local_addr,
            gl_conns->local_addr_len,
        };

        //process ACK, update rtt
        ssize_t done = quiche_conn_recv(conn_io->conn, buf, read, &recv_info);

        if (done < 0) {
            fprintf(stderr, "failed to process packet: %zd\n", done);
            if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                g_mutex_unlock(gl_mutex);
            }
            continue;
        }
        //fprintf(stderr, "%ld, recv %zd bytes\n", getcurTime(), done);

        if (quiche_conn_is_established(conn_io->conn)) {
            if (gl_use_dgram && quiche_conn_is_readable(conn_io->conn)) {
                //for dgram recv
                ssize_t recv_len = quiche_conn_dgram_recv(conn_io->conn, buf, sizeof(buf));
                printf("RECV from client: %s", buf);
                if (recv_len < 0) {
                    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                        g_mutex_unlock(gl_mutex);
                    }
                    return FALSE;
                }
                ready_to_send = true;
                gl_recv_conn_io = conn_io;
            }
            else {
                //for stream recv
                uint64_t s = 0;
                quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);
                while (quiche_stream_iter_next(readable, &s)) {
                    //fprintf(stderr, "stream %" PRIu64 " is readable\n", s);
                    bool fin = false;
                    ssize_t recv_len = quiche_conn_stream_recv(conn_io->conn, s, buf, sizeof(buf), &fin);
                    fprintf(stdout, "RECV from client: %s", buf);
                    if (recv_len < 0) {
                        if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                            g_mutex_unlock(gl_mutex);
                        }
                        fprintf(stdout, "recv len <0");
                        return FALSE;
                    }
                    if (fin && !ready_to_send) {
                        ready_to_send = true;
                        gl_recv_conn_io = conn_io;

                        // For SYN APP
                        if (gl_app_type == APP_SYNTHETIC_DATA) {
                            //TODO: send same data on different streams
                            int cur_stream_id = 9;
                            static uint8_t foo_buffer[100]; //50MB
                            int data_len_per_stream = 100 / gl_num_streams;
                            for (int k = 1; k <= 3; k++) { // k: current urgency level
                                for (int i = 0; i < gl_num_streams; i++) {
                                    int size = quiche_conn_stream_send_full(gl_recv_conn_io->conn, cur_stream_id,
                                                                            foo_buffer, data_len_per_stream, true, 0,
                                                                            k, cur_stream_id);
                                    //int size = quiche_conn_stream_send(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, data_len_per_stream, true);
                                    if (gl_if_debug) {
                                        fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d on urgency %d\n",
                                                getcurTime(),
                                                size, data_len_per_stream, cur_stream_id, k);
                                    }
                                    cur_stream_id += 4;
                                }
                            }
                        }
                        else if (gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
                            int cur_stream_id = 44009;
                            int urgency = 1;
                            static uint8_t foo_buffer[10];
                            for (int i = 0; i < gl_num_urgency; i++) {
                                int size = quiche_conn_stream_send_full(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, 10, true, 0, urgency, 0);
                                //int size = quiche_conn_stream_send(gl_recv_conn_io->conn, cur_stream_id, foo_buffer, 10, true);
                                if (gl_if_debug) {
                                    fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d\n", getcurTime(),
                                            size, 10, cur_stream_id);
                                }
                                cur_stream_id += 4;
                                urgency += 1;
                            }

                        }

                    }
                }
                quiche_stream_iter_free(readable);
            }

        }
        if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
            g_mutex_unlock(gl_mutex);
        }
    }

    HASH_ITER(hh, gl_conns->h, conn_io, tmp) {
        flush_egress(conn_io, true);//send ack frame, etc
        if (quiche_conn_is_closed(conn_io->conn)) {
            quiche_stats stats;

            quiche_conn_stats(conn_io->conn, &stats);
            fprintf(stderr, "connection closed, recv=%zu sent=%zu lost=%zu rtt=%" PRIu64 "ns cwnd=%zu\n",
                    stats.recv, stats.sent, stats.lost, stats.paths[0].rtt, stats.paths[0].cwnd);

            HASH_DELETE(hh, gl_conns->h, conn_io);
            quiche_conn_free(conn_io->conn);
            free(conn_io);
        }
    }

    //make sure the static buffer in flush_egress is init by the main thread
    if (gl_app_type == APP_H264_DATA) {
        if (ready_to_send && !is_sending) {
            printf("starting all pipelines\n");
            is_sending = true;
            //gstreamer_send_start_pipeline(pipeline, 0);
            start_th_pipelines();
            printf("all pipelines started\n");
        }
    }
    else if (gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        if (ready_to_send && !is_sending) {
            printf("APP_SYNTHETIC_DATA_PERIOD starting sending data every 1 sec\n");
            is_sending = true;
            start_th_pipelines();
            printf("all pipelines started\n");
        }
    }
    return TRUE;
}

void goHandlePipelineBuffer(void *buffer, int bufferLen, SampleHandlerUserData* s) {
    if (s->pipelineId >= 0 && s->pipelineId < gl_num_pipeline) {
        //copy buffer to somewhere else
        if (quiche_conn_is_established(gl_recv_conn_io->conn)) {
            if (gl_use_dgram) {
                quiche_conn_dgram_send(gl_recv_conn_io->conn, (uint8_t *) buffer, bufferLen);
                printf("dgram_send %d bytes\n", bufferLen);
                //flush_egress(gl_recv_conn_io);
            }
            else {
                //increase stream id
                //static int cur_stream_id = 5;
                //check rtp header marker bit
                //unsigned char * tmp = (unsigned char *) buffer;
                //int size = quiche_conn_stream_send(gl_recv_conn_io->conn, s->cur_stream_id, (uint8_t *) buffer, bufferLen, true);
                int deadline_ms = s->deadline_ms;
                int priority = s->priority;
                int depend_id = s->cur_stream_id;
                if (gl_app_type == APP_H264_DATA) {
                    g_mutex_lock(gl_mutex);
                }
                unsigned char * tmp = (unsigned char *) buffer;
//                if (tmp[12] == 0x67) {
//                    //meta data
//                    deadline_ms = 1000;
//                    priority = 0;
//                }
//                else if (tmp[12] == 0x68) {
//                    //meta data
//                    deadline_ms = 1000;
//                    priority = 1;
//                }
//                else if (tmp[12] == 0x06) {
//                    //meta data
//                    deadline_ms = 1000;
//                    priority = 2;
//                }
//                else if (tmp[12] == 0x65) {
//                    priority += 10;
//                }
//                else {
//                    priority += 20;
//                }

                if (tmp[12] == 0x67){
                    //SPS
                }
                else if (tmp[12] == 0x68 || tmp[12] == 0x65 || tmp[12] == 0x61) {
                    //PPS I-frame P-frame
                    depend_id = s->cur_stream_id - 4 * gl_num_pipeline;
                }


                //deadline_ms = 200;
                //priority = s->cur_stream_id * 10;
                int size = quiche_conn_stream_send_full(gl_recv_conn_io->conn, s->cur_stream_id, (uint8_t *) buffer, bufferLen, true, deadline_ms, priority, depend_id);
                if (gl_app_type == APP_H264_DATA) {
                    g_mutex_unlock(gl_mutex);
                }
                fprintf(stderr, "%ld, pipeline %d stream_send %d/%d bytes on stream id %d, ddl %d, prior %d\n", getcurTime(), s->pipelineId, size, bufferLen, s->cur_stream_id, deadline_ms, priority);
                s->cur_stream_id += 4 * gl_num_pipeline;
                flush_egress(gl_recv_conn_io, false);
            }
        }
    }
    //fprintf(stderr, "free buffer %ld\n", getcurTime());

    free(buffer);
    buffer = NULL;
    //
}

void goHandleSendEOS(gpointer data) {
//    char *hello = "eos";
//    sendto(sockfd, (const char *)hello, strlen(hello),
//           0, (const struct sockaddr *) &servaddr,
//           sizeof(servaddr));

    static int eos_cnt = 0;
    eos_cnt += 1;
    fprintf(stderr, "eos cnt %d/%d\n", eos_cnt, gl_num_pipeline);
    SampleHandlerUserData * rtp_stream_info = (SampleHandlerUserData * )data;
    fprintf(stderr, "pipline id: %d\n", rtp_stream_info->pipelineId);
    if (eos_cnt == gl_num_pipeline) {
        uint8_t *buffer = "eos";
        if (quiche_conn_is_established(gl_recv_conn_io->conn)) {
            //quiche_conn_stream_send(recv_conn_io->conn, s, (uint8_t *)resp, 5, true);
            int size = quiche_conn_stream_send(gl_recv_conn_io->conn, rtp_stream_info->cur_stream_id, buffer, 3, true);
            fprintf(stderr, "EOS: %ld, pipeline %d stream_send %d/%d bytes on stream id %d\n", getcurTime(), rtp_stream_info->pipelineId, size, 3, rtp_stream_info->cur_stream_id);
            flush_egress(gl_recv_conn_io, false);
        }
        fprintf(stderr, "eos sent.\n");
    }
    g_main_loop_quit(rtp_stream_info->gstreamer_send_main_loop);

    //TODO: quit mainloop

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

        pipeline_infos[i].gstreamer_send_main_loop = NULL;

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
        sscanf(argv[6], "%d", &gl_num_streams);
        sscanf(argv[7], "%d", &gl_urgency_step);
        fprintf(stdout, "Running synthetic data app, num_streams: %d\n", gl_num_streams);
    }
    else if (gl_app_type == APP_H264_DATA) {
        sscanf(argv[6], "%d", &gl_num_pipeline);
        fprintf(stdout, "Running H264 video app, num_pipeline: %d\n", gl_num_pipeline);
        gl_pipeline_infos = (SampleHandlerUserData *) malloc(gl_num_pipeline * sizeof(SampleHandlerUserData));
        read_pipeline_conf("ppl.txt", gl_pipeline_infos, gl_num_pipeline);
        gl_mutex = (GMutex *) malloc(sizeof(GMutex));
        g_mutex_init(gl_mutex);
    }
    else if (gl_app_type == APP_SYNTHETIC_DATA_PERIOD) {
        sscanf(argv[6], "%d", &gl_num_streams);
        sscanf(argv[7], "%d", &gl_app_syn_period_new_stream);
        fprintf(stdout, "Running synthetic data period app, num_streams: %d, if_new_stream: %d\n", gl_num_streams, gl_app_syn_period_new_stream);
        gl_mutex = (GMutex *) malloc(sizeof(GMutex));
        g_mutex_init(gl_mutex);
    }
    else if (gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        sscanf(argv[6], "%d", &gl_num_streams);
        sscanf(argv[7], "%d", &gl_num_urgency);
        fprintf(stdout, "Running synthetic data static schedule app, num_streams: %d, num_urgency: %d\n", gl_num_streams, gl_num_urgency);
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
    struct addrinfo *local;
    if (getaddrinfo(host, port, &hints, &local) != 0) {
        perror("failed to resolve host");
        return -1;
    }

    int sock = socket(local->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("failed to create socket");
        return -1;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        perror("failed to make socket non-blocking");
        return -1;
    }

    if (bind(sock, local->ai_addr, local->ai_addrlen) < 0) {
        perror("failed to connect socket");
        return -1;
    }

    gl_config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (gl_config == NULL) {
        fprintf(stderr, "failed to create gl_config\n");
        return -1;
    }

    quiche_config_load_cert_chain_from_pem_file(gl_config, "./cert.crt");
    quiche_config_load_priv_key_from_pem_file(gl_config, "./cert.key");

    quiche_config_set_application_protos(gl_config,
        (uint8_t *) "\x0ahq-interop\x05hq-29\x05hq-28\x05hq-27\x08http/0.9", 38);

    quiche_config_set_max_idle_timeout(gl_config, 5000);
    quiche_config_set_max_recv_udp_payload_size(gl_config, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(gl_config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(gl_config, 1000000000);
    quiche_config_set_initial_max_stream_data_bidi_local(gl_config, 1000000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(gl_config, 1000000000);
    quiche_config_set_initial_max_streams_bidi(gl_config, 1000000000);
    //quiche_config_set_cc_algorithm(gl_config, QUICHE_CC_RENO);
    quiche_config_set_cc_algorithm(gl_config, QUICHE_CC_BBR);
    quiche_config_enable_dgram(gl_config, true, 1000, 1000);
    //quiche_config_set_scheduler_name(gl_config, "DTP");
    quiche_config_set_scheduler_name(gl_config, "DTP");
    //!!! only use one connection here
    struct connections c;
    c.sock = sock;
    c.h = NULL;
    c.local_addr = local->ai_addr;
    c.local_addr_len = local->ai_addrlen;
    gl_conns = &c;


//    /* init multi thread, create thread for each pipeline
//     * mutex is needed, only one thread can read/write to quiche API */
//    gl_mutex = (GMutex *) malloc(sizeof(GMutex));
//    gl_pipeline_threads = (GThread **) malloc(gl_num_pipeline * sizeof(GThread *));
//    g_mutex_init(gl_mutex);
//
//    GError* th_error = NULL;
//    for (int i = 0; i < gl_num_pipeline; i++) {
//        gl_pipeline_threads[i] = g_thread_try_new(NULL, (GThreadFunc)pipeline_th_call,
//                                                  &gl_pipeline_infos[i], &th_error);
//        if (gl_pipeline_threads[i] == NULL) {
//            g_critical("Create thread error: %s\n",th_error->message);
//            g_error_free(th_error);
//        }
//    }


    /* main thread waiting for recv IO event*/
    gpointer m_data = NULL;
    GIOChannel* channel = g_io_channel_unix_new(sock);
    g_io_add_watch(channel, G_IO_IN, (GIOFunc) recv_cb, m_data);
    g_io_channel_unref(channel);


    /* start main thread */
    gl_gstreamer_send_main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(gl_gstreamer_send_main_loop);

    //gstreamer create pipeline
    //char pipelineStr[] = "filesrc location=small.mp4 ! decodebin ! videoconvert ! x264enc name=x264enc pass=5 speed-preset=4 bitrate=1000000 tune=4 ! rtph264pay name=rtph264pay mtu=1000 ! appsink name=appsink";

    //pipeline = gstreamer_send_create_pipeline(pipelineStr);


    //loop = g_main_loop_new(NULL, FALSE);
    //g_main_loop_run(loop);


    //gstreamer_send_destroy_pipeline();


    freeaddrinfo(local);
    quiche_config_free(gl_config);
    if (gl_app_type == APP_H264_DATA || gl_app_type == APP_SYNTHETIC_DATA_PERIOD || gl_app_type == APP_SYNTHETIC_DATA_STATIC_SCHEDULE) {
        free(gl_mutex);
        g_mutex_clear(gl_mutex);
    }
    return 0;
}
