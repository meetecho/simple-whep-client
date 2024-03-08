/*
 * Simple WHEP client
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: GPLv3
 *
 * Based on webrtc-sendrecv.c, which is released under a BSD 2-Clause
 * License and Copyright(c) 2017, Centricular:
 * https://github.com/centricular/gstwebrtc-demos/blob/master/sendrecv/gst/webrtc-sendrecv.c
 *
 */

/* Generic includes */
#include <signal.h>
#include <string.h>
#include <inttypes.h>

/* GStreamer */
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

/* HTTP stack (WHEP API) */
#include <libsoup/soup.h>

/* Local includes */
#include "debug.h"


/* Logging */
int whep_log_level = LOG_INFO;
gboolean whep_log_timestamps = FALSE;
gboolean whep_log_colors = TRUE, disable_colors = FALSE;

/* State management */
enum whep_state {
	WHEP_STATE_DISCONNECTED = 0,
	WHEP_STATE_CONNECTING = 1,
	WHEP_STATE_CONNECTION_ERROR,
	WHEP_STATE_CONNECTED,
	WHEP_STATE_SUBSCRIBING,
	WHEP_STATE_OFFER_PREPARED,
	WHEP_STATE_STARTED,
	WHEP_STATE_API_ERROR,
	WHEP_STATE_ERROR
};

/* Global properties */
static GMainLoop *loop = NULL;
static GstElement *pipeline = NULL, *pc = NULL;
static const char *audio_caps = NULL, *video_caps = NULL;
static gboolean no_trickle = FALSE, gathering_done = FALSE,
	follow_link = FALSE, force_turn = FALSE;
static const char *stun_server = NULL, **turn_server = NULL;
static char *auto_stun_server = NULL, **auto_turn_server = NULL;
static int latency = -1;

/* API properties */
static enum whep_state state = 0;
static const char *server_url = NULL, *token = NULL, *eos_sink_name = NULL;
static char *resource_url = NULL, *latest_etag = NULL;;

/* Trickle ICE management */
static char *ice_ufrag = NULL, *ice_pwd = NULL, *first_mid = NULL;
static GAsyncQueue *candidates = NULL;

/* Helper methods and callbacks */
static gboolean whep_check_plugins(void);
static void whep_options(void);
static gboolean whep_initialize(void);
static void whep_negotiation_needed(GstElement *element, gpointer user_data);
static void whep_offer_available(GstPromise *promise, gpointer user_data);
static void whep_candidate(GstElement *webrtc G_GNUC_UNUSED,
	guint mlineindex, char *candidate, gpointer user_data G_GNUC_UNUSED);
static gboolean whep_send_candidates(gpointer user_data);
static void whep_connection_state(GstElement *webrtc, GParamSpec *pspec,
	gpointer user_data G_GNUC_UNUSED);
static void whep_ice_gathering_state(GstElement *webrtc, GParamSpec *pspec,
	gpointer user_data G_GNUC_UNUSED);
static void whep_ice_connection_state(GstElement *webrtc, GParamSpec *pspec,
	gpointer user_data G_GNUC_UNUSED);
static void whep_dtls_connection_state(GstElement *dtls, GParamSpec *pspec,
	gpointer user_data G_GNUC_UNUSED);
static void whep_connect(GstWebRTCSessionDescription *offer);
static void whep_process_link_header(char *link);
static gboolean whep_parse_offer(char *sdp_offer);
static void whep_disconnect(char *reason);
static void whep_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data);

/* Helper struct to handle libsoup HTTP sessions */
typedef struct whep_http_session {
	/* libsoup HTTP session */
	SoupSession *http_conn;
	/* libsoup HTTP message */
	SoupMessage *msg;
	/* Redirect url */
	char *redirect_url;
	/* Number of redirects happened so far */
	guint redirects;
} whep_http_session;
/* Helper method to send HTTP messages */
static guint whep_http_send(whep_http_session *session, char *method,
	char *url, char *payload, char *content_type, GBytes **bytes);


/* Signal handler */
static volatile gint stop = 0, disconnected = 0;
static void whep_handle_signal(int signum) {
	WHEP_LOG(LOG_INFO, "Stopping the WHEP client...\n");
	if(g_atomic_int_compare_and_exchange(&stop, 0, 1)) {
		whep_disconnect("Shutting down");
	} else {
		g_atomic_int_inc(&stop);
		if(g_atomic_int_get(&stop) > 2)
			exit(1);
	}
}

/* Supported command-line arguments */
static GOptionEntry opt_entries[] = {
	{ "url", 'u', 0, G_OPTION_ARG_STRING, &server_url, "Address of the WHEP endpoint (required)", NULL },
	{ "token", 't', 0, G_OPTION_ARG_STRING, &token, "Authentication Bearer token to use (optional)", NULL },
	{ "audio", 'A', 0, G_OPTION_ARG_STRING, &audio_caps, "GStreamer caps to use for audio (optional, required if audio-only)", NULL },
	{ "video", 'V', 0, G_OPTION_ARG_STRING, &video_caps, "GStreamer caps to use for video (optional, required if video-only)", NULL },
	{ "no-trickle", 'n', 0, G_OPTION_ARG_NONE, &no_trickle, "Don't trickle candidates, but put them in the SDP offer (default: false)", NULL },
	{ "follow-link", 'f', 0, G_OPTION_ARG_NONE, &follow_link, "Use the Link headers returned by the WHEP server to automatically configure STUN/TURN servers to use (default: false)", NULL },
	{ "stun-server", 'S', 0, G_OPTION_ARG_STRING, &stun_server, "STUN server to use, if any (stun://hostname:port)", NULL },
	{ "turn-server", 'T', 0, G_OPTION_ARG_STRING_ARRAY, &turn_server, "TURN server to use, if any; can be called multiple times (turn(s)://username:password@host:port?transport=[udp,tcp])", NULL },
	{ "force-turn", 'F', 0, G_OPTION_ARG_NONE, &force_turn, "In case TURN servers are provided, force using a relay (default: false)", NULL },
	{ "log-level", 'l', 0, G_OPTION_ARG_INT, &whep_log_level, "Logging level (0=disable logging, 7=maximum log level; default: 4)", NULL },
	{ "disable-colors", 'o', 0, G_OPTION_ARG_NONE, &disable_colors, "Disable colors in the logging (default: enabled)", NULL },
	{ "log-timestamps", 'L', 0, G_OPTION_ARG_NONE, &whep_log_timestamps, "Enable logging timestamps (default: disabled)", NULL },
	{ "eos-sink-name", 'e', 0, G_OPTION_ARG_STRING, &eos_sink_name, "GStreamer sink name for EOS signal", NULL },
	{ "jitter-buffer", 'b', 0, G_OPTION_ARG_INT, &latency, "Jitter buffer (latency) to use in RTP, in milliseconds (default: -1, use webrtcbin's default)", NULL },
	{ NULL },
};

/* Main application */
int main(int argc, char *argv[]) {

	/* Parse the command-line arguments */
	GError *error = NULL;
	GOptionContext *opts = g_option_context_new("-- Simple WHEP client");
	g_option_context_set_help_enabled(opts, TRUE);
	g_option_context_add_main_entries(opts, opt_entries, NULL);
	if(!g_option_context_parse(opts, &argc, &argv, &error)) {
		g_print("%s\n", error->message);
		g_error_free(error);
		exit(1);
	}
	/* If some arguments are missing, fail */
	if(server_url == NULL || (audio_caps == NULL && video_caps == NULL)) {
		char *help = g_option_context_get_help(opts, TRUE, NULL);
		g_print("%s", help);
		g_free(help);
		g_option_context_free(opts);
		exit(1);
	}

	/* Logging level: default is info and no timestamps */
	if(whep_log_level == 0)
		whep_log_level = LOG_INFO;
	if(whep_log_level < LOG_NONE)
		whep_log_level = 0;
	else if(whep_log_level > LOG_MAX)
		whep_log_level = LOG_MAX;
	if(disable_colors)
		whep_log_colors = FALSE;

	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, whep_handle_signal);
	signal(SIGTERM, whep_handle_signal);

	WHEP_LOG(LOG_INFO, "\n--------------------\n");
	WHEP_LOG(LOG_INFO, "Simple WHEP client\n");
	WHEP_LOG(LOG_INFO, "------------------\n\n");

	WHEP_LOG(LOG_INFO, "WHEP endpoint:  %s\n", server_url);
	WHEP_LOG(LOG_INFO, "Bearer Token:   %s\n", token ? token : "(none)");
	WHEP_LOG(LOG_INFO, "Trickle ICE:    %s\n", no_trickle ? "no (candidates in SDP offer)" : "yes (HTTP PATCH)");
	WHEP_LOG(LOG_INFO, "Auto STUN/TURN: %s\n", follow_link ? "yes (via Link headers)" : "no");
	if(!follow_link || stun_server || turn_server) {
		if(stun_server && strstr(stun_server, "stun://") != stun_server) {
			WHEP_LOG(LOG_WARN, "Invalid STUN address (should be stun://hostname:port)\n");
			stun_server = NULL;
		} else {
			WHEP_LOG(LOG_INFO, "STUN server:    %s\n", stun_server ? stun_server : "(none)");
		}
		if(turn_server == NULL || turn_server[0] == NULL) {
			WHEP_LOG(LOG_INFO, "TURN server:    (none)\n");
		} else {
			int i=0;
			while(turn_server[i] != NULL) {
				if(strstr(turn_server[i], "turn://") != turn_server[i] &&
						strstr(turn_server[i], "turns://") != turn_server[i]) {
					WHEP_LOG(LOG_WARN, "Invalid TURN address (should be turn(s)://username:password@host:port?transport=[udp,tcp]\n");
				} else {
					WHEP_LOG(LOG_INFO, "TURN server:    %s\n", turn_server[i]);
				}
				i++;
			}
		}
	}
	if(force_turn) {
		if(!follow_link && !turn_server) {
			WHEP_LOG(LOG_WARN, "Can't force TURN, no TURN servers provided\n");
			force_turn = FALSE;
		} else {
			WHEP_LOG(LOG_INFO, "Forcing TURN:   true\n");
		}
	}
	WHEP_LOG(LOG_INFO, "Audio caps: %s\n", audio_caps ? audio_caps : "(none)");
	WHEP_LOG(LOG_INFO, "Video caps: %s\n\n", video_caps ? video_caps : "(none)");
	if(latency > 1000)
		WHEP_LOG(LOG_WARN, "Very high jitter-buffer latency configured (%u)\n", latency);

	/* Initialize gstreamer */
	gst_init(NULL, NULL);
	/* Make sure our gstreamer dependency has all we need */
	if(!whep_check_plugins())
		exit(1);

	/* Start the main Glib loop */
	loop = g_main_loop_new(NULL, FALSE);
	/* If we need to autoconfigure STUN/TURN, send an OPTIONS */
	if(follow_link)
		whep_options();
	/* Initialize the stack (and then connect to the WHEP endpoint) */
	if(!whep_initialize())
		exit(1);

	/* Loop forever */
	g_main_loop_run(loop);
	if(loop != NULL)
		g_main_loop_unref(loop);

	/* We're done */
	if(pipeline) {
		gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
		WHEP_PREFIX(LOG_INFO, "GStreamer pipeline stopped\n");
		gst_object_unref(pipeline);
	}

	g_free(resource_url);
	g_free(latest_etag);
	g_free(ice_ufrag);
	g_free(ice_pwd);
	g_free(first_mid);
	g_async_queue_unref(candidates);
	g_free(auto_stun_server);
	if(auto_turn_server != NULL) {
		int count = 0;
		while(auto_turn_server[count] != NULL) {
			g_free(auto_turn_server[count]);
			count++;
		}
	}
	g_free(auto_turn_server);

	gst_deinit();

	WHEP_LOG(LOG_INFO, "\nBye!\n");
	exit(0);
}


/* Helper method to ensure GStreamer has the modules we need */
static gboolean whep_check_plugins(void) {
	/* Note: since the pipeline is dynamic, there may be more requirements... */
	const char *needed[] = {
		"opus",
		"vpx",
		"nice",
		"webrtc",
		"dtls",
		"srtp",
		"rtpmanager",
		"videotestsrc",
		"audiotestsrc",
		NULL
	};
	GstRegistry *registry = gst_registry_get();
	if(registry == NULL) {
		WHEP_LOG(LOG_FATAL, "No plugins registered in gstreamer\n");
		return FALSE;
	}
	gboolean ret = TRUE;

	int i = 0;
	GstPlugin *plugin = NULL;
	for(i = 0; i < g_strv_length((char **) needed); i++) {
		plugin = gst_registry_find_plugin(registry, needed[i]);
		if(plugin == NULL) {
			WHEP_LOG(LOG_FATAL, "Required gstreamer plugin '%s' not found\n", needed[i]);
			ret = FALSE;
			continue;
		}
		gst_object_unref(plugin);
	}
	return ret;
}

/* Helper method to send an OPTIONS to the WHEP server to get the STUN/TURN servers */
static void whep_options(void) {
	stun_server = NULL;
	turn_server = NULL;
	/* Create an HTTP connection */
	whep_http_session session = { 0 };
	guint status = whep_http_send(&session, "OPTIONS", (char *)server_url, NULL, NULL, NULL);
	if(status != 200 && status != 204) {
		/* Didn't get the success we were expecting */
		WHEP_LOG(LOG_WARN, " [%u] %s\n\n", status, status ? soup_message_get_reason_phrase(session.msg) : "HTTP error");
		g_object_unref(session.msg);
		g_object_unref(session.http_conn);
		return;
	}
	/* Check if there's Link headers with STUN/TURN servers we can use */
	const char *link = soup_message_headers_get_list(soup_message_get_response_headers(session.msg), "link");
	if(link == NULL) {
		WHEP_LOG(LOG_WARN, "No Link headers in OPTIONS response\n");
	} else {
		WHEP_PREFIX(LOG_INFO, "Auto configuration of STUN/TURN servers:\n");
		int i = 0;
		gchar **links = g_strsplit(link, ", ", -1);
		while(links[i] != NULL) {
			whep_process_link_header(links[i]);
			i++;
		}
		g_clear_pointer(&links, g_strfreev);
	}
	WHEP_LOG(LOG_INFO, "\n");
}

static gboolean source_events(GstPad *pad, GstObject *parent, GstEvent *event) {
	gboolean ret;

	switch(GST_EVENT_TYPE(event)) {
		case GST_EVENT_EOS:
			ret = gst_pad_event_default(pad, parent, event);
			whep_disconnect("Shutting down (EOS)");
			break;
		default: {
			ret = gst_pad_event_default(pad, parent, event);
			break;
		}
	}

	return ret;
}

/* Helper method to initialize the GStreamer WebRTC stack */
static gboolean whep_initialize(void) {
	/* Prepare the pipeline, using the info we got from the command line */
	pipeline = gst_pipeline_new(NULL);
	pc = gst_element_factory_make("webrtcbin", NULL);
	g_object_set(pc, "bundle-policy", 3, NULL);
	if(stun_server != NULL)
		g_object_set(pc, "stun-server", stun_server, NULL);
	if(turn_server != NULL)
		g_object_set(pc, "turn-server", turn_server, NULL);
	if(force_turn)
		g_object_set(pc, "ice-transport-policy", 1, NULL);
	gst_bin_add(GST_BIN(pipeline), pc);
	/* Add transceivers to receive audio and/or video */
	if(audio_caps) {
		GstWebRTCRTPTransceiver *transceiver = NULL;
		GstCaps *caps = gst_caps_from_string(audio_caps);
		g_signal_emit_by_name(pc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, caps, &transceiver);
		gst_caps_unref(caps);
		gst_object_unref(transceiver);
	}
	if(video_caps) {
		GstWebRTCRTPTransceiver *transceiver = NULL;
		GstCaps *caps = gst_caps_from_string(video_caps);
		g_signal_emit_by_name(pc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, caps, &transceiver);
		gst_caps_unref(caps);
		gst_object_unref(transceiver);
	}
	gst_element_sync_state_with_parent(pc);
	/* We'll handle incoming streams, and how to render them, dynamically */
	g_signal_connect(pc, "pad-added", G_CALLBACK(whep_incoming_stream), pc);

	if(eos_sink_name != NULL) {
		GstElement *eossrc = gst_bin_get_by_name(GST_BIN(pipeline), eos_sink_name);
		GstPad *sinkpad = gst_element_get_static_pad(eossrc, "sink");
		gst_pad_set_event_function(sinkpad, source_events);
		gst_object_unref(sinkpad);
	}

	/* Check if there's any TURN server to add */
	if((turn_server != NULL && turn_server[0] != NULL) || (auto_turn_server != NULL && auto_turn_server[0] != NULL)) {
		int i=0;
		gboolean ret = FALSE;
		char *ts = NULL;
		while((ts = turn_server ? (char *)turn_server[i] : auto_turn_server[i]) != NULL) {
			if(strstr(ts, "turn://") != ts && strstr(ts, "turns://") != ts) {
				/* Invalid TURN server, skip */
			} else {
				g_signal_emit_by_name(pc, "add-turn-server", ts, &ret);
				if(!ret)
					WHEP_LOG(LOG_WARN, "Error adding TURN server (%s)\n", ts);
			}
			i++;
		}
	}
	/* Let's configure the function to be invoked when an SDP offer can be prepared */
	g_signal_connect(pc, "on-negotiation-needed", G_CALLBACK(whep_negotiation_needed), NULL);
	/* We need a different callback to be notified about candidates to trickle to Janus */
	g_signal_connect(pc, "on-ice-candidate", G_CALLBACK(whep_candidate), NULL);
	/* We also add a couple of callbacks to be notified about connection state changes */
	g_signal_connect(pc, "notify::connection-state", G_CALLBACK(whep_connection_state), NULL);
	g_signal_connect(pc, "notify::ice-gathering-state", G_CALLBACK(whep_ice_gathering_state), NULL);
	g_signal_connect(pc, "notify::ice-connection-state", G_CALLBACK(whep_ice_connection_state), NULL);
	/* Create a queue for gathered candidates */
	candidates = g_async_queue_new_full((GDestroyNotify)g_free);

	/* If a latency value has been passed as an argument, enforce it */
	GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(pc), "rtpbin");
	if(latency >= 0)
		g_object_set(rtpbin, "latency", latency, "buffer-mode", 0, NULL);
	guint rtp_latency = 0;
	g_object_get(rtpbin, "latency", &rtp_latency, NULL);
	WHEP_PREFIX(LOG_INFO, "Configured jitter-buffer size (latency) for PeerConnection to %ums\n", rtp_latency);
	gst_object_unref(rtpbin);

	/* Start the pipeline */
	gst_element_set_state(pipeline, GST_STATE_READY);

	WHEP_PREFIX(LOG_INFO, "Starting the GStreamer pipeline\n");
	GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
	if(ret == GST_STATE_CHANGE_FAILURE) {
		WHEP_LOG(LOG_ERR, "Failed to set the pipeline state to playing\n");
		goto err;
	}

	/* Done */
	return TRUE;

err:
	/* If we got here, something went wrong */
	if(pipeline)
		g_clear_object(&pipeline);
	if(pc)
		pc = NULL;
	return FALSE;
}

/* Callback invoked when we need to prepare an SDP offer */
static void whep_negotiation_needed(GstElement *element, gpointer user_data) {
	if(resource_url != NULL) {
		/* We've sent an offer already, is something wrong? */
		WHEP_LOG(LOG_WARN, "GStreamer trying to create a new offer, but we don't support renegotiations yet...\n");
		return;
	}
	WHEP_PREFIX(LOG_INFO, "Creating offer\n");
	state = WHEP_STATE_OFFER_PREPARED;
	GstPromise *promise = gst_promise_new_with_change_func(whep_offer_available, user_data, NULL);
	g_signal_emit_by_name(pc, "create-offer", NULL, promise);
}

/* Callback invoked when we have an SDP offer ready to be sent */
static GstWebRTCSessionDescription *offer = NULL;
static void whep_offer_available(GstPromise *promise, gpointer user_data) {
	WHEP_PREFIX(LOG_INFO, "Offer created\n");
	/* Make sure we're in the right state */
	g_assert_cmphex(state, ==, WHEP_STATE_OFFER_PREPARED);
	g_assert_cmphex(gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
	const GstStructure *reply = gst_promise_get_reply(promise);
	gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
	gst_promise_unref(promise);

	/* Set the local description locally */
	WHEP_PREFIX(LOG_INFO, "Setting local description\n");
	promise = gst_promise_new();
	g_signal_emit_by_name(pc, "set-local-description", offer, promise);
	gst_promise_interrupt(promise);
	gst_promise_unref(promise);

	/* Now that a DTLS stack is available, try monitoring the DTLS state too */
	GstElement *dtls = gst_bin_get_by_name(GST_BIN(pc), "dtlsdec0");
	g_signal_connect(dtls, "notify::connection-state", G_CALLBACK(whep_dtls_connection_state), NULL);
	gst_object_unref(dtls);

	/* Now that the offer is ready, connect to the WHEP endpoint and send it there
	 * (unless we're not tricking, in which case we wait for gathering to be
	 * completed, and then add all candidates to this offer before sending it) */
	if(!no_trickle || gathering_done) {
		whep_connect(offer);
		gst_webrtc_session_description_free(offer);
		offer = NULL;
	}
}

/* Callback invoked when a candidate to trickle becomes available */
static void whep_candidate(GstElement *webrtc G_GNUC_UNUSED,
		guint mlineindex, char *candidate, gpointer user_data G_GNUC_UNUSED) {
	if(g_atomic_int_get(&stop) || g_atomic_int_get(&disconnected))
		return;
	/* Make sure we're in the right state*/
	if(state < WHEP_STATE_OFFER_PREPARED) {
		whep_disconnect("Can't trickle, not in a PeerConnection");
		return;
	}
	if(mlineindex != 0) {
		/* We're bundling, so we don't care */
		return;
	}
	int component = 0;
	gchar **parts = g_strsplit(candidate, " ", -1);
	if(parts[0] && parts[1])
		component = atoi(parts[1]);
	g_strfreev(parts);
	if(component != 1) {
		/* We're bundling, so we don't care */
		return;
	}
	/* Keep track of the candidate, we'll send it later when the timer fires */
	g_async_queue_push(candidates, g_strdup(candidate));
}

/* Helper method to send candidates via HTTP PATCH */
static gboolean whep_send_candidates(gpointer user_data) {
	if(candidates == NULL || g_async_queue_length(candidates) == 0)
		return TRUE;
	/* Prepare the fragment to send (credentials + fake mline + candidate) */
	char fragment[4096];
	g_snprintf(fragment, sizeof(fragment),
		"a=ice-ufrag:%s\r\n"
		"a=ice-pwd:%s\r\n"
		"m=%s 9 RTP/AVP 0\r\n", ice_ufrag, ice_pwd, audio_caps ? "audio" : "video");
	if(first_mid) {
		g_strlcat(fragment, "a=mid:", sizeof(fragment));
		g_strlcat(fragment, first_mid, sizeof(fragment));
		g_strlcat(fragment, "\r\n", sizeof(fragment));
	}
	char *candidate = NULL;
	while((candidate = g_async_queue_try_pop(candidates)) != NULL) {
		WHEP_PREFIX(LOG_VERB, "Sending candidates: %s\n", candidate);
		g_strlcat(fragment, "a=", sizeof(fragment));
		g_strlcat(fragment, candidate, sizeof(fragment));
		g_strlcat(fragment, "\r\n", sizeof(fragment));
		g_free(candidate);
	}
	/* Send the candidate via a PATCH message */
	if(resource_url == NULL) {
		WHEP_LOG(LOG_WARN, "No resource url, can't trickle...\n");
		return TRUE;
	}
	whep_http_session session = { 0 };
	guint status = whep_http_send(&session, "PATCH", resource_url, fragment, "application/trickle-ice-sdpfrag", NULL);
	if(status != 200 && status != 204) {
		/* Couldn't trickle? */
		WHEP_LOG(LOG_WARN, " [trickle] %u %s\n", status, status ? soup_message_get_reason_phrase(session.msg) : "HTTP error");
	}
	g_object_unref(session.msg);
	g_object_unref(session.http_conn);
	/* If the candidates we sent included an end-of-candidates, let's stop here */
	if(strstr(fragment, "end-of-candidates") != NULL)
		return FALSE;
	return TRUE;
}

/* Callback invoked when the connection state changes */
static void whep_connection_state(GstElement *webrtc, GParamSpec *pspec,
		gpointer user_data G_GNUC_UNUSED) {
	guint state = 0;
	g_object_get(webrtc, "connection-state", &state, NULL);
	switch(state) {
		case 1:
			WHEP_PREFIX(LOG_INFO, "PeerConnection connecting...\n");
			break;
		case 2:
			WHEP_PREFIX(LOG_INFO, "PeerConnection connected\n");
			break;
		case 4:
			WHEP_PREFIX(LOG_ERR, "PeerConnection failed\n");
			whep_disconnect("PeerConnection failed");
			break;
		case 0:
		case 3:
		case 5:
		default:
			/* We don't care (we should in case of restarts?) */
			break;
	}
}

/* Callback invoked when the ICE gathering state changes */
static void whep_ice_gathering_state(GstElement *webrtc, GParamSpec *pspec,
		gpointer user_data G_GNUC_UNUSED) {
	guint state = 0;
	g_object_get(webrtc, "ice-gathering-state", &state, NULL);
	switch(state) {
		case 1:
			WHEP_PREFIX(LOG_INFO, "ICE gathering started...\n");
			break;
		case 2:
			WHEP_PREFIX(LOG_INFO, "ICE gathering completed\n");
			/* Send an a=end-of-candidates trickle */
			g_async_queue_push(candidates, g_strdup("end-of-candidates"));
			gathering_done = TRUE;
			/* If we're not trickling, send the SDP with all candidates now */
			if(no_trickle) {
				whep_connect(offer);
				gst_webrtc_session_description_free(offer);
				offer = NULL;
			}
			break;
		default:
			break;
	}
}

/* Callback invoked when the ICE connection state changes */
static void whep_ice_connection_state(GstElement *webrtc, GParamSpec *pspec,
		gpointer user_data G_GNUC_UNUSED) {
	guint state = 0;
	g_object_get(webrtc, "ice-connection-state", &state, NULL);
	switch(state) {
		case 1:
			WHEP_PREFIX(LOG_INFO, "ICE connecting...\n");
			break;
		case 2:
			WHEP_PREFIX(LOG_INFO, "ICE connected\n");
			break;
		case 3:
			WHEP_PREFIX(LOG_INFO, "ICE completed\n");
			break;
		case 4:
			WHEP_PREFIX(LOG_ERR, "ICE failed\n");
			whep_disconnect("ICE failed");
			break;
		case 0:
		case 5:
		default:
			/* We don't care (we should in case of restarts?) */
			break;
	}
}

/* Callback invoked when the DTLS connection state changes */
static void whep_dtls_connection_state(GstElement *dtls, GParamSpec *pspec,
		gpointer user_data G_GNUC_UNUSED) {
	guint state = 0;
	g_object_get(dtls, "connection-state", &state, NULL);
	switch(state) {
		case 1:
			WHEP_PREFIX(LOG_INFO, "DTLS connection closed\n");
			whep_disconnect("PeerConnection closed");
			break;
		case 2:
			WHEP_PREFIX(LOG_ERR, "DTLS failed\n");
			whep_disconnect("DTLS failed");
			break;
		case 3:
			WHEP_PREFIX(LOG_INFO, "DTLS connecting...\n");
			break;
		case 4:
			WHEP_PREFIX(LOG_INFO, "DTLS connected\n");
			break;
		default:
			/* We don't care (we should in case of restarts?) */
			break;
	}
}

/* Helper method to connect to the WHEP endpoint */
static void whep_connect(GstWebRTCSessionDescription *offer) {
	/* Convert the SDP object to a string */
	char *sdp_offer = gst_sdp_message_as_text(offer->sdp);
	WHEP_PREFIX(LOG_INFO, "Sending SDP offer (%zu bytes)\n", strlen(sdp_offer));

	/* If we're not trickling, add our candidates to the SDP */
	if(no_trickle) {
		/* Prepare the candidate attributes */
		char attributes[4096], expanded_sdp[8192];
		attributes[0] = '\0';
		expanded_sdp[0] = '\0';
		char *candidate = NULL;
		while((candidate = g_async_queue_try_pop(candidates)) != NULL) {
			WHEP_PREFIX(LOG_VERB, "Adding candidate to SDP: %s\n", candidate);
			g_strlcat(attributes, "a=", sizeof(attributes));
			g_strlcat(attributes, candidate, sizeof(attributes));
			g_strlcat(attributes, "\r\n", sizeof(attributes));
			g_free(candidate);
		}
		/* Add them to all m-lines */
		int mlines = 0, i = 0;
		gchar **lines = g_strsplit(sdp_offer, "\r\n", -1);
		gchar *line = NULL;
		while(lines[i] != NULL) {
			line = lines[i];
			if(strstr(line, "m=") == line) {
				/* New m-line */
				mlines++;
				if(mlines > 1)
					g_strlcat(expanded_sdp, attributes, sizeof(expanded_sdp));
			}
			if(strlen(line) > 2) {
				g_strlcat(expanded_sdp, line, sizeof(expanded_sdp));
				g_strlcat(expanded_sdp, "\r\n", sizeof(expanded_sdp));
			}
			i++;
		}
		g_clear_pointer(&lines, g_strfreev);
		g_strlcat(expanded_sdp, attributes, sizeof(expanded_sdp));
		g_free(sdp_offer);
		sdp_offer = g_strdup(expanded_sdp);
	}
	/* Done */
	WHEP_LOG(LOG_VERB, "%s\n", sdp_offer);

	/* Partially parse the SDP to find ICE credentials and the mid for the bundle m-line */
	if(!whep_parse_offer(sdp_offer)) {
		whep_disconnect("SDP error");
		return;
	}

	/* Create an HTTP connection */
	whep_http_session session = { 0 };
	GBytes *bytes = NULL;
	guint status = whep_http_send(&session, "POST", (char *)server_url, sdp_offer, "application/sdp", &bytes);
	g_free(sdp_offer);
	if(status != 201) {
		/* Didn't get the success we were expecting */
		WHEP_LOG(LOG_ERR, " [%u] %s\n", status, status ? soup_message_get_reason_phrase(session.msg) : "HTTP error");
		g_object_unref(session.msg);
		g_object_unref(session.http_conn);
		if(bytes != NULL)
			g_bytes_unref(bytes);
		whep_disconnect("HTTP error");
		return;
	}
	/* Get the response */
	const char *content_type = soup_message_headers_get_content_type(soup_message_get_response_headers(session.msg), NULL);
	if(content_type == NULL || strcasecmp(content_type, "application/sdp")) {
		WHEP_LOG(LOG_ERR, "Unexpected content-type '%s'\n", content_type);
		g_object_unref(session.msg);
		g_object_unref(session.http_conn);
		if(bytes != NULL)
			g_bytes_unref(bytes);
		whep_disconnect("HTTP error");
		return;
	}
	/* Get the body */
	if(bytes == NULL || g_bytes_get_size(bytes) == 0) {
		WHEP_LOG(LOG_ERR, "Missing SDP answer\n");
		g_object_unref(session.msg);
		g_object_unref(session.http_conn);
		if(bytes != NULL)
			g_bytes_unref(bytes);
		whep_disconnect("SDP error");
		return;
	}
	char *answer = g_malloc(g_bytes_get_size(bytes) + 1);
	memcpy(answer, g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	answer[g_bytes_get_size(bytes)] = '\0';
	g_bytes_unref(bytes);
	if(strstr(answer, "v=0\r\n") != answer) {
		WHEP_LOG(LOG_ERR, "Invalid SDP answer\n");
		g_object_unref(session.msg);
		g_object_unref(session.http_conn);
		whep_disconnect("SDP error");
		return;
	}
	/* Check if there's an ETag we should send in upcoming requests */
	const char *etag = soup_message_headers_get_one(soup_message_get_response_headers(session.msg), "etag");
	if(etag == NULL) {
		WHEP_LOG(LOG_WARN, "No ETag header, won't be able to set If-Match when trickling\n");
	} else {
		latest_etag = g_strdup(etag);
	}
	/* Parse the location header to populate the resource url */
	const char *location = soup_message_headers_get_one(soup_message_get_response_headers(session.msg), "location");
	if(location == NULL) {
		WHEP_LOG(LOG_WARN, "No Location header, won't be able to trickle or teardown the session\n");
	} else {
		if(strstr(location, "http")) {
			/* Easy enough */
			resource_url = g_strdup(location);
		} else {
			/* Relative path */
			GUri *l_uri = g_uri_parse(server_url, SOUP_HTTP_URI_FLAGS, NULL);
			GUri *uri = NULL;
			if(location[0] == '/') {
				/* Use the full returned path as new path */
				uri = g_uri_build(SOUP_HTTP_URI_FLAGS,
					g_uri_get_scheme(l_uri),
					g_uri_get_userinfo(l_uri),
					g_uri_get_host(l_uri),
					g_uri_get_port(l_uri),
					location, NULL, NULL);
			} else {
				/* Relative url, build the resource url accordingly */
				const char *endpoint_path = g_uri_get_path(l_uri);
				gchar **parts = g_strsplit(endpoint_path, "/", -1);
				int i=0;
				while(parts[i] != NULL) {
					if(parts[i+1] == NULL) {
						/* Last part of the path, replace it */
						g_free(parts[i]);
						parts[i] = g_strdup(location);
					}
					i++;
				}
				char *resource_path = g_strjoinv("/", parts);
				g_strfreev(parts);
				uri = g_uri_build(SOUP_HTTP_URI_FLAGS,
					g_uri_get_scheme(l_uri),
					g_uri_get_userinfo(l_uri),
					g_uri_get_host(l_uri),
					g_uri_get_port(l_uri),
					location, NULL, NULL);
				g_free(resource_path);
			}
			resource_url = g_uri_to_string(uri);
			g_uri_unref(l_uri);
			g_uri_unref(uri);
		}
		WHEP_PREFIX(LOG_INFO, "Resource URL: %s\n", resource_url);
	}
	if(!no_trickle) {
		/* Now that we know the resource url, prepare the timer to send trickle candidates:
		 * since most candidates will be local, rather than sending an HTTP PATCH message as
		 * soon as we're aware of it, we queue it, and we send a (grouped) message every ~100ms */
		GSource *patch_timer = g_timeout_source_new(100);
		g_source_set_callback(patch_timer, whep_send_candidates, NULL, NULL);
		g_source_attach(patch_timer, NULL);
		g_source_unref(patch_timer);
	}

	/* Process the SDP answer */
	WHEP_PREFIX(LOG_INFO, "Received SDP answer (%zu bytes)\n", strlen(answer));
	WHEP_LOG(LOG_VERB, "%s\n", answer);

	/* Check if there are any candidates in the SDP: we'll need to fake trickles in case */
	if(strstr(answer, "candidate") != NULL) {
		int mlines = 0, i = 0;
		gchar **lines = g_strsplit(answer, "\r\n", -1);
		gchar *line = NULL;
		while(lines[i] != NULL) {
			line = lines[i];
			if(strstr(line, "m=") == line) {
				/* New m-line */
				mlines++;
				if(mlines > 1)	/* We only need candidates from the first one */
					break;
			} else if(mlines == 1 && strstr(line, "a=candidate") != NULL) {
				/* Found a candidate, fake a trickle */
				line += 2;
				WHEP_LOG(LOG_VERB, "  -- Found candidate: %s\n", line);
				g_signal_emit_by_name(pc, "add-ice-candidate", 0, line);
			}
			i++;
		}
		g_clear_pointer(&lines, g_strfreev);
	}
	/* Convert the SDP to something webrtcbin can digest */
	GstSDPMessage *sdp = NULL;
	int ret = gst_sdp_message_new(&sdp);
	if(ret != GST_SDP_OK) {
		/* Something went wrong */
		WHEP_LOG(LOG_ERR, "Error initializing SDP object (%d)\n", ret);
		g_object_unref(session.msg);
		g_object_unref(session.http_conn);
		g_free(answer);
		whep_disconnect("SDP error");
		return;
	}
	ret = gst_sdp_message_parse_buffer((guint8 *)answer, strlen(answer), sdp);
	g_object_unref(session.msg);
	g_object_unref(session.http_conn);
	g_free(answer);
	if(ret != GST_SDP_OK) {
		/* Something went wrong */
		gst_sdp_message_free(sdp);
		WHEP_LOG(LOG_ERR, "Error parsing SDP buffer (%d)\n", ret);
		whep_disconnect("SDP error");
		return;
	}
	GstWebRTCSessionDescription *gst_sdp = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);

	/* Set remote description on our pipeline */
	WHEP_PREFIX(LOG_INFO, "Setting remote description\n");
	GstPromise *promise = gst_promise_new();
	g_signal_emit_by_name(pc, "set-remote-description", gst_sdp, promise);
	gst_promise_interrupt(promise);
	gst_promise_unref(promise);
	gst_webrtc_session_description_free(gst_sdp);
}

/* Helper method to disconnect from the WHEP endpoint */
static void whep_disconnect(char *reason) {
	if(!g_atomic_int_compare_and_exchange(&disconnected, 0, 1))
		return;
	WHEP_PREFIX(LOG_INFO, "Disconnecting from server (%s)\n", reason);
	if(resource_url == NULL) {
		/* FIXME Nothing to do? */
		g_main_loop_quit(loop);
		return;
	}

	/* Create an HTTP connection */
	whep_http_session session = { 0 };
	guint status = whep_http_send(&session, "DELETE", resource_url, NULL, NULL, NULL);
	if(status != 200) {
		WHEP_LOG(LOG_WARN, " [%u] %s\n", status, status ? soup_message_get_reason_phrase(session.msg) : "HTTP error");
	}
	g_object_unref(session.msg);
	g_object_unref(session.http_conn);

	/* Done */
	g_main_loop_quit(loop);
}

/* Static helper to autoaccept certificates */
static gboolean whep_http_accept_certs(SoupMessage *msg, GTlsCertificate *certificate,
		GTlsCertificateFlags tls_errors, gpointer user_data) {
    return TRUE;
}

/* Helper method to send HTTP messages */
static guint whep_http_send(whep_http_session *session, char *method,
		char *url, char *payload, char *content_type, GBytes **bytes) {
	if(session == NULL || method == NULL || url == NULL) {
		WHEP_LOG(LOG_ERR, "Invalid arguments...\n");
		return 0;
	}
	/* Create an HTTP connection */
	session->http_conn = soup_session_new();
	session->msg = soup_message_new(method, session->redirect_url ? session->redirect_url : url);
	soup_message_set_flags(session->msg, SOUP_MESSAGE_NO_REDIRECT);
	g_signal_connect(session->msg, "accept-certificate", G_CALLBACK(whep_http_accept_certs), NULL);
	if(payload != NULL && content_type != NULL) {
		GBytes *pb = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(session->msg, content_type, pb);
		g_bytes_unref(pb);
	}
	if(token != NULL) {
		/* Add an authorization header too */
		char auth[1024];
		g_snprintf(auth, sizeof(auth), "Bearer %s", token);
		soup_message_headers_append(soup_message_get_request_headers(session->msg), "Authorization", auth);
	}
	if(latest_etag != NULL) {
		/* Add an If-Match header too with the available ETag */
		soup_message_headers_append(soup_message_get_request_headers(session->msg), "If-Match", latest_etag);
	}
	/* Send the message synchronously */
	GBytes *rb = NULL;
	GError *error = NULL;
	if(bytes != NULL) {
		rb = soup_session_send_and_read(session->http_conn, session->msg, NULL, &error);
	} else {
		GInputStream *stream = soup_session_send(session->http_conn, session->msg, NULL, &error);
		g_object_unref(stream);
	}
	if(error != NULL) {
		WHEP_LOG(LOG_ERR, "Error sending request: %s...\n", error->message);
		g_error_free(error);
		if(rb != NULL)
			g_bytes_unref(rb);
		return 0;
	}
	SoupStatus status = soup_message_get_status(session->msg);
	if(status == 301 || status == 307) {
		/* Redirected? Let's try again */
		session->redirects++;
		if(session->redirects > 10) {
			/* Redirected too many times, give up... */
			WHEP_LOG(LOG_ERR, "Too many redirects, giving up...\n");
			if(rb != NULL)
				g_bytes_unref(rb);
			return 0;
		}
		g_free(session->redirect_url);
		const char *location = soup_message_headers_get_one(soup_message_get_response_headers(session->msg), "location");
		if(strstr(location, "http")) {
			/* Easy enough */
			session->redirect_url = g_strdup(location);
		} else {
			/* Relative path */
			GUri *l_uri = g_uri_parse(server_url, SOUP_HTTP_URI_FLAGS, NULL);
			GUri *uri = g_uri_build(SOUP_HTTP_URI_FLAGS,
				g_uri_get_scheme(l_uri),
				g_uri_get_userinfo(l_uri),
				g_uri_get_host(l_uri),
				g_uri_get_port(l_uri),
				location, NULL, NULL);
			session->redirect_url = g_uri_to_string(uri);
			g_uri_unref(l_uri);
			g_uri_unref(uri);
		}
		WHEP_LOG(LOG_INFO, "  -- Redirected to %s\n", session->redirect_url);
		g_object_unref(session->msg);
		g_object_unref(session->http_conn);
		if(rb != NULL)
			g_bytes_unref(rb);
		return whep_http_send(session, method, url, payload, content_type, bytes);
	}
	/* If we got here, we're done */
	g_free(session->redirect_url);
	session->redirect_url = NULL;
	if(rb != NULL)
		*bytes = rb;
	return status;
}

/* Helper method to parse SDP offers and extract stuff we need */
static gboolean whep_parse_offer(char *sdp_offer) {
	gchar **parts = g_strsplit(sdp_offer, "\n", -1);
	gboolean mline = FALSE, success = TRUE, done = FALSE;
	if(parts) {
		int index = 0;
		char *line = NULL, *cr = NULL;
		while(!done && success && (line = parts[index]) != NULL) {
			cr = strchr(line, '\r');
			if(cr != NULL)
				*cr = '\0';
			if(*line == '\0') {
				if(cr != NULL)
					*cr = '\r';
				index++;
				continue;
			}
			if(strlen(line) < 3) {
				WHEP_LOG(LOG_ERR, "Invalid line (%zu bytes): %s", strlen(line), line);
				success = FALSE;
				break;
			}
			if(*(line+1) != '=') {
				WHEP_LOG(LOG_ERR, "Invalid line (2nd char is not '='): %s", line);
				success = FALSE;
				break;
			}
			char c = *line;
			if(!mline) {
				/* Global stuff */
				switch(c) {
					case 'a': {
						line += 2;
						char *semicolon = strchr(line, ':');
						if(semicolon != NULL && *(semicolon+1) != '\0') {
							*semicolon = '\0';
							if(!strcasecmp(line, "ice-ufrag")) {
								g_free(ice_ufrag);
								ice_ufrag = g_strdup(semicolon+1);
							} else if(!strcasecmp(line, "ice-pwd")) {
								g_free(ice_pwd);
								ice_pwd = g_strdup(semicolon+1);
							}
							*semicolon = ':';
						}
						break;
					}
					case 'm': {
						/* We found the first m-line, that we'll bundle on */
						mline = TRUE;
						break;
					}
					default: {
						/* We ignore everything else, this is not a full parser */
						break;
					}
				}
			} else {
				/* m-line stuff */
				switch(c) {
					case 'a': {
						line += 2;
						char *semicolon = strchr(line, ':');
						if(semicolon != NULL && *(semicolon+1) != '\0') {
							*semicolon = '\0';
							if(!strcasecmp(line, "ice-ufrag")) {
								g_free(ice_ufrag);
								ice_ufrag = g_strdup(semicolon+1);
							} else if(!strcasecmp(line, "ice-pwd")) {
								g_free(ice_pwd);
								ice_pwd = g_strdup(semicolon+1);
							} else if(!strcasecmp(line, "mid")) {
								g_free(first_mid);
								first_mid = g_strdup(semicolon+1);
							}
							*semicolon = ':';
						}
						break;
					}
					case 'm': {
						/* First m-line ended, we're done */
						done = TRUE;
						break;
					}
					default: {
						/* We ignore everything else, this is not a full parser */
						break;
					}
				}
			}
			if(cr != NULL)
				*cr = '\r';
			index++;
		}
		if(cr != NULL)
			*cr = '\r';
		g_strfreev(parts);
	}
	return success;
}

/* Helper method to parse a Link header, and in case set the STUN/TURN server */
static void whep_process_link_header(char *link) {
	if(link == NULL)
		return;
	WHEP_PREFIX(LOG_INFO, "  -- %s\n", link);
	if(strstr(link, "rel=\"ice-server\"") == NULL) {
		WHEP_LOG(LOG_WARN, "Missing 'rel=\"ice-server\"' attribute, skipping...\n");
		return;
	}
	gboolean brackets = FALSE;
	if(*link == '<') {
		link++;
		brackets = TRUE;
	}
	if(strstr(link, "stun:") == link) {
		/* STUN server */
		if(auto_stun_server != NULL) {
			WHEP_LOG(LOG_WARN, "Ignoring multiple STUN servers...\n");
			return;
		}
		gchar **parts = g_strsplit(link, brackets ? ">; " : "; ", -1);
		if(strstr(parts[0], "stun://") == parts[0]) {
			/* Easy enough */
			auto_stun_server = g_strdup(parts[0]);
		} else {
			char address[256];
			g_snprintf(address, sizeof(address), "stun://%s", parts[0] + strlen("stun:"));
			auto_stun_server = g_strdup(address);
		}
		g_clear_pointer(&parts, g_strfreev);
		WHEP_PREFIX(LOG_INFO, "  -- -- %s\n", auto_stun_server);
		return;
	} else if(strstr(link, "turn:") == link || strstr(link, "turns:") == link) {
		/* TURN server */
		gboolean turns = (strstr(link, "turns:") == link);
		char address[1024], host[256];
		char *username = NULL, *credential = NULL;
		host[0] = '\0';
		GHashTable *list = soup_header_parse_semi_param_list(link);
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, list);
		while(g_hash_table_iter_next(&iter, &key, &value)) {
			if(strstr((char *)key, (turns ? "turns:" : "turn:")) == (char *)key) {
				/* Host part */
				if(strstr((char *)key, (turns ? "turns://" : "turn://")) == (char *)key) {
					g_snprintf(host, sizeof(host), "%s", (char *)key + strlen(turns ? "turns://" : "turn://"));
				} else {
					g_snprintf(host, sizeof(host), "%s", (char *)key + strlen(turns ? "turns:" : "turn:"));
				}
				if(value != NULL) {
					g_strlcat(host, "=", sizeof(host));
					g_strlcat(host, (char *)value, sizeof(host));
				}
			} else if(!strcasecmp((char *)key, "username")) {
				/* Username */
				if(value != NULL) {
					/* We need to escape this, as it will be part of the TURN uri */
					g_free(username);
					username = g_uri_escape_string((char *)value, NULL, FALSE);
				}
			} else if(!strcasecmp((char *)key, "credential")) {
				/* Credential */
				if(value != NULL) {
					/* We need to escape this, as it will be part of the TURN uri */
					g_free(credential);
					credential = g_uri_escape_string((char *)value, NULL, FALSE);
				}
			}
		}
		soup_header_free_param_list(list);
		if(strlen(username) > 0 && strlen(credential) > 0) {
			g_snprintf(address, sizeof(address), "%s://%s:%s@%s",
				turns ? "turns" : "turn", username, credential, host);
		} else {
			g_snprintf(address, sizeof(address), "%s://%s",
				turns ? "turns" : "turn", host);
		}
		if(brackets) {
			char *b = strstr(address, ">");
			if(b)
				*b = '\0';
		}
		WHEP_PREFIX(LOG_INFO, "  -- -- %s\n", address);
		g_free(username);
		g_free(credential);
		/* Add to the list of TURN servers */
		if(auto_turn_server == NULL) {
			auto_turn_server = g_malloc0(2*sizeof(gpointer));
			auto_turn_server[0] = g_strdup(address);
		} else {
			int count = 0;
			while(auto_turn_server[count] != NULL)
				count++;
			auto_turn_server = g_realloc(auto_turn_server, (count+2)*sizeof(gpointer));
			auto_turn_server[count] = g_strdup(address);
			auto_turn_server[count+1] = NULL;
		}
		return;
	}
	WHEP_LOG(LOG_WARN, "Unsupported protocol, skipping...\n");
	return;
}

/* Callbacks invoked when we have a stream from an existing subscription */
static void whep_handle_media_stream(GstPad *pad, gboolean video) {
	/* Create the elements needed to play/render the stream */
	GstElement *entry = gst_element_factory_make("queue", NULL);
	GstElement *conv = gst_element_factory_make(video ? "videoconvert" : "audioconvert", NULL);
	GstElement *resample = video ? NULL : gst_element_factory_make("audioresample", NULL);
	GstElement *sink = gst_element_factory_make(video ? "autovideosink" : "autoaudiosink", NULL);
	if(!video) {
		/* This is audio */
		gst_bin_add_many(GST_BIN(pipeline), entry, conv, resample, sink, NULL);
	} else {
		/* This is video */
		gst_bin_add_many(GST_BIN(pipeline), entry, conv, sink, NULL);
	}
	gst_element_sync_state_with_parent(entry);
	gst_element_sync_state_with_parent(conv);
	if(resample)
		gst_element_sync_state_with_parent(resample);
	gst_element_sync_state_with_parent(sink);
	if(!video && !gst_element_link_many(entry, conv, resample, sink, NULL)) {
		WHEP_LOG(LOG_ERR, "Error linking audio pad...\n");
	} else if(video && !gst_element_link_many(entry, conv, sink, NULL)) {
		WHEP_LOG(LOG_ERR, "Error linking video pad...\n");
	}
	g_object_set(sink, "sync", FALSE, NULL);
	/* Finally, let's connect the webrtcbin pad to our entry queue */
	GstPad *entry_pad = gst_element_get_static_pad(entry, "sink");
	GstPadLinkReturn ret = gst_pad_link(pad, entry_pad);
	if(ret != GST_PAD_LINK_OK) {
		WHEP_LOG(LOG_ERR, "Error connecting webrtcbin to %s pad (%d)...\n",
			video ? "video" : "audio", ret);
	}
}
static void whep_incoming_decodebin_stream(GstElement *decodebin, GstPad *pad, gpointer user_data) {
	/* The decodebin element has a new stream, render it */
	if(!gst_pad_has_current_caps(pad)) {
		WHEP_LOG(LOG_ERR, "Pad '%s' has no caps, ignoring\n", GST_PAD_NAME(pad));
		return;
	}
	GstCaps *caps = gst_pad_get_current_caps(pad);
	const char *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
	if(g_str_has_prefix(name, "video")) {
		whep_handle_media_stream(pad, TRUE);
	} else if(g_str_has_prefix(name, "audio")) {
		whep_handle_media_stream(pad, FALSE);
	} else {
		WHEP_LOG(LOG_ERR, "Unknown pad %s, ignoring\n", GST_PAD_NAME(pad));
	}
}
static void whep_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data) {
	/* Create an element to decode the stream */
	WHEP_LOG(LOG_INFO, "Creating decodebin element\n");
	GstElement *decodebin = gst_element_factory_make("decodebin", NULL);
	g_signal_connect(decodebin, "pad-added", G_CALLBACK(whep_incoming_decodebin_stream), pc);
	gst_bin_add(GST_BIN(pipeline), decodebin);
	gst_element_sync_state_with_parent(decodebin);
	GstPad *sinkpad = gst_element_get_static_pad(decodebin, "sink");
	gst_pad_link(pad, sinkpad);
	gst_object_unref(sinkpad);
}
