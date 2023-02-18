#ifndef GST_SINK_H
#define GST_SINK_H

#include <glib.h>
#include <gst/gst.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct SampleHandlerUserData {
    char pipeline[512];
    int pipelineId;
    int priority;
    int deadline_ms;
    int type;
    int cur_stream_id;
} SampleHandlerUserData;

void gstreamer_receive_start_mainloop(void);

GstElement *gstreamer_receive_create_pipeline(char *pipeline);
void gstreamer_receive_start_pipeline(GstElement *pipeline);
void gstreamer_receive_stop_pipeline(GstElement* pipeline);
void gstreamer_receive_destroy_pipeline(GstElement* pipeline);
void gstreamer_receive_push_buffer(GstElement *pipeline, void *buffer, int len, uint64_t pipelineId);

//extern void goHandleReceiveEOS();

#endif
