/*
 * Simple WHEP client
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: GPLv3
 *
 * Logging utilities, copied from the Janus WebRTC Server
 *
 */

#ifndef WHEP_DEBUG_H
#define WHEP_DEBUG_H

#include <inttypes.h>

#include <glib.h>
#include <glib/gprintf.h>

extern int whep_log_level;
extern gboolean whep_log_timestamps;
extern gboolean whep_log_colors;

/* Log colors */
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

/* Log levels */
#define LOG_NONE     (0)
#define LOG_FATAL    (1)
#define LOG_ERR      (2)
#define LOG_WARN     (3)
#define LOG_INFO     (4)
#define LOG_VERB     (5)
#define LOG_HUGE     (6)
#define LOG_DBG      (7)
#define LOG_MAX LOG_DBG

/* Coloured prefixes for errors and warnings logging. */
static const char *whep_log_prefix[] = {
/* no colors */
	"",
	"[FATAL] ",
	"[ERR] ",
	"[WARN] ",
	"",
	"",
	"",
	"",
/* with colors */
	"",
	ANSI_COLOR_MAGENTA"[FATAL]"ANSI_COLOR_RESET" ",
	ANSI_COLOR_RED"[ERR]"ANSI_COLOR_RESET" ",
	ANSI_COLOR_YELLOW"[WARN]"ANSI_COLOR_RESET" ",
	"",
	"",
	"",
	""
};
static const char *whep_name_prefix[] = {
/* no colors */
	"[WHEP] ",
/* with colors */
	ANSI_COLOR_CYAN"[WHEP]"ANSI_COLOR_RESET" "
};

/* Simple wrapper to g_print/printf */
#define WHEP_PRINT g_print
/* Logger based on different levels, which can either be displayed
 * or not according to the configuration of the gateway.
 * The format must be a string literal. */
#define WHEP_LOG(level, format, ...) \
do { \
	if (level > LOG_NONE && level <= LOG_MAX && level <= whep_log_level) { \
		char whep_log_ts[64] = ""; \
		char whep_log_src[128] = ""; \
		if (whep_log_timestamps) { \
			struct tm wheptmresult; \
			time_t whepltime = time(NULL); \
			localtime_r(&whepltime, &wheptmresult); \
			strftime(whep_log_ts, sizeof(whep_log_ts), \
			         "[%a %b %e %T %Y] ", &wheptmresult); \
		} \
		if (level == LOG_FATAL || level == LOG_ERR || level == LOG_DBG) { \
			snprintf(whep_log_src, sizeof(whep_log_src), \
			         "[%s:%s:%d] ", __FILE__, __FUNCTION__, __LINE__); \
		} \
		g_print("%s%s%s" format, \
		        whep_log_ts, \
		        whep_log_prefix[level | ((int)whep_log_colors << 3)], \
		        whep_log_src, \
		        ##__VA_ARGS__); \
	} \
} while (0)

/* Same as above, but with a [WHEP] prefix */
#define WHEP_PREFIX(level, format, ...) \
do { \
	if (level > LOG_NONE && level <= LOG_MAX && level <= whep_log_level) { \
		char whep_log_ts[64] = ""; \
		char whep_log_src[128] = ""; \
		if (whep_log_timestamps) { \
			struct tm wheptmresult; \
			time_t whepltime = time(NULL); \
			localtime_r(&whepltime, &wheptmresult); \
			strftime(whep_log_ts, sizeof(whep_log_ts), \
			         "[%a %b %e %T %Y] ", &wheptmresult); \
		} \
		if (level == LOG_FATAL || level == LOG_ERR || level == LOG_DBG) { \
			snprintf(whep_log_src, sizeof(whep_log_src), \
			         "[%s:%s:%d] ", __FILE__, __FUNCTION__, __LINE__); \
		} \
		g_print("%s%s%s%s" format, \
		        whep_name_prefix[whep_log_colors], \
		        whep_log_ts, \
		        whep_log_prefix[level | ((int)whep_log_colors << 3)], \
		        whep_log_src, \
		        ##__VA_ARGS__); \
	} \
} while (0)

#endif
