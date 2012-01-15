#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#define SYSLOG_NAMES
#include <syslog.h>

#include "postgres.h"
#include "tcop/tcopprot.h"
#include "libpq/libpq.h"
#include "utils/memutils.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

#define DEFAULT_SYSLOG_FACILITY	"local0"
#define DEFAULT_PAYLOAD_FORMAT	"json"
#define DEFAULT_FORMAT_FUNC		format_json

#define MAX_MESSAGE_SIZE		8192

#define NETSTR(buf, size, value) \
					snprintf(buf, size, "%u:%s,", \
						(unsigned)((value) ? strlen(value) : 0), \
						(value) ? (value) : "")


struct LogTarget;	/* Forward declaration */

typedef const char *(*format_payload_t)(struct LogTarget *t, ErrorData *e);

typedef struct LogTarget {
	struct LogTarget   *next;
	const char		   *name;
	char			   *remote_ip;
	int					remote_port;
	int					log_socket;
	struct sockaddr_in	si_remote;
	char			   *log_format;
	char			   *syslog_facility;
	int					facility_id;

	/* Log filtering */
	int					min_elevel;
	char			   *message_filter;

	/* Formatting function */
	format_payload_t	format_payload;
} LogTarget;


void _PG_init(void);
static void emit_log(ErrorData *edata);
static const char *format_json(struct LogTarget *t, ErrorData *edata);
static const char *format_syslog(struct LogTarget *t, ErrorData *edata);
static const char *format_netstr(struct LogTarget *t, ErrorData *edata);
static void defineStringVariable(const char *name, const char *short_desc, char **value_addr);
static void defineIntVariable(const char *name, const char *short_desc, int *value_addr);
static void escape_json(char **dst, size_t *max, const char *str);
static void append_string(char **dst, size_t *max, const char *src);
static void append_json_string(char **buf, size_t *max, const char *key, const char *val, bool addComma);
static void append_json_int(char **buf, size_t *max, const char *key, int val, bool addComma);


static emit_log_hook_type	prev_emit_log_hook = NULL;
static LogTarget		   *log_targets = NULL;
static char				   *log_target_names = "";
static char				   *log_username = NULL;
static char				   *log_database = NULL;
static char				   *log_hostname = NULL;
static char					my_hostname[64];
static char					msgbuf[MAX_MESSAGE_SIZE];


/* Convenience wrapper for DefineCustomStringVariable */
static void defineStringVariable(	const char *name,
									const char *short_desc,
									char **value_addr)
{
	DefineCustomStringVariable(name,
			short_desc,
			NULL,
			value_addr,
#if PG_VERSION_NUM >= 80400
			NULL,				/* bootValue since 8.4 */
#endif
#if PG_VERSION_NUM >= 80400
			PGC_SIGHUP,
			0,					/* flags parameter since 8.4 */
#else
			PGC_USERSET,		/* 8.3 only allows USERSET custom params */
#endif
#if PG_VERSION_NUM >= 90100
			NULL,				/* check_hook parameter since 9.1 */
#endif
			NULL,
			NULL);
}

/* Convinience wrapper for DefineCustomIntVariable */
static void defineIntVariable(	const char *name,
								const char *short_desc,
								int *value_addr)
{
	DefineCustomIntVariable(name,
			short_desc,
			NULL,
			value_addr,
#if PG_VERSION_NUM >= 80400
			0, 					/* bootValue since 8.4 */
#endif
			0,
			65535,
#if PG_VERSION_NUM >= 80400
			PGC_SIGHUP,
			0,
#else
			PGC_USERSET,		/* 8.3 only allows USERSET custom params */
#endif
#if PG_VERSION_NUM >= 90100
			NULL,				/* check_hook parameter since 9.1 */
#endif
			NULL,
			NULL);
}

/*
 * Module Load Callback
 */
void
_PG_init(void)
{
	LogTarget	   *tail = log_targets;
	char		   *target_names, *tgname;
	MemoryContext	mctx;

	/* Install Hooks */
	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = emit_log;

	/* Obtain my hostname for syslogging */
	if (gethostname(my_hostname, sizeof(my_hostname)) != 0)
		snprintf(my_hostname, sizeof(my_hostname), "[unknown]");

	mctx = MemoryContextSwitchTo(TopMemoryContext);

	defineStringVariable("logforward.target_names",
						 "List of log forwarding destination names",
						 &log_target_names);
	target_names = pstrdup(log_target_names);

	/*
	 * Set up the log targets.
	 */
	for (tgname = strtok(target_names, ","); tgname != NULL; tgname = strtok(NULL, ","))
	{
		LogTarget  *target = palloc(sizeof(LogTarget));
		char		buf[64];

		target->name = tgname;
		target->next = NULL;
		target->remote_ip = "";
		target->remote_port = 0;
		target->min_elevel = 0;
		target->message_filter = NULL;
		target->log_format = DEFAULT_PAYLOAD_FORMAT;
		target->syslog_facility = DEFAULT_SYSLOG_FACILITY;
		target->facility_id = -1;

		/* Obtain the target specific GUC settings */
		snprintf(buf, sizeof(buf), "logforward.%s_host", tgname);
		defineStringVariable(buf, "Remote IP address where logs are forwarded",
							 &target->remote_ip);

		snprintf(buf, sizeof(buf), "logforward.%s_port", tgname);
		defineIntVariable(	buf, "Remote port where logs are forwarded",
							 &target->remote_port);

		snprintf(buf, sizeof(buf), "logforward.%s_min_elevel", tgname);
		defineIntVariable(	buf, "Minimum elevel that will be forwarded",
							 &target->min_elevel);

		snprintf(buf, sizeof(buf), "logforward.%s_message_filter", tgname);
		defineStringVariable(buf, "Messages to be filtered for this target",
							 &target->message_filter);

		snprintf(buf, sizeof(buf), "logforward.%s_format", tgname);
		defineStringVariable(buf, "Log format for this target: json, netstr, syslog",
							 &target->log_format);

		snprintf(buf, sizeof(buf), "logforward.%s_facility", tgname);
		defineStringVariable(buf, "Syslog facility for syslog targets",
							 &target->syslog_facility);

		/*
		 * Set up the logging socket
		 */
		if (!target->remote_ip)
		{
			fprintf(stderr, "pg_logforward: %s: no target ip address defined.\n", tgname);
			continue;
		}

		if (!target->remote_port)
		{
			fprintf(stderr, "pg_logforward: %s: no target port defined.\n", tgname);
			continue;
		}

		if ((target->log_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
		{
			fprintf(stderr, "pg_logforward: %s: cannot create socket: %s\n",
					tgname, strerror(errno));
			continue;
		}

		if (fcntl(target->log_socket, F_SETFL, O_NONBLOCK) == -1)
		{
			fprintf(stderr, "pg_logforward: %s: cannot set socket nonblocking: %s\n",
					tgname, strerror(errno));
			continue;
		}

		memset((char *) &target->si_remote, 0, sizeof(target->si_remote));
		target->si_remote.sin_family = AF_INET;
		target->si_remote.sin_port = htons(target->remote_port);

		if (inet_aton(target->remote_ip, &target->si_remote.sin_addr) == 0)
			fprintf(stderr, "pg_logforward: %s: invalid remote address: %s\n",
					tgname, target->remote_ip);

		/*
		 * Determine format for logging target.
		 */
		if (!target->log_format)
			target->log_format = DEFAULT_PAYLOAD_FORMAT;

		if (strcmp(target->log_format, "json") == 0)
			target->format_payload = format_json;
		else if (strcmp(target->log_format, "netstr") == 0)
			target->format_payload = format_netstr;
		else if (strcmp(target->log_format, "syslog") == 0)
		{
			CODE   *c;

			target->format_payload = format_syslog;
			if (!target->syslog_facility)
				target->syslog_facility = DEFAULT_SYSLOG_FACILITY;

			/* Determine the syslog facility */
			for (c = facilitynames; c->c_name && target->facility_id < 0; c++)
				if (strcasecmp(c->c_name, target->syslog_facility) == 0)
					target->facility_id = LOG_FAC(c->c_val);
			
			/* No valid facility found, skip the target */
			if (target->facility_id < 0)
			{
				fprintf(stderr, "pg_logforward: invalid syslog facility: %s\n",
					target->syslog_facility);
				break;
			}
		}
		else
		{
			fprintf(stderr, "pg_logforward: unknown payload format (%s), using %s",
				target->log_format, DEFAULT_PAYLOAD_FORMAT);
			target->format_payload = DEFAULT_FORMAT_FUNC;
		}

		fprintf(stderr, "pg_logforward: forwarding to target %s: %s:%d, format: %s\n",
				tgname, target->remote_ip, target->remote_port, target->log_format);

		/* Append the new target to the list of targets */
		if (tail)
			tail->next = target;
		else
			log_targets = target;
		tail = target;
	}

	pfree(target_names);

	MemoryContextSwitchTo(mctx);
}

/*
 * Append one string to another, return the number of characters added.
 *
 * The value pointed to by max is decreased and 'dst' advanced by the number of
 * characters added.
 */
static void
append_string(char **dst, size_t *max, const char *src)
{
	size_t len;

	if (! *max || !src)
		return;

	len = strlen(src);
	if (len >= *max)
		len = *max - 1;

	strncat(*dst, src, len);
	*max -= len;
	*dst += len;
}

/*
 * Add a json key/strvalue pair to the buffer.
 */
static void
append_json_string(char **buf, size_t *max, const char *key, const char *val, bool addComma)
{
	escape_json(buf, max, key);
	append_string(buf, max, ": ");
	if (val)
		escape_json(buf, max, val ? val : "null");
	else
		append_string(buf, max, "null");
	if (addComma)
		append_string(buf, max, ", ");
}

/*
 * Add a json key/intvalue pair to the buffer.
 */
static void
append_json_int(char **buf, size_t *max, const char *key, int val, bool addComma)
{
	char intbuf[16];

	escape_json(buf, max, key);
	append_string(buf, max, ": ");

	snprintf(intbuf, sizeof(buf), "%d", val);
	append_string(buf, max, intbuf);

	if (addComma)
		append_string(buf, max, ", ");
}

/*
 * Produce a JSON string literal.
 */
static void
escape_json(char **dst, size_t *max, const char *str)
{
	const char *p;
	char		buf[16];

	append_string(dst, max, "\"");
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':	append_string(dst, max, "\\b"); break;
			case '\f':	append_string(dst, max, "\\f"); break;
			case '\n':	append_string(dst, max, "\\n"); break;
			case '\r':	append_string(dst, max, "\\r"); break;
			case '\t':	append_string(dst, max, "\\t"); break;
			case '"':	append_string(dst, max, "\\\""); break;
			case '\\':	append_string(dst, max, "\\\\"); break;
			default:
				if ((unsigned char) *p < ' ')
				{
					snprintf(buf, sizeof(buf), "\\u%04x", (int) *p);
					append_string(dst, max, buf);
				}
				else
				{
					buf[0] = *p;
					buf[1] = '\0';
					append_string(dst, max, buf);
				}
				break;
		}
	}
	append_string(dst, max, "\"");
}

/*
 * Format the edata as JSON
 */
static const char *format_json(struct LogTarget *target, ErrorData *edata)
{
	char	   *buf;
	size_t		len;

	buf = msgbuf;
	len = sizeof(msgbuf);
	*buf = '\0';

	append_string(&buf, &len, "{ ");
	append_json_string(&buf, &len, "username", log_username, true);
	append_json_string(&buf, &len, "database", log_database, true);
	append_json_string(&buf, &len, "remotehost", log_hostname, true);
	append_json_string(&buf, &len, "debug_query_string", debug_query_string, true);
	append_json_int(&buf, &len, "elevel", edata->elevel, true);
	append_json_string(&buf, &len, "funcname", edata->funcname, true);
	append_json_int(&buf, &len, "sqlerrcode", edata->sqlerrcode, true);
	append_json_string(&buf, &len, "message", edata->message, true);
	append_json_string(&buf, &len, "detail", edata->detail, true);
	append_json_string(&buf, &len, "hint", edata->hint, true);
	append_json_string(&buf, &len, "context", edata->context, false);
	append_string(&buf, &len, " }");

	return msgbuf;
}

/*
 * Format the payload as standard syslog message.
 * See: http://tools.ietf.org/html/rfc5424
 */
static const char *format_syslog(struct LogTarget *target, ErrorData *edata)
{
	int			pri, len, i;
	int			severity = -1;
	time_t		now;
	struct tm  *gmt;
	char		ts[32];

	/* Map the postgres elevel to syslog severity */
	int levels[][2] = {
		{ DEBUG1, 7 }, { INFO, 6 }, { NOTICE, 5 }, { WARNING, 4},
		{ ERROR, 3 }, { FATAL, 2 }, { PANIC, 0 },
	};

	for (i = 0; i < sizeof(levels)/sizeof(levels[0]) && severity < 0; i++)
		if (edata->elevel <= levels[i][0])
			severity = levels[i][1];

	pri = target->facility_id * 8 + severity;

	time(&now);
	gmt = gmtime(&now);
	strftime(ts, sizeof(ts), "%F-%dT%H:%M:%SZ", gmt);

	/*
	 * Syslog message format:
	 * PRI VERSION TS HOSTNAME APPNAME PROCID MSGID SDATA MSG
	 */

	/* header */
	len = snprintf(msgbuf, sizeof(msgbuf), "<%d>1 %s %s postgres %d %s ",
		pri, ts, my_hostname, MyProcPid, "-");
	
	/* structured data - skip for now */
	len += snprintf(msgbuf+len, sizeof(msgbuf) - len, "%s ", "-");

	/* message payload */
	len += snprintf(msgbuf+len, sizeof(msgbuf) - len, "%s", edata->message);

	return msgbuf;
}

/*
 * Format the payload as set of netstrings. No fancy stuff, just
 * one field after another: elevel, sqlerrcode, user, database, host,
 * funcname, message, detail, hint, context, debug_query_string
 */
static const char *format_netstr(struct LogTarget *target, ErrorData *edata)
{
	char		intbuf[16];
	char	   *intptr = intbuf;	/* hack to suppress compiler warning */
	int			len = 0;

	snprintf(intbuf, sizeof(intbuf), "%d", edata->elevel);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, intptr);
	snprintf(intbuf, sizeof(intbuf), "%d", edata->sqlerrcode);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, intptr);

	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, log_username);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, log_database);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, log_hostname);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, edata->funcname);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, edata->message);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, edata->detail);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, edata->hint);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, edata->context);
	len += NETSTR(msgbuf+len, sizeof(msgbuf)-len, debug_query_string);

	return msgbuf;
}

/*
 * Handler for intercepting EmitErrorReport.
 */
static void
emit_log(ErrorData *edata)
{
	const char	*buf = NULL;
	LogTarget   *t;

	/* Call any previous hooks */
	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);

	if (MyProcPort)
	{
		log_database = MyProcPort->database_name;
		log_hostname = MyProcPort->remote_host;
		log_username = MyProcPort->user_name;
	}

	/*
	 * Loop through the log targets, send the message if all 
	 * filter conditions are met.
	 */
	for (t = log_targets; t != NULL; t = t->next)
	{
		/* Skip messages with too low severity */
		if (edata->elevel < t->min_elevel)
			continue;

		/* Skip uninteresting messages */
		if (t->message_filter && !strstr(edata->message, t->message_filter))
			continue;

		buf = t->format_payload(t, edata);

		if (sendto(t->log_socket, buf, strlen(buf), 0, &t->si_remote, sizeof(t->si_remote)) < 0)
			fprintf(stderr, "pg_logforward: sendto: %s\n", strerror(errno));

	}
}

