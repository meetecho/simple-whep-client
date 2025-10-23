FROM debian:bookworm AS builder

RUN apt-get update --allow-releaseinfo-change && \
    apt-get install -y --no-install-recommends \
        gdb \
        git \
        build-essential \
        devscripts \
        pkg-config \
        sudo \
        libc6-dev \
        gstreamer1.0-tools \
        gstreamer1.0-nice \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-plugins-good \
        libglib2.0-dev \
        libgstreamer-plugins-bad1.0-dev \
        libsoup-3.0-dev \
        libjson-glib-dev \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-libav && \
        apt-get clean && \
        rm -rf /var/lib/apt/lists/*

WORKDIR /opt

RUN git clone https://github.com/meetecho/simple-whep-client.git && cd simple-whep-client && make

WORKDIR /opt/simple-whep-client

ENV URL=http://localhost:3000/whep/foo

ENTRYPOINT ./whep-client -u $URL -A "application/x-rtp,media=audio,encoding-name=opus,clock-rate=48000,encoding-params=(string)2,payload=111" -V "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96" -S stun://stun.l.google.com:19302
