OS := $(shell uname)

SOURCE_DIR = ../src
BUILD_DIR = $(CURDIR)/build
LIB_DIR = $(BUILD_DIR)/debug
INCLUDE_DIR = ../include

INCS = -I$(INCLUDE_DIR)
CFLAGS = -I. -Wall -pedantic -g

ifeq ($(OS), Darwin)
CFLAGS += -framework Security
endif

LIBCRYPTO_DIR = $(dir $(shell find ${BUILD_DIR} -name libcrypto.a))
LIBSSL_DIR = $(dir $(shell find ${BUILD_DIR} -name libssl.a))

LDFLAGS = -L$(LIBCRYPTO_DIR) -L$(LIBSSL_DIR) -L$(LIB_DIR)

LIBS = $(LIB_DIR)/libquiche.a -lev -ldl -pthread -lm

all: client server server_urgency_diff_sche gserver2 gclient2

client: client.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

server: server.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

server_urgency_diff_sche: server_urgency_diff_sche.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

server_urgency_diff_sche_overhead: server_urgency_diff_sche_overhead.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

gclient2: gclient2.c gstsink.c gstsink.h $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) gclient2.c gstsink.c -o $@ $(INCS) $(LIBS) `pkg-config --cflags --libs glib-2.0 gobject-2.0 gtk+-2.0 gstreamer-1.0 gstreamer-app-1.0`

gserver2: gserver2.c gstsrc.c gstsrc.h $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) gserver2.c gstsrc.c -o $@ $(INCS) $(LIBS) `pkg-config --cflags --libs glib-2.0 gobject-2.0 gtk+-2.0 gstreamer-1.0 gstreamer-app-1.0`


http3-client: http3-client.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

http3-server: http3-server.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

$(LIB_DIR)/libquiche.a: $(shell find $(SOURCE_DIR) -type f -name '*.rs')
	cd .. && cargo build --target-dir $(BUILD_DIR) --features ffi

clean:
	@$(RM) -rf client server http3-client server_urgency_diff_sche server_urgency_diff_sche_overhead gserver2 gclient2 srv.log cli.log http3-server build/ *.dSYM/
