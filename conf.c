/*
 * Parse configure file.
 *
 * Author: Wu Bingzheng
 *
 */

#include "conf.h"

#define COMMAND_LENGTH	PATH_LENGTH
#define ARGUMENT_LENGTH	PATH_LENGTH

#define OHC_CONF_DONE	(char *)OHC_DONE
#define OHC_CONF_OK	(char *)OHC_OK
#define OHC_CONF_AGAIN	(char *)OHC_AGAIN


static ohc_conf_t conf_cycle;
static ohc_server_t default_server;
static LIST_HEAD(reserved_servers);
static LIST_HEAD(reserved_devices);

typedef struct command_s ohc_conf_command_t;
typedef const char *conf_parse_f(ohc_conf_command_t *, void *, char *);

//24=8+12+4
struct command_s {
	char		*name;
	conf_parse_f	*set_handler;
	int		offset;
};

/* handler of PATH type argument */
static const char *conf_set_path(ohc_conf_command_t *cmd, void *data, char *arg)
{
	char *dest = ((char *)data) + cmd->offset;

	strncpy(dest, arg, PATH_LENGTH);
	return (dest[PATH_LENGTH - 1] != '\0') ? "path too long" : OHC_CONF_OK;
}

/* handler of INT type argument */
static const char *conf_set_int(ohc_conf_command_t *cmd, void *data, char *arg)
{
	char *endp;
	long n = strtoul(arg, &endp, 0);
	if(*endp != '\0' || n > INT_MAX || n < 0) {
		return "invalid integer";
	}

	*((int *)(((char *)data) + cmd->offset)) = (int)n;
	return OHC_CONF_OK;
}

/* handler of SIZE type argument, supporting K,M,G,T */
static const char *conf_set_size(ohc_conf_command_t *cmd, void *data, char *arg)
{
	char *endp;
	int shift;
	long n = strtol(arg, &endp, 0);
	if(endp == arg || n == LONG_MAX || n < 0) {
		return "invalid size";
	}

	switch(*endp) {
		case 'T':  shift = 40; endp++; break;
		case 'G':  shift = 30; endp++; break;
		case 'M':  shift = 20; endp++; break;
		case 'K':  shift = 10; endp++; break;
		case '\0': shift = 0; break;
		default:   return "invalid size";
	}

	if(*endp != '\0') {
		return "invalid size";
	}

	if(shift != 0 && (n & (~0UL << (64 - shift)))) {
		return "invalid size";
	}

	*((size_t *)(((char *)data) + cmd->offset)) = n << shift;
	return OHC_CONF_OK;
}

/* handler of FLAG type argument, only 'on' and 'off' are allowed */
static const char *conf_set_flag(ohc_conf_command_t *cmd, void *data, char *arg)
{
	ohc_flag_t flag;

	if(strcmp(arg, "on") == 0) {
		flag = 1;
	} else if(strcmp(arg, "off") == 0) {
		flag = 0;
	} else {
		return "invalid flag";
	}

	*((ohc_flag_t *)(((char *)data) + cmd->offset)) = flag;
	return OHC_CONF_OK;
}

static const char *conf_new_device(ohc_conf_command_t *cmd, void *data, char *arg)
{
	ohc_device_t *device;
	struct list_head *p;

	if(!list_empty(&reserved_devices)) {
		p = reserved_devices.next;
		list_del(p);
		device = list_entry(p, ohc_device_t, dnode);
	} else {
		device = malloc(sizeof(ohc_device_t));
		if(device == NULL) {
			return "NoMem";
		}
	}
	bzero(device, sizeof(ohc_device_t));

	device->fd = -1;
	strcpy(device->filename, arg);
	list_add_tail(&device->dnode, &conf_cycle.devices);
	return OHC_CONF_OK;
}

static const char *conf_new_server(ohc_conf_command_t *cmd, void *data, char *arg)
{
	ohc_server_t *server;
	struct list_head *p;
	char *endp;
	long port = strtol(arg, &endp, 0);
	if(*endp != '\0' || port < 0 || port > 65535) {
		return "invalid port";
	}

	if(!list_empty(&reserved_servers)) {
		p = reserved_servers.next;
		list_del(p);
		server = list_entry(p, ohc_server_t, snode);
	} else {
		server = malloc(sizeof(ohc_server_t));
		if(server == NULL) {
			return "NoMem";
		}
	}

	*server = default_server;
	server->listen_port = (unsigned short)port;
	server->listen_fd = -1;
	list_add_tail(&server->snode, &conf_cycle.servers);

	return OHC_CONF_OK;
}

/* all configure commands, except 'include' */
#define COMMAND_NUMBER (int)(sizeof(g_commands) / sizeof(ohc_conf_command_t))
static ohc_conf_command_t g_commands[] = {
	/* global */
	{	"threads",
		conf_set_int,
		offsetof(ohc_conf_t, threads)
	},
	{	"error_log",
		conf_set_path,
		offsetof(ohc_conf_t, error_log)
	},
	{	"quit_timeout",
		conf_set_int,
		offsetof(ohc_conf_t, quit_timeout)
	},
	{	"device_badblock_percent",
		conf_set_int,
		offsetof(ohc_conf_t, device_badblock_percent)
	},
	{	"device_check_270G",
		conf_set_flag,
		offsetof(ohc_conf_t, device_check_270G)
	},
	{	"device",
		conf_new_device,
		0
	},

	/* server */
	{	"listen",
		conf_new_server,
		0
	},
	{	"capacity",
		conf_set_size,
		offsetof(ohc_server_t, capacity)
	},
	{	"access_log",
		conf_set_path,
		offsetof(ohc_server_t, access_log)
	},
	{	"connections_limit",
		conf_set_int,
		offsetof(ohc_server_t, connections_limit)
	},
	{	"rcvbuf",
		conf_set_size,
		offsetof(ohc_server_t, rcvbuf)
	},
	{	"sndbuf",
		conf_set_size,
		offsetof(ohc_server_t, sndbuf)
	},
	{	"recv_timeout",
		conf_set_int,
		offsetof(ohc_server_t, recv_timeout)
	},
	{	"request_timeout",
		conf_set_int,
		offsetof(ohc_server_t, request_timeout)
	},
	{	"send_timeout",
		conf_set_int,
		offsetof(ohc_server_t, send_timeout)
	},
	{	"expire_default",
		conf_set_int,
		offsetof(ohc_server_t, expire_default)
	},
	{	"expire_force",
		conf_set_int,
		offsetof(ohc_server_t, expire_force)
	},
	{	"keepalive_timeout",
		conf_set_int,
		offsetof(ohc_server_t, keepalive_timeout)
	},
	{	"item_max_size",
		conf_set_size,
		offsetof(ohc_server_t, item_max_size)
	},
	{	"key_include_host",
		conf_set_flag,
		offsetof(ohc_server_t, key_include_host)
	},
	{	"key_include_ohc_key",
		conf_set_flag,
		offsetof(ohc_server_t, key_include_ohc_key)
	},
	{	"key_include_query",
		conf_set_flag,
		offsetof(ohc_server_t, key_include_query)
	},
	{	"server_dump",
		conf_set_flag,
		offsetof(ohc_server_t, server_dump)
	},
	{	"shutdown_if_not_store",
		conf_set_flag,
		offsetof(ohc_server_t, shutdown_if_not_store)
	},
	{	"passby_enable",
		conf_set_flag,
		offsetof(ohc_server_t, passby_enable)
	},
	{	"passby_begin_item_nr",
		conf_set_int,
		offsetof(ohc_server_t, passby_begin_item_nr)
	},
	{	"passby_begin_consumed",
		conf_set_size,
		offsetof(ohc_server_t, passby_begin_consumed)
	},
	{	"passby_limit_nr",
		conf_set_int,
		offsetof(ohc_server_t, passby_limit_nr)
	},
	{	"passby_expire",
		conf_set_int,
		offsetof(ohc_server_t, passby_expire)
	},
	{	"status_period",
		conf_set_int,
		offsetof(ohc_server_t, status_period)
	},
};

static const char *conf_readline(FILE *fp, char *cmd, char *arg)
{
	char buffer[PATH_LENGTH];
	char *cmd_start, *cmd_end, *arg_start, *arg_end, *line_end;

	if(fgets(buffer, PATH_LENGTH, fp) == NULL) {
		return OHC_CONF_DONE;
	}

	cmd_start = strnonwhite(buffer);
	if(*cmd_start == '\0' || *cmd_start == '#') {
		return OHC_CONF_AGAIN;
	}

	cmd_end = strwhite(cmd_start);
	if(*cmd_end == '\0') {
		return "miss argument";
	}

	arg_start = strnonwhite(cmd_end);
	if(*arg_start == '\0') {
		return "miss argument";
	}

	arg_end = strwhite(arg_start);

	line_end = strnonwhite(arg_end);
	if(*line_end != '\0' && *line_end != '#') {
		return "too many arguments";
	}

	*cmd_end = *arg_end = '\0';
	memcpy(cmd, cmd_start, cmd_end - cmd_start + 1);
	memcpy(arg, arg_start, arg_end - arg_start + 1);
	return OHC_CONF_OK;
}

/* parse one file. @deep is used to control include-endless-loop. */
static int conf_parse_file(const char *filename, int deep)
{
	char cmd[COMMAND_LENGTH];
	char arg[ARGUMENT_LENGTH];
	ohc_conf_command_t *c;
	void *context;
	FILE *fp;
	int line = 0, i;
	const char *msg_rc;
	static int server_context_begin = -1;

	/* init server_context_begin */
	if(server_context_begin == -1) {
		for(i = 0; i < COMMAND_NUMBER; i++) {
			if(strcmp("listen", g_commands[i].name) == 0) {
				server_context_begin = i;
				break;
			}
		}
	}

	if(deep > 10) {
		log_error_admin(0, "configure file include too deep");
		return OHC_ERROR;
	}

	fp = fopen(filename, "r");
	if(fp == NULL) {
		log_error_admin(errno, "open conf file %s ", filename);
		return OHC_ERROR;
	}

	while(1) {
		/* read line from conf file */
		line++;
		msg_rc = conf_readline(fp, cmd, arg);
		if(msg_rc == OHC_CONF_AGAIN) {
			continue;
		}
		if(msg_rc != OHC_CONF_OK) {
			break;
		}

		/* "include" */
		if(strcmp(cmd, "include") == 0) {
			char *included_file = arg;
			char buffer[PATH_LENGTH];
			char *dir = strrchr(filename, '/');
			if(dir) {
				int len = dir - filename + 1;
				memcpy(buffer, filename, len);
				strncpy(buffer + len, arg, PATH_LENGTH - len);
				included_file = buffer;
			}

			if(conf_parse_file(included_file, deep + 1) != OHC_OK) {
				msg_rc = "error in include file";
				break;
			}
			continue;
		}

		/* normal commands */
		for(i = 0; i < COMMAND_NUMBER; i++) {
			c = &g_commands[i];
			if(strcmp(cmd, c->name) == 0) {
				break;
			}
		}

		if(i == COMMAND_NUMBER) {
			msg_rc = "no command match";
			break;
		}

		if(i < server_context_begin) {
			if(!list_empty(&conf_cycle.servers)) {
				msg_rc = "global command in server context";
				break;
			}
			context = &conf_cycle;
		} else {
			context = list_empty(&conf_cycle.servers)
				? &default_server
				: list_entry(conf_cycle.servers.prev, ohc_server_t, snode);
		}

		/* call handler */
		msg_rc = c->set_handler(c, context, arg);
		if(msg_rc != OHC_CONF_OK) {
			break;
		}
	}

	fclose(fp);

	if(msg_rc != OHC_CONF_DONE) {
		log_error_admin(0, "%s in line %d in conf file %s",
				msg_rc, line, filename);
		return OHC_ERROR;
	}
	return OHC_OK;
}

/* parse file and put the result into conf_cycle. entry of this file. */
ohc_conf_t *conf_parse(const char *filename)
{
	/* reuse the servers and devices, in order to avoid @conf_destroy. */
	static int first = 1;
	if(first) {
		first = 0;
	} else {
		list_splice(&conf_cycle.servers, &reserved_servers);
		list_splice(&conf_cycle.devices, &reserved_devices);
	}

	/* init conf */
	INIT_LIST_HEAD(&conf_cycle.devices);
	INIT_LIST_HEAD(&conf_cycle.servers);
	conf_cycle.threads = 4;
	conf_cycle.quit_timeout = 60;
	conf_cycle.device_badblock_percent = 1;
	conf_cycle.device_check_270G = 1;
	strcpy(conf_cycle.error_log, "error.log");

	/* init default_server */
	bzero(&default_server, sizeof(default_server));
	default_server.capacity = 0;
	default_server.connections_limit = 1000;
	default_server.send_timeout = 60;
	default_server.recv_timeout = 60;
	default_server.request_timeout = 60;
	default_server.keepalive_timeout = 60;
	default_server.item_max_size = 100 << 20; /*100M*/
	default_server.expire_default = 259200;  /*3days*/
	default_server.expire_force = 0;
	default_server.sndbuf = 0;
	default_server.rcvbuf = 0;
	default_server.server_dump = 1;
	default_server.shutdown_if_not_store = 0;
	default_server.key_include_query = 0;
	default_server.key_include_host = 0;
	default_server.key_include_ohc_key = 0;
	default_server.passby_enable = 0;
	default_server.passby_begin_item_nr = 1000*1000;
	default_server.passby_begin_consumed = 100L << 30; /*100G*/
	default_server.passby_limit_nr = 1000*1000;
	default_server.passby_expire = 3600;
	default_server.status_period = 60;
	strcpy(default_server.access_log, "access.log");

	/* parse */
	if(conf_parse_file(filename, 0) == OHC_ERROR) {
		return NULL;
	}
	return &conf_cycle;
}
