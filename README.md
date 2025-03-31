Simple WHEP Client
==================

This is a prototype implementation of a [WHEP client](https://datatracker.ietf.org/doc/draft-ietf-wish-whep/), developed by [Meetecho](https://www.meetecho.com). While it's conceived to be used mostly for testing with [Simple WHEP Server](https://github.com/meetecho/simple-whep-server) (based on [Janus](https://github.com/meetecho/janus-gateway/)), as a standard WHEP implementation in theory it should be able to interoperate just as well with other WHEP implementations.

> Note: this is an implementation of WHEP (WebRTC-HTTP egress protocol), **NOT** WHIP (WebRTC-HTTP ingestion protocol). If you're looking for a WHIP client to ingest media in a server, check [Simple WHEP Client](https://github.com/meetecho/simple-whip-client) instead.

# Building the WHEP client

The main dependencies of this client are:

* [pkg-config](http://www.freedesktop.org/wiki/Software/pkg-config/)
* [GLib](http://library.gnome.org/devel/glib/)
* [libsoup](https://wiki.gnome.org/Projects/libsoup) (~= 2.4)
* [GStreamer](https://gstreamer.freedesktop.org/) (>= 1.16)

Make sure the related development versions of the libraries are installed, before attempting to build the client, as to keep things simple the `Makefile` is actually very raw and naive: it makes use of `pkg-config` to detect where the libraries are installed, but if some are not available it will still try to proceed (and will fail with possibly misleading error messages). All of the libraries should be available in most repos (they definitely are on Fedora, which is what I use everyday, and to my knowledge Ubuntu as well).

> Notice that, while the Makefile assumes a Linux build, at least in principle it should be possible to build it on other platforms as well, as it makes us of cross-platform dependencies. In case you're willing to submit enhancements to the Makefile to build the client on Windows and/or MacOS as well, it would be more than welcome.

Once the dependencies are installed, all you need to do to build the WHEP client is to type:

	make

This will create a `whep-client` executable. Trying to launch that without arguments should display a help section:

```
Usage:
  whep-client [OPTION?] -- Simple WHEP client

Help Options:
  -h, --help               Show help options

Application Options:
  -u, --url                Address of the WHEP endpoint (required)
  -t, --token              Authentication Bearer token to use (optional)
  -A, --audio              GStreamer caps to use for audio (optional, required if audio-only)
  -V, --video              GStreamer caps to use for video (optional, required if video-only)
  -n, --no-trickle         Don't trickle candidates, but put them in the SDP offer (default: false)
  -f, --follow-link        Use the Link headers returned by the WHEP server to automatically configure STUN/TURN servers to use (default: false)
  -S, --stun-server        STUN server to use, if any (stun://hostname:port)
  -T, --turn-server        TURN server to use, if any; can be called multiple times (turn(s)://username:password@host:port?transport=[udp,tcp])
  -F, --force-turn         In case TURN servers are provided, force using a relay (default: false)
  -l, --log-level          Logging level (0=disable logging, 7=maximum log level; default: 4)
  -o, --disable-colors     Disable colors in the logging (default: enabled)
  -L, --log-timestamps     Enable logging timestamps (default: disabled)
  -e, --eos-sink-name      GStreamer sink name for EOS signal
  -b, --jitter-buffer      Jitter buffer (latency) to use in RTP, in milliseconds (default: -1, use webrtcbin's default)
```

# Testing the WHEP client

The WHEP client only requires a few arguments, namely the WHEP endpoint to subscribe to (e.g., an endpoint created in the [Simple WHEP Server](https://github.com/meetecho/simple-whep-server)) and the audio and/or video caps of the codecs you expect to receive. If codecs match, incoming streams are automatically created from the negotiation process, and rendered accordingly.

A simple example, that assumes the specified endpoint requires the "verysecret" token via Bearer authorization, is the following:

```
./whep-client -u http://localhost:7090/whep/endpoint/abc123 \
	-A "application/x-rtp,media=audio,encoding-name=opus,clock-rate=48000,encoding-params=(string)2,payload=111" \
	-V "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96" \
	-t verysecret
```

In case, e.g., STUN is needed too, the above command can be extended like this:

```
./whep-client -u http://localhost:7090/whep/endpoint/abc123 \
	-A "application/x-rtp,media=audio,encoding-name=opus,clock-rate=48000,encoding-params=(string)2,payload=111" \
	-V "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96" \
	-t verysecret -S stun://stun.l.google.com:19302
```

You can stop the client via CTRL+C, which will automatically send an HTTP DELETE to the WHEP resource to tear down the session.
