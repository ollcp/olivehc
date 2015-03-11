/*
 * Parse configure file.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_CONF_H_
#define _OHC_CONF_H_

#include "olivehc.h"

struct ohc_conf_s {
	int		threads;
	int		device_badblock_percent;
	ohc_flag_t	device_check_270G;
	time_t		quit_timeout;

	char		error_log[PATH_LENGTH];
	FILE		*error_filp;

	struct list_head	servers;
	struct list_head	devices;

};

ohc_conf_t *conf_parse(const char *filename);

#endif
