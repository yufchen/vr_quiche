#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "gstsink.h"

#define PORT     23000
#define MAXLINE 2000


int main() {
    //UDP server
    int sockfd;
    char buffer[MAXLINE];
    struct sockaddr_in servaddr, cliaddr;

    // Creating socket file descriptor
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    // Filling server information
    servaddr.sin_family = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    // Bind the socket with the server address
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 ){
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    int len, n;

    len = sizeof(cliaddr);  //len is value/result


    //GStreamer-----------------------------
    //gstreamer_receive_start_mainloop();
    char pipelineStr[] = "appsrc name=src ! application/x-rtp ! rtpjitterbuffer ! rtph264depay ! decodebin ! videoconvert ! clocksync ! y4menc ! filesink location=recv.yuv";
    GstElement* pipeline = gstreamer_receive_create_pipeline(pipelineStr);
    gstreamer_receive_start_pipeline(pipeline);

    printf("start listening\n");
    while (1) {
        n = recvfrom(sockfd, (char *) buffer, MAXLINE,
                     0, (struct sockaddr *) &cliaddr,
                     &len);
        if (memcmp(buffer, "eos", 3) == 0) {
            printf("get eos, end\n");
            gstreamer_receive_stop_pipeline(pipeline);
            gstreamer_receive_destroy_pipeline(pipeline);
            break;
        }
        printf("recv %d bytes\n", n);
        gstreamer_receive_push_buffer(pipeline, buffer, n);
    }

        return 0;
}
