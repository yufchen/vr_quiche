#ifndef GST_SRC_H
#define GST_SRC_H

#include <gst/gst.h>

typedef struct SampleHandlerUserData {
    char pipeline[512];
    int pipelineId;
    int priority;
    int deadline_ms;
    int type;
    int cur_stream_id; //no init
    GMainLoop *gstreamer_send_main_loop; //no init
} SampleHandlerUserData;

extern void goHandleSendEOS(gpointer data);
extern void goHandlePipelineBuffer(void *buffer, int bufferLen, SampleHandlerUserData* s);

//void gstreamer_send_start_mainloop(GMainLoop *gstreamer_send_main_loop);
//void gstreamer_send_quit_mainloop(GMainLoop *gstreamer_send_main_loop);

GstElement* gstreamer_send_create_pipeline(char *pipelineStr);
void gstreamer_send_start_pipeline(GstElement* pipeline, SampleHandlerUserData * pipeline_info);
void gstreamer_send_stop_pipeline(GstElement* pipeline);
void gstreamer_send_destroy_pipeline(GstElement* pipeline);

unsigned int gstreamer_get_property_uint(GstElement* pipeline, char *name, char *prop);
void gstreamer_send_set_property_uint(GstElement* pipeline, char *name, char *prop, unsigned int value);

#endif
