#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "gstsrc.h"

#define PORT     23000
#define MAXLINE 2000

int sockfd;
struct sockaddr_in servaddr;
GstElement* mypipeline;

void goHandlePipelineBuffer(void *buffer, int bufferLen, int pipelineId) {
    if (pipelineId == 0) {
        //copy buffer to somewhere else
        sendto(sockfd, buffer, bufferLen, 0, (const struct sockaddr *) &servaddr, sizeof(servaddr));
        printf("send %d bytes\n", bufferLen);
    }
    free(buffer);
}

void goHandleSendEOS() {
    char *hello = "eos";
    sendto(sockfd, (const char *)hello, strlen(hello),
           0, (const struct sockaddr *) &servaddr,
           sizeof(servaddr));
    printf("eos sent.\n");
}

int main() {
    //UDP clinet

    // Creating socket file descriptor
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));

    // Filling server information
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = INADDR_ANY;

    int n, len;

//    char *hello = "Hello from client";
//    sendto(sockfd, (const char *)hello, strlen(hello),
//           0, (const struct sockaddr *) &servaddr,
//           sizeof(servaddr));
//    printf("Hello message sent.\n");

    char pipelineStr[] = "filesrc location=small.mp4 ! decodebin ! videoconvert ! x264enc name=x264enc pass=5 speed-preset=4 bitrate=1000000 tune=4 ! rtph264pay name=rtph264pay mtu=1000 ! appsink name=appsink";
    mypipeline = gstreamer_send_create_pipeline(pipelineStr);
    gstreamer_send_start_pipeline(mypipeline, 0);

    gstreamer_send_start_mainloop();

    gstreamer_send_destroy_pipeline(mypipeline);

    printf("Hello, World!\n");

    return 0;
}
