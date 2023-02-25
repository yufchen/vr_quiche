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

#include <ev.h>
#include <uthash.h>
#include <string.h>

#include <quiche.h>

#define LOCAL_CONN_ID_LEN 16

#define MAX_DATAGRAM_SIZE 1350

#define MAX_TOKEN_LEN \
    sizeof("quiche") - 1 + \
    sizeof(struct sockaddr_storage) + \
    QUICHE_MAX_CONN_ID_LEN

struct connections {
    int sock;

    struct sockaddr *local_addr;
    socklen_t local_addr_len;

    struct conn_io *h;
};

bool flag = false;
int peace = 0;
FILE *fp = NULL;
int num_times = 0;
bool stop_sending = false;
ssize_t init_retrans = 0;


long getcurTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long curtime = tv.tv_sec *1000000 + tv.tv_usec;
    return curtime;
}

bool record_flag = false;

long gl_start_ts;
long gl_out_ts_prior1[1000];
int gl_out_cnt_prior1 = 0;

long gl_out_ts_prior2[1000];
int gl_out_cnt_prior2 = 0;

long gl_out_ts_prior3[1000];
int gl_out_cnt_prior3 = 0;


struct conn_io {
    ev_timer timer;

    int sock;

    uint8_t cid[LOCAL_CONN_ID_LEN];

    quiche_conn *conn;

    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;

    UT_hash_handle hh;
};

static quiche_config *config = NULL;

static struct connections *conns = NULL;

static void timeout_cb(EV_P_ ev_timer *w, int revents);

static void debug_log(const char *line, void *argp) {
    fprintf(stderr, "%s\n", line);
}

static void flush_egress(struct ev_loop *loop, struct conn_io *conn_io) {
    //fprintf(stderr, "\nflush egress");
    static uint8_t out[MAX_DATAGRAM_SIZE];

    quiche_send_info send_info;

    ssize_t tot_sent1 = 0;
    ssize_t tot_sent2 = 0;
    ssize_t tot_sent3 = 0;

    while (1) {
        long temp_time = getcurTime();
        ssize_t stream_cap_prior1 = quiche_conn_stream_capacity(conn_io->conn, 9);
        ssize_t stream_cap_prior2 = quiche_conn_stream_capacity(conn_io->conn, 13);
        ssize_t stream_cap_prior3 = quiche_conn_stream_capacity(conn_io->conn, 17);

        ssize_t written = quiche_conn_send(conn_io->conn, out, sizeof(out),
                                               &send_info);

        stream_cap_prior1 -= quiche_conn_stream_capacity(conn_io->conn, 9);
        stream_cap_prior2 -= quiche_conn_stream_capacity(conn_io->conn, 13);
        stream_cap_prior3 -= quiche_conn_stream_capacity(conn_io->conn, 17);


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

        if (stream_cap_prior1 > 0 && flag && record_flag) {
            tot_sent1 += sent;
            fprintf(stderr, "cnt: %d dgram sent %zd bytes, total %zd bytes\n", gl_out_cnt_prior1, sent, tot_sent1);
            gl_out_ts_prior1[gl_out_cnt_prior1] = temp_time;
            gl_out_cnt_prior1 += 1;
        }
        else if (stream_cap_prior2 > 0 && flag && record_flag) {
            tot_sent2 += sent;
            fprintf(stderr, "cnt: %d dgram sent %zd bytes, total %zd bytes\n", gl_out_cnt_prior2, sent, tot_sent2);
            gl_out_ts_prior2[gl_out_cnt_prior2] = temp_time;
            gl_out_cnt_prior1 += 1;
        }
        else if (stream_cap_prior3 > 0 && flag && record_flag) {
            tot_sent3 += sent;
            fprintf(stderr, "cnt: %d dgram sent %zd bytes, total %zd bytes\n", gl_out_cnt_prior3, sent, tot_sent3);
            gl_out_ts_prior3[gl_out_cnt_prior3] = temp_time;
            gl_out_cnt_prior1 += 1;
        }

        
    }


    if (record_flag) {
        //record only once
        record_flag = false;
    }
    double t = quiche_conn_timeout_as_nanos(conn_io->conn) / 1e9f;
    conn_io->timer.repeat = t;
    ev_timer_again(loop, &conn_io->timer);
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
                                      config);

    if (conn == NULL) {
        fprintf(stderr, "failed to create connection\n");
        return NULL;
    }

    conn_io->sock = conns->sock;
    conn_io->conn = conn;

    memcpy(&conn_io->peer_addr, peer_addr, peer_addr_len);
    conn_io->peer_addr_len = peer_addr_len;

    ev_init(&conn_io->timer, timeout_cb);
    conn_io->timer.data = conn_io;

    HASH_ADD(hh, conns->h, cid, LOCAL_CONN_ID_LEN, conn_io);

    fprintf(stderr, "new connection\n");

    return conn_io;
}

static void recv_cb(EV_P_ ev_io *w, int revents) {
    struct conn_io *tmp, *conn_io = NULL;
    static bool ready_to_send = false;
    static uint8_t buf[65535];
    static uint8_t out[MAX_DATAGRAM_SIZE];

    while (1) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_addr_len);

        ssize_t read = recvfrom(conns->sock, buf, sizeof(buf), 0,
                                (struct sockaddr *) &peer_addr,
                                &peer_addr_len);

        if (read < 0) {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                //fprintf(stderr, "recv would block\n");
                break;
            }

            perror("failed to read");
            return;
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
            continue;
        }

        HASH_FIND(hh, conns->h, dcid, dcid_len, conn_io);

        if (conn_io == NULL) {
            if (!quiche_version_is_supported(version)) {
                fprintf(stderr, "version negotiation\n");

                ssize_t written = quiche_negotiate_version(scid, scid_len,
                                                           dcid, dcid_len,
                                                           out, sizeof(out));

                if (written < 0) {
                    fprintf(stderr, "failed to create vneg packet: %zd\n",
                            written);
                    continue;
                }

                ssize_t sent = sendto(conns->sock, out, written, 0,
                                      (struct sockaddr *) &peer_addr,
                                      peer_addr_len);
                if (sent != written) {
                    perror("failed to send");
                    continue;
                }

                //fprintf(stderr, "sent %zd bytes\n", sent);
                continue;
            }

            if (token_len == 0) {
                fprintf(stderr, "stateless retry\n");

                mint_token(dcid, dcid_len, &peer_addr, peer_addr_len,
                           token, &token_len);

                uint8_t new_cid[LOCAL_CONN_ID_LEN];

                if (gen_cid(new_cid, LOCAL_CONN_ID_LEN) == NULL) {
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
                    continue;
                }

                ssize_t sent = sendto(conns->sock, out, written, 0,
                                      (struct sockaddr *) &peer_addr,
                                      peer_addr_len);
                if (sent != written) {
                    perror("failed to send");
                    continue;
                }

                //fprintf(stderr, "sent %zd bytes\n", sent);
                continue;
            }


            if (!validate_token(token, token_len, &peer_addr, peer_addr_len,
                               odcid, &odcid_len)) {
                fprintf(stderr, "invalid address validation token\n");
                continue;
            }

            conn_io = create_conn(dcid, dcid_len, odcid, odcid_len,
                                  conns->local_addr, conns->local_addr_len,
                                  &peer_addr, peer_addr_len);

            if (conn_io == NULL) {
                continue;
            }
        }

        quiche_recv_info recv_info = {
            (struct sockaddr *)&peer_addr,
            peer_addr_len,

            conns->local_addr,
            conns->local_addr_len,
        };

        ssize_t done = quiche_conn_recv(conn_io->conn, buf, read, &recv_info);

        if (done < 0) {
            fprintf(stderr, "failed to process packet: %zd\n", done);
            continue;
        }

        //fprintf(stderr, "recv %zd bytes\n", done);

        if (quiche_conn_is_established(conn_io->conn)) {
            uint64_t s = 0;
            quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);

            while (quiche_stream_iter_next(readable, &s)) {
                fprintf(stderr, "stream %" PRIu64 " is readable\n", s);

                bool fin = false;
                ssize_t recv_len = quiche_conn_stream_recv(conn_io->conn, s,
                                                           buf, sizeof(buf),
                                                           &fin);
                fprintf(stdout, "RECV from client: %s", buf);
                if (recv_len < 0) {
                    break;
                }

                if (fin && !ready_to_send) {
                    ready_to_send = true;
                   // static const char resp[40000] = {'\0'};
                    //printf("Stream Capacity : %d\n", (int)quiche_conn_stream_capacity(conn_io->conn, 4));
                    //printf("Sent %d bytes.\n", (int)quiche_conn_stream_send(conn_io->conn, 5, (uint8_t *) resp, 40000, true));
                    //printf("Sent %d bytes.\n", (int)quiche_conn_stream_send(conn_io->conn, 5, (uint8_t *) resp, 40000, true));
                    //flush_egress(loop, conn_io);
                }
            }

            quiche_stream_iter_free(readable);
        }

    }


    HASH_ITER(hh, conns->h, conn_io, tmp) {
        if (ready_to_send && quiche_conn_is_established(conn_io->conn)) {
            //static const char resp[40000] = {'\0'};
            static const char resp2[100000] = {'\0'};
            ssize_t stream_cap = quiche_conn_stream_capacity(conn_io->conn, 4);
            //printf("\nStream Cap: %d\n", stream_cap);

            if (stream_cap > 180000 && !stop_sending){
                quiche_stats stats;
                quiche_conn_stats(conn_io->conn, &stats);
                init_retrans = stats.retrans;

                flag = true;
                record_flag = true;

                stop_sending = true;

                printf("Start sending with priority\n");
                static uint8_t foo_buffer[50000] = {'\0'};
                //urgency level 1  stream_id 9
                int size = quiche_conn_stream_send_full(conn_io->conn, 9, foo_buffer, 50000, true, 0, 1, 0);
                fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d on urgency %d\n", getcurTime(), size, 50000, 9, 1);
                //urgency level 2  stream_id 13
                size = quiche_conn_stream_send_full(conn_io->conn, 13, foo_buffer, 50000, true, 0, 2, 0);
                fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d on urgency %d\n", getcurTime(), size, 50000, 13, 2);
                //urgency level 3  stream_id 17
                size = quiche_conn_stream_send_full(conn_io->conn, 17, foo_buffer, 50000, true, 0, 3, 0);
                fprintf(stderr, "%ld, stream_send %d/%d bytes on stream id %d on urgency %d\n", getcurTime(), size, 50000, 17, 3);
                gl_start_ts = getcurTime();


            }
            if (!flag){
                //printf("Sent %d bytes.\n", (int)quiche_conn_stream_send(conn_io->conn, 4, (uint8_t *) resp2, 10000, false));
                quiche_conn_stream_send(conn_io->conn, 4, (uint8_t *) resp2, 100000, false);
            }
        }

        flush_egress(loop, conn_io);

        if (quiche_conn_is_closed(conn_io->conn)) {
            quiche_stats stats;
            quiche_path_stats path_stats;

            quiche_conn_stats(conn_io->conn, &stats);
            quiche_conn_path_stats(conn_io->conn, 0, &path_stats);

            fprintf(stderr, "connection closed, recv=%zu sent=%zu lost=%zu rtt=%" PRIu64 "ns cwnd=%zu\n",
                    stats.recv, stats.sent, stats.lost, path_stats.rtt, path_stats.cwnd);

            HASH_DELETE(hh, conns->h, conn_io);

            ev_timer_stop(loop, &conn_io->timer);
            quiche_conn_free(conn_io->conn);
            free(conn_io);
        }
    }
}

static void timeout_cb(EV_P_ ev_timer *w, int revents) {
    struct conn_io *conn_io = w->data;
    quiche_conn_on_timeout(conn_io->conn);

    fprintf(stderr, "timeout\n");

    flush_egress(loop, conn_io);

    if (quiche_conn_is_closed(conn_io->conn)) {
        quiche_stats stats;
        quiche_path_stats path_stats;

        quiche_conn_stats(conn_io->conn, &stats);
        quiche_conn_path_stats(conn_io->conn, 0, &path_stats);

        fprintf(stderr, "connection closed, retrans = %zu recv=%zu sent=%zu lost=%zu rtt=%" PRIu64 "ns cwnd=%zu\n",
                stats.retrans, stats.recv, stats.sent, stats.lost, path_stats.rtt, path_stats.cwnd);

        HASH_DELETE(hh, conns->h, conn_io);

        ev_timer_stop(loop, &conn_io->timer);
        quiche_conn_free(conn_io->conn);
        free(conn_io);


        fprintf(fp, "%ld, start time, cnt: %d %d %d\n", gl_start_ts, gl_out_cnt_prior1, gl_out_cnt_prior2, gl_out_cnt_prior3);



        for (int i = 0; i < gl_out_cnt_prior1; i++){
            fprintf(fp, "%ld, stream prior 1 out, cnt: %d\n", gl_out_ts_prior1[i], i);
            fprintf(fp, "%ld, stream prior 2 out, cnt: %d\n", gl_out_ts_prior2[i], i);
            fprintf(fp, "%ld, stream prior 3 out, cnt: %d\n", gl_out_ts_prior3[i], i);
        }

        //do not consider retransmission
//        for (int i = 0; i < gl_stream_out_cnt; i++){
//            fprintf(fp, "%ld, stream out\n", gl_stream_out_ts[i]);
//        }

        fprintf(fp, "Sent %d Datagram frames\n", peace);
        fclose(fp);
        num_times++;

       // fclose(fp);
        //exit(0);

        return;
    }
}

int main(int argc, char *argv[]) {
    const char *host = argv[1];
    const char *port = argv[2];

    flag = false;
    peace = 0;
    stop_sending = false;


    const char *log_file = argv[3];
    fp = fopen(log_file, "w");
    if(fp == NULL)
        exit(0);



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

    config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (config == NULL) {
        fprintf(stderr, "failed to create config\n");
        return -1;
    }

    quiche_config_load_cert_chain_from_pem_file(config, "./cert.crt");
    quiche_config_load_priv_key_from_pem_file(config, "./cert.key");

    quiche_config_set_application_protos(config,
        (uint8_t *) "\x0ahq-interop\x05hq-29\x05hq-28\x05hq-27\x08http/0.9", 38);

    quiche_config_set_max_idle_timeout(config, 5000);
    quiche_config_set_max_recv_udp_payload_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(config, 1000000000);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000000);
    quiche_config_set_initial_max_stream_data_uni(config, 1000000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(config, 1000000000);
    quiche_config_set_initial_max_streams_bidi(config, 1000000000);
    quiche_config_set_cc_algorithm(config, QUICHE_CC_BBR);
    quiche_config_enable_dgram(config, true, 1000000, 1000000);

    struct connections c;
    c.sock = sock;
    c.h = NULL;
    c.local_addr = local->ai_addr;
    c.local_addr_len = local->ai_addrlen;





    conns = &c;

    ev_io watcher;

    struct ev_loop *loop = ev_default_loop(0);

    ev_io_init(&watcher, recv_cb, sock, EV_READ);
    ev_io_start(loop, &watcher);
    watcher.data = &c;

    ev_loop(loop, 0);

    freeaddrinfo(local);

    quiche_config_free(config);

    return 0;
}


