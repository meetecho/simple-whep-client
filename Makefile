CC = gcc
STUFF = $(shell pkg-config --cflags "gstreamer-webrtc-1.0 >= 1.16" "gstreamer-sdp-1.0 >= 1.16" gstreamer-video-1.0 libsoup-3.0 json-glib-1.0) -D_GNU_SOURCE
STUFF_LIBS = $(shell pkg-config --libs "gstreamer-webrtc-1.0 >= 1.16" "gstreamer-sdp-1.0 >= 1.16" gstreamer-video-1.0 libsoup-3.0 json-glib-1.0)
OPTS = -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wunused #-Werror #-O2
GDB = -g -ggdb
OBJS = src/whep-client.o

all: whep-client

%.o: %.c
	$(CC) $(ASAN) $(STUFF) -fPIC $(GDB) -c $< -o $@ $(OPTS)

whep-client: $(OBJS)
	$(CC) $(GDB) -o whep-client $(OBJS) $(ASAN_LIBS) $(STUFF_LIBS)

clean:
	rm -f whep-client src/*.o
