/**
 * \file notifications.c
 * \author Radek Krejci <rkrejci@cesnet.cz>
 * \brief Implementation of functions to handle NETCONF Notifications.
 *
 * Copyright (C) 2012 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#define _GNU_SOURCE
#define _BSD_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <poll.h>
#include <pthread.h>

#include <dbus/dbus.h>

#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "notifications.h"
#include "netconf_internal.h"
#include "messages_internal.h"
#include "netconf.h"
#include "session.h"
#include "config.h"

static const char rcsid[] __attribute__((used)) ="$Id: "__FILE__": "RCSID" $";

#define NCNTF_RULES_SIZE (1024*1024)
#define NCNTF_STREAMS_NS "urn:ietf:params:xml:ns:netmod:notification"

/* sleep time in dispatch loops in microseconds */
#define NCNTF_DISPATCH_SLEEP 100

#define NC_NTF_DBUS_PATH "/libnetconf/notifications/stream"
#define NC_NTF_DBUS_INTERFACE "libnetconf.notifications.stream"
static DBusConnection* dbus = NULL;
static pthread_mutex_t *dbus_mut = NULL;

/* path to the Event stream files, default path is defined in config.h */
static char* streams_path = NULL;

static pthread_key_t ncntf_replay_done;

/*
 * STREAM FILE FORMAT
 * char[8] == "NCSTREAM"
 * uint16_t 0xffxx - magic number to detect byte order and file format version (xx)
 * uint16_t len1;
 * char[len1] name; - this must correspond with the file name
 * uint16_t len2;
 * char[len2] description;
 * uint8_t replay;
 * uint64_t (time_t meanings) created;
 * char[] records;
 *
 */

/* magic bytes to recognize libnetconf's stream files */
#define MAGIC_NAME "NCSTREAM"
#define MAGIC_VERSION 0xFF01

struct stream {
	int fd_events;
	int fd_rules;
	char* name;
	char* desc;
	uint8_t replay;
	time_t created;
	int locked;
	char* rules;
	unsigned int data;
	struct stream *next;
};

/* status information of streams configuration */
static xmlDocPtr ncntf_config = NULL;

/* internal list of used streams with mutex to controll access into the list */
static struct stream *streams = NULL;
static pthread_mutex_t *streams_mut = NULL;

/* local function declaration */
static int ncntf_event_isallowed(const char* stream, const char* event);

/*
 * Modify the given list of files in the specified directory to keep only
 * regular files in the list. Parameter n specifies the number of items in the
 * list. The list is ussually obtained using scandir().
 *
 */
static void filter_reg_files(char* dirpath, struct dirent **filelist, int n)
{
	char* filepath;
	struct stat sb;

	assert(filelist != NULL);
	assert(dirpath != NULL);

	for(--n; n >= 0; n--) {
		if (filelist[n] == NULL) {
			continue;
		}
#ifdef _DIRENT_HAVE_D_TYPE
		if (filelist[n]->d_type == DT_UNKNOWN) {
			/* try another detection method -> use stat */
#endif
			/* d_type is not available -> use stat */
			if (asprintf(&filepath, "%s/%s", dirpath, filelist[n]->d_name) == -1) {
				ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
				free(filelist[n]);
				filelist[n] = NULL;
				continue;
			}
			if (stat(filepath, &sb) == -1) {
				ERROR("stat() failed on file %s - %s (%s:%d)", filepath, strerror(errno), __FILE__, __LINE__);
				free(filelist[n]);
				filelist[n] = NULL;
				free(filepath);
				continue;
			}
			free(filepath);
			if (!S_ISREG(sb.st_mode)) {
				/* the file is not a regular file containing stream events */
				free(filelist[n]);
				filelist[n] = NULL;
				continue;
			}
#ifdef _DIRENT_HAVE_D_TYPE
		} else if (filelist[n]->d_type != DT_REG) {
			/* the file is not a regular file containing stream events */
			free(filelist[n]);
			filelist[n] = NULL;
			continue;
		}
#endif
	}
}

/*
 * Check accessibility of the path to the given directory path with stream files.
 *
 * returns 0 on success, non-zero value else
 */
static int check_streams_path(char* path)
{
	struct stat sb;

	/* check accessibility of the path */
	if (access(path, F_OK|R_OK|W_OK) != 0) {
		if (errno == ENOENT) {
			/* path does not exist -> create it */
			if (mkdir(path, 0777) == -1) {
				WARN("Unable to create Events streams directory %s (%s).", path, strerror(errno));
				return (EXIT_FAILURE);
			}
			return (EXIT_SUCCESS);
		}
		WARN("Unable to access Events streams directory %s (%s).", path, strerror(errno));
		return (EXIT_FAILURE);
	} else {
		/* check that the file is directory */
		if (stat(path, &sb) == -1) {
			WARN("Unable to get information about Events streams directory %s (%s).", path, strerror(errno));
			return (EXIT_FAILURE);
		}
		if (!S_ISDIR(sb.st_mode)) {
			WARN("Events streams directory path %s exists, but it is not a directory.", path);
			return (EXIT_FAILURE);
		}
		return (EXIT_SUCCESS);
	}
}

/*
 * Set path to the directory with the stream files. It can be set by
 * environment variable defined as macro STREAMS_PATH_ENV (LIBNETCONF_STREAMS).
 * If this variable is not defined, default value from macro STREAMS_PATH_DEF
 * (/var/run/netconf_events) is used.
 *
 * returns 0 on success, non-zero value else
 */
static int set_streams_path()
{
	char* env;

	/* forgot previously set value (do not free, it is static) */
	streams_path = NULL;

	/* try to get path from the environment variable */
	env = getenv(NCNTF_STREAMS_PATH_ENV);
	if (env != NULL) {
		VERB("Checking Events stream path %s from %s environment variable.", env, NCNTF_STREAMS_PATH_ENV);
		if (check_streams_path(env) == 0) {
			streams_path = env;
		}
	}
	if (streams_path == NULL) {
		/* try to use default path */
		VERB("Checking default Events stream path %s.", NCNTF_STREAMS_PATH);
		if (check_streams_path(NCNTF_STREAMS_PATH) == 0) {
			streams_path = NCNTF_STREAMS_PATH;
		}
	}

	if (streams_path == NULL) {
		return (EXIT_FAILURE);
	} else {
		return (EXIT_SUCCESS);
	}
}

static xmlDocPtr streams_to_xml(void)
{
	xmlDocPtr config;
	xmlNodePtr node_streams, node_stream;
	struct stream *s;
	char* time;

	/* create empty configuration */
	config = xmlNewDoc(BAD_CAST "1.0");
	xmlDocSetRootElement(config, xmlNewNode(NULL, BAD_CAST "netconf"));
	xmlNewNs(config->children, BAD_CAST NCNTF_STREAMS_NS, NULL);
	node_streams = xmlAddChild(config->children, xmlNewNode(NULL, BAD_CAST "streams"));

	for (s = streams; s != NULL; s = s->next) {
		node_stream = xmlAddChild(node_streams, xmlNewNode(NULL, BAD_CAST "stream"));
		xmlNewChild(node_stream, NULL, BAD_CAST "name", BAD_CAST s->name);
		xmlNewChild(node_stream, NULL, BAD_CAST "description", BAD_CAST s->desc);
		xmlNewChild(node_stream, NULL, BAD_CAST "replaySupport", (s->replay == 1) ? BAD_CAST "true" : BAD_CAST "false");
		if (s->replay == 1) {
			time = nc_time2datetime(s->created);
			xmlNewChild(node_stream, NULL, BAD_CAST "replayLogCreationTime", BAD_CAST time);
			free (time);
		}
	}

	return (config);
}

static int map_rules(struct stream *s)
{
	mode_t mask;
	char* filepath = NULL;
	ssize_t r;

	assert(s != NULL);
	assert(s->rules == NULL);

	if (streams_path == NULL) {
		return (EXIT_FAILURE);
	}

	if (s->fd_rules == -1) {
		if (asprintf(&filepath, "%s/%s.rules", streams_path, s->name) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			return (EXIT_FAILURE);
		}
		mask = umask(0000);
		/* check if file with the rules exists */
		if (access(filepath, F_OK) != 0) {
			/* file does not exist, create it */
			if ((s->fd_rules = open(filepath, O_CREAT|O_RDWR|O_EXCL, 0777)) == -1) {
				if (errno != EEXIST) {
					ERROR("Unable to open Events stream rules file %s (%s)", filepath, strerror(errno));
					return (EXIT_FAILURE);
				}
				/* else file exists (someone already created it) just open it */
			} else {
				/* create sparse file */
				lseek(s->fd_rules, NCNTF_RULES_SIZE -1, SEEK_END);
				while (((r = write(s->fd_rules, &"\0", 1)) == -1) && (errno == EAGAIN ||errno == EINTR));
				if (r == -1) {
					WARN("Creating sparse stream event rules file failed (%s).", strerror(errno));
				}
				lseek(s->fd_rules, 0, SEEK_SET);
			}
		}
		if (s->fd_rules == -1) {
			s->fd_rules = open(filepath, O_RDWR);
		}
		umask(mask);
		if (s->fd_rules == -1) {
			ERROR("Unable to open Events stream rules file %s (%s)", filepath, strerror(errno));
			free(filepath);
			return (EXIT_FAILURE);
		}
		free(filepath);
	}

	s->rules = mmap(NULL, NCNTF_RULES_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd_rules, 0);
	if (s->rules == MAP_FAILED) {
		/* something bad happend */
		ERROR("mmapping Events stream rules file failed (%s)", strerror(errno));
		return (EXIT_FAILURE);
	} else {
		return (EXIT_SUCCESS);
	}
}

/*
 * Create a new stream file and write the header corresponding to the given
 * stream structure. If the file is already opened (stream structure has file
 * descriptor), it only rewrites the header of the file. All data from the
 * existing file are lost!
 *
 * returns 0 on success, non-zero value else
 */
static int write_fileheader(struct stream *s)
{
	char* filepath = NULL, *header;
	uint16_t len, version = MAGIC_VERSION;
	mode_t mask;
	uint64_t t;
	ssize_t r;
	size_t hlen = 0, offset = 0;

	/* check used variables */
	assert(s != NULL);
	assert(s->name != NULL);

	if (streams_path == NULL) {
		return (EXIT_FAILURE);
	}

	/* check if the corresponding file is already opened */
	if (s->fd_events == -1) {
		/* open and create/truncate the file */
		if (asprintf(&filepath, "%s/%s.events", streams_path, s->name) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			return (EXIT_FAILURE);
		}
		mask = umask(0000);
		s->fd_events = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0777);
		umask(mask);
		if (s->fd_events == -1) {
			ERROR("Unable to create Events stream file %s (%s)", filepath, strerror(errno));
			free(filepath);
			return (EXIT_FAILURE);
		}
		free(filepath);
	} else {
		/* truncate the file */
		if (ftruncate(s->fd_events, 0) == -1) {
			ERROR("ftruncate() failed on stream file \'%s\' failed (%s).", s->name, strerror(errno));
			return (EXIT_FAILURE);
		}
		lseek(s->fd_events, 0, SEEK_SET);
	}

	/* prepare the header */
	hlen = strlen(MAGIC_NAME) + ((s->desc == NULL) ? 0 : strlen(s->desc)) + strlen(s->name) + sizeof(uint8_t) + (4 * sizeof(uint16_t)) + sizeof(uint64_t) + 2;
	header = malloc(hlen);

	/* magic bytes */
	memcpy(header + offset, &MAGIC_NAME, strlen(MAGIC_NAME));
	offset += strlen(MAGIC_NAME);

	/* version */
	memcpy(header + offset, &version, sizeof(uint16_t));
	offset += sizeof(uint16_t);

	/* stream name */
	len = (uint16_t) strlen(s->name) + 1;
	memcpy(header + offset, &len, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	memcpy(header + offset, s->name, len);
	offset += len;

	/* stream description */
	if (s->desc != NULL) {
		len = (uint16_t) strlen(s->desc) + 1;
		memcpy(header + offset, &len, sizeof(uint16_t));
		offset += sizeof(uint16_t);
		memcpy(header + offset, s->desc, len);
		offset += len;
	} else {
		/* no description */
		len = 1;
		memcpy(header + offset, &len, sizeof(uint16_t));
		offset += sizeof(uint16_t);
		memcpy(header + offset, "", len);
		offset += len;
	}
	/* replay flag */
	memcpy(header + offset, &(s->replay), sizeof(uint8_t));
	offset += sizeof(uint8_t);

	/* creation time */
	t = (uint64_t) s->created;
	memcpy(header + offset, &t, sizeof(uint64_t));
	offset += sizeof(uint64_t);

	/* check expected and prepared length */
	if (offset != hlen) {
		WARN("%s: prepared stream file header length differs expected length (%zd:%zd)", __func__, offset, hlen);
	}

	/* write the header */
	while (((r = write(s->fd_events, &header, offset)) == -1) && (errno == EAGAIN ||errno == EINTR));
	if (r == -1) {
		WARN("Writing stream event file header failed (%s).", strerror(errno));
		if (ftruncate(s->fd_events, 0) == -1) {
			ERROR("ftruncate() failed on stream file \'%s\' failed (%s).", s->name, strerror(errno));
		}
		free(header);
		return (EXIT_FAILURE);
	}
	free(header);

	/* set where the data starts */
	s->data = lseek(s->fd_events, 0, SEEK_CUR);

	return (EXIT_SUCCESS);
}

/*
 * Read file header and fill the stream structure of the stream file specified
 * as filepath. If file is not proper libnetconf's stream file, NULL is
 * returned.
 */
static struct stream *read_fileheader(const char* filepath)
{
	struct stream *s;
	int fd;
	char magic_name[strlen(MAGIC_NAME)];
	uint16_t magic_number;
	uint16_t len;
	uint64_t t;
	int r;

	/* open the file */
	fd = open(filepath, O_RDWR);
	if (fd == -1) {
		ERROR("Unable to open Events stream file %s (%s)", filepath, strerror(errno));
		return (NULL);
	}

	/* create and fill the stream structure according to the file header */
	s = malloc(sizeof(struct stream));
	s->fd_events = fd;
	/* check magic bytes */
	if ((r = read(s->fd_events, &magic_name, strlen(MAGIC_NAME))) <= 0) {
		goto read_fail;
	}
	if (strncmp(magic_name, MAGIC_NAME, strlen(MAGIC_NAME)) != 0) {
		/* file is not of libnetconf's stream file format */
		free(s);
		return (NULL);
	}
	if ((r = read(s->fd_events, &magic_number, sizeof(uint16_t))) <= 0) {
		goto read_fail;
	}
	/* \todo: handle different endianity and versions */

	/* read the stream name */
	if ((r = read(s->fd_events, &len, sizeof(uint16_t))) <= 0) {
		goto read_fail;
	}
	s->name = malloc(len * sizeof(char));
	if ((r = read(s->fd_events, s->name, len)) <= 0) {
		goto read_fail;
	}
	/* read the description of the stream */
	if ((r = read(s->fd_events, &len, sizeof(uint16_t))) <= 0) {
		goto read_fail;
	}
	s->desc = malloc(len * sizeof(char));
	if ((r = read(s->fd_events, s->desc, len)) <= 0) {
		goto read_fail;
	}
	/* read the replay flag */
	if ((r = read(s->fd_events, &(s->replay), sizeof(uint8_t))) <= 0) {
		goto read_fail;
	}
	/* read creation time */
	if ((r = read(s->fd_events, &(t), sizeof(uint64_t))) <= 0) {
		goto read_fail;
	}
	s->created = (time_t)t;

	s->locked = 0;
	s->rules = NULL;
	s->fd_rules = -1;
	s->next = NULL;

	/* move to the end of the file */
	s->data = lseek(s->fd_events, 0, SEEK_CUR);

	return (s);

read_fail:
	ERROR("Reading stream file header failed (%s).", (r < 0) ? strerror(errno) : "Unexpected end of file");
	close (fd);
	free(s);
	return (NULL);
}

/*
 * free the stream structure
 */
static void ncntf_stream_free(struct stream *s)
{
	if (s == NULL) {
		return;
	}

	if (s->desc != NULL) {
		free(s->desc);
	}
	if (s->name != NULL) {
		free(s->name);
	}
	if (s->fd_events != -1) {
		close(s->fd_events);
	}
	free(s);
}

/*
 * Get the stream structure according to the given stream name
 */
static struct stream* ncntf_stream_get(const char* stream)
{
	struct stream *s;
	char* filepath;

	if (stream == NULL) {
		return (NULL);
	}

	/* search for the specified stream in the list according to the name */
	for (s = streams; s != NULL; s = s->next) {
		if (strcmp(s->name, stream) == 0) {
			/* the specified stream does exist */
			return (s);
		}
	}

	/*
	 * the stream was not found in the current list - try to look at the
	 * stream directory if the stream file wasn't created meanwhile
	 */
	if (s == NULL) {
		/* try to localize so far unrecognized stream file */
		if (asprintf(&filepath, "%s/%s.events", streams_path, stream) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			return (NULL);
		}
		if (((s = read_fileheader(filepath)) != NULL) && (map_rules(s) == 0)) {
			/* add the stream file into the stream list */
			s->next = streams;
			streams = s;
		} else if (s != NULL) {
			ERROR("Unable to map Event stream rules file into memory.");
			ncntf_stream_free(s);
			s = NULL;
		}
		free(filepath);
	}

	return (s);
}

/*
 * lock the stream file to avoid concurrent writing/reading from different
 * processes.
 */
static int ncntf_stream_lock(struct stream *s)
{
	off_t offset;

	/* this will be blocking, but all these locks should be short-term */
	offset = lseek(s->fd_events, 0, SEEK_CUR);
	lseek(s->fd_events, 0, SEEK_SET);
	if (lockf(s->fd_events, F_LOCK, 0) == -1) {
		lseek(s->fd_events, offset, SEEK_SET);
		ERROR("Stream file locking failed (%s).", strerror(errno));
		return (EXIT_FAILURE);
	}
	lseek(s->fd_events, offset, SEEK_SET);
	s->locked = 1;
	return (EXIT_SUCCESS);
}

/*
 * unlock the stream file after reading/writing
 */
static int ncntf_stream_unlock(struct stream *s)
{
	off_t offset;

	if (s->locked == 0) {
		/* nothing to do */
		return (EXIT_SUCCESS);
	}

	offset = lseek(s->fd_events, 0, SEEK_CUR);
	lseek(s->fd_events, 0, SEEK_SET);
	if (lockf(s->fd_events, F_ULOCK, 0) == -1) {
		lseek(s->fd_events, offset, SEEK_SET);
		ERROR("Stream file unlocking failed (%s).", strerror(errno));
		return (EXIT_FAILURE);
	}
	lseek(s->fd_events, offset, SEEK_SET);
	s->locked = 0;
	return (EXIT_SUCCESS);
}

/*
 * Initialize D-Bus communication
 */
static int ncntf_dbus_init(void)
{
	DBusError dbus_err;

	DBG_LOCK("dbus_mut");
	pthread_mutex_lock(dbus_mut);
	if (dbus == NULL) {
		/* initialize the errors */
		dbus_error_init(&dbus_err);

		/* connect to the D-Bus */
		dbus = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_err);
		if (dbus_error_is_set(&dbus_err)) {
			ERROR("D-Bus connection error (%s)", dbus_err.message);
			dbus_error_free(&dbus_err);
		}
		if (dbus == NULL) {
			ERROR("Unable to connect to the D-Bus's system bus");
			DBG_UNLOCK("dbus_mut");
			pthread_mutex_unlock(dbus_mut);
			return (EXIT_FAILURE);
		}
	}
	DBG_UNLOCK("dbus_mut");
	pthread_mutex_unlock(dbus_mut);

	return (EXIT_SUCCESS);
}

/*
 * close D-Bus communication channel
 */
static void ncntf_dbus_close(void)
{
	DBG_LOCK("dbus_mut");
	pthread_mutex_lock(dbus_mut);
	if (dbus != NULL) {
		dbus_connection_unref(dbus);
		dbus = NULL;
	}
	DBG_UNLOCK("dbus_mut");
	pthread_mutex_unlock(dbus_mut);
}

/*
 * Initiate the list of available streams. It opens all accessible stream files
 * from the stream directory.
 *
 * If the function is called repeatedly, stream files are closed and opened
 * again - current processing like iteration of the events in the stream must
 * start again.
 */
static int ncntf_streams_init(void)
{
	int n;
	struct dirent **filelist;
	struct stream *s = NULL;
	char* filepath;

	if (ncntf_config != NULL) {
		/* we are already initialized */
		return(EXIT_SUCCESS);
	}

	/* check the streams path */
	if (streams_path == NULL && set_streams_path() != 0) {
		return (EXIT_FAILURE);
	}

	/*
	 * lock the whole initialize operation, not only streams variable
	 * manipulation since this starts a complete work with the streams
	 */
	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);

	/* explore the stream directory */
	n = scandir(streams_path, &filelist, NULL, alphasort);
	if (n < 0) {
		ERROR("Unable to read from Events streams directory %s (%s).", streams_path, strerror(errno));
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		return (EXIT_FAILURE);
	}
	/* keep only regular files that could store the events stream */
	filter_reg_files(streams_path, filelist, n);
	/* and open all libnetconf's stream files - file's magic number is checked */
	for(--n; n >= 0; n--) {
		if (filelist[n] == NULL) { /* was not a regular file */
			continue;
		}

		if (asprintf(&filepath, "%s/%s", streams_path, filelist[n]->d_name) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			free(filelist[n]);
			continue;
		}
		if ((s = read_fileheader(filepath)) != NULL && (map_rules(s) == 0)) {
			/* add the stream file into the stream list */
			s->next = streams;
			streams = s;
		} else if (s != NULL) {
			ERROR("Unable to map Event stream rules file into memory.");
			ncntf_stream_free(s);
			s = NULL;
		}/* else - not an event stream file */
		free(filepath);

		/* free the directory entry information */
		free(filelist[n]);
	}

	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);
	free(filelist);

	/* dump streams into xml status data */
	if ((ncntf_config = streams_to_xml()) == NULL) {
		return (EXIT_FAILURE);
	}

	if (ncntf_stream_isavailable(NCNTF_STREAM_BASE) == 0) {
		/* create default NETCONF stream if does not exist */
		ncntf_stream_new(NCNTF_STREAM_BASE, "NETCONF Base Notifications", 1);
		/* allow notifications defined in RFC 6470 */
		ncntf_stream_allow_events(NCNTF_STREAM_BASE, "netconf-config-change");
		ncntf_stream_allow_events(NCNTF_STREAM_BASE, "netconf-capability-change");
		ncntf_stream_allow_events(NCNTF_STREAM_BASE, "netconf-session-start");
		ncntf_stream_allow_events(NCNTF_STREAM_BASE, "netconf-session-end");
		ncntf_stream_allow_events(NCNTF_STREAM_BASE, "netconf-confirmed-commit");
	}

	return (EXIT_SUCCESS);
}

/*
 * close all opened streams in the global list
 */
static void ncntf_streams_close(void)
{
	struct stream *s;

	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);
	s = streams;
	while(s != NULL) {
		streams = s->next;
		ncntf_stream_free(s);
		s = streams;
	}
	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);
}

int ncntf_init(void)
{
	int ret, r;
	pthread_mutexattr_t mattr;

	if (ncntf_config != NULL) {
		/* we are already initialized */
		return(EXIT_SUCCESS);
	}

	/* init streams' mutex if needed */
	if (streams_mut == NULL) {
		if (pthread_mutexattr_init(&mattr) != 0) {
			ERROR("Memory allocation failed (%s:%d).", __FILE__, __LINE__);
			return (EXIT_FAILURE);
		}
		if ((streams_mut = malloc(sizeof(pthread_mutex_t))) == NULL) {
			ERROR("Memory allocation failed (%s:%d).", __FILE__, __LINE__);
			pthread_mutexattr_destroy(&mattr);
			return (EXIT_FAILURE);
		}
		/*
		 * mutex MUST be recursive since we use it to cover update
		 * operation and it internally calls sequence of close and init
		 */
		pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
		if ((r = pthread_mutex_init(streams_mut, &mattr)) != 0) {
			ERROR("Mutex initialization failed (%s).", strerror(r));
			pthread_mutexattr_destroy(&mattr);
			return (EXIT_FAILURE);
		}
		pthread_mutexattr_destroy(&mattr);
	}

	/* init dbus's mutex if needed */
	if (dbus_mut == NULL) {
		if (pthread_mutexattr_init(&mattr) != 0) {
			ERROR("Memory allocation failed (%s:%d).", __FILE__, __LINE__);
			return (EXIT_FAILURE);
		}
		if ((dbus_mut = malloc(sizeof(pthread_mutex_t))) == NULL) {
			ERROR("Memory allocation failed (%s:%d).", __FILE__, __LINE__);
			pthread_mutexattr_destroy(&mattr);
			return (EXIT_FAILURE);
		}
		/*
		 * mutex MUST be recursive since we use it this way
		 */
		pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
		if ((r = pthread_mutex_init(dbus_mut, &mattr)) != 0) {
			ERROR("Mutex initialization failed (%s).", strerror(r));
			pthread_mutexattr_destroy(&mattr);
			return (EXIT_FAILURE);
		}
		pthread_mutexattr_destroy(&mattr);
	}

	pthread_key_create(&ncntf_replay_done, free);

	/* initiate DBus communication */
	if ((ret = ncntf_dbus_init()) != 0) {
		return (ret);
	}

	/* initiate streams */
	if ((ret = ncntf_streams_init()) != 0) {
		return (ret);
	}

	return (EXIT_SUCCESS);
}

char* ncntf_status(void)
{
	xmlChar* data;

	xmlDocDumpFormatMemory(ncntf_config, &data, NULL, 1);
	return ((char*)data);
}

void ncntf_close(void)
{
	if (ncntf_config != NULL) {
		xmlFreeDoc(ncntf_config);
		ncntf_config = NULL;

		ncntf_dbus_close();
		ncntf_streams_close();
		pthread_mutex_destroy(streams_mut);
		pthread_mutex_destroy(dbus_mut);
		free(streams_mut);
		streams_mut = NULL;
		free(dbus_mut);
		dbus_mut = NULL;
	}
}

int ncntf_stream_new(const char* name, const char* desc, int replay)
{
	struct stream *s;
	xmlDocPtr oldconfig;

	if (ncntf_config == NULL) {
		return (EXIT_FAILURE);
	}

	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);

	/* check the stream name if the requested stream already exists */
	for (s = streams; s != NULL; s = s->next) {
		if (strcmp(name, s->name) == 0) {
			WARN("Requested new stream \'%s\' already exists.", name);
			DBG_UNLOCK("streams_mut");
			pthread_mutex_unlock(streams_mut);
			return (EXIT_FAILURE);
		}
	}

	s = malloc(sizeof(struct stream));
	if (s == NULL) {
		ERROR("Memory allocation failed - %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		return (EXIT_FAILURE);
	}
	s->name = strdup(name);
	s->desc = strdup(desc);
	s->replay = replay;
	s->created = time(NULL);
	s->locked = 0;
	s->next = NULL;
	s->rules = NULL;
	s->fd_events = -1;
	s->fd_rules = -1;
	if (write_fileheader(s) != 0 || map_rules(s) != 0) {
		ncntf_stream_free(s);
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		return (EXIT_FAILURE);
	} else {
		/* add created stream into the list */
		s->next = streams;
		streams = s;
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		oldconfig = ncntf_config;
		ncntf_config = streams_to_xml();
		xmlFreeDoc(oldconfig);
		return (EXIT_SUCCESS);
	}
}

int ncntf_stream_allow_events(const char* stream, const char* event)
{
	char* end;
	struct stream* s;

	if (stream == NULL || event == NULL) {
		return (EXIT_FAILURE);
	}

	if (ncntf_event_isallowed(stream, event)) {
		return (EXIT_SUCCESS);
	}

	if ((s = ncntf_stream_get(stream)) == NULL) {
		/* stream does not exist or some error occurred */
		return (EXIT_FAILURE);
	}

	/* create new rule */
	end = strrchr(s->rules, '\n');
	if (end == NULL) {
		/* rules is empty */
		end = s->rules;
	} else {
		end++;
	}
	strcpy(end, event);
	strcpy(end + strlen(event), "\n");

	return (EXIT_SUCCESS);
}

char** ncntf_stream_list(void)
{
	char** list;
	struct stream *s;
	int i;

	if (ncntf_config == NULL) {
		return (NULL);
	}

	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);

	for (s = streams, i = 0; s != NULL; s = s->next, i++);
	list = calloc(i + 1, sizeof(char*));
	if (list == NULL) {
		ERROR("Memory allocation failed - %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		return (NULL);
	}
	for (s = streams, i = 0; s != NULL; s = s->next, i++) {
		list[i] = strdup(s->name);
	}
	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);

	return(list);
}

int ncntf_stream_isavailable(const char* name)
{
	struct stream *s;

	if (ncntf_config == NULL || name == NULL) {
		return(0);
	}

	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);
	for (s = streams; s != NULL; s = s->next) {
		if (strcmp(s->name, name) == 0) {
			/* the specified stream does exist */
			DBG_UNLOCK("streams_mut");
			pthread_mutex_unlock(streams_mut);
			return (1);
		}
	}
	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);

	return (0); /* the stream does not exist */
}

void ncntf_stream_iter_start(const char* stream)
{
	struct stream *s;
	char* dbus_filter = NULL;
	DBusError err;
	int *replay_done;

	if (ncntf_config == NULL) {
		return;
	}

	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);
	if ((s = ncntf_stream_get(stream)) == NULL) {
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		return;
	}
	lseek(s->fd_events, s->data, SEEK_SET);
	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);

	/* subscribe DBus signals for the stream */
	if (asprintf(&dbus_filter, "type='signal',interface='%s',path='%s/%s',member='Event'",
			NC_NTF_DBUS_INTERFACE, NC_NTF_DBUS_PATH, stream) == -1) {
		WARN("asprintf() failed (%s:%d).", __FILE__, __LINE__);
	}
	dbus_error_init(&err);

	DBG_LOCK("dbus_mut");
	pthread_mutex_lock(dbus_mut);
	dbus_bus_add_match(dbus, dbus_filter, &err);
	dbus_connection_flush(dbus);
	DBG_UNLOCK("dbus_mut");
	pthread_mutex_unlock(dbus_mut);

	free(dbus_filter);

	if (dbus_error_is_set(&err)) {
		WARN("%s", err.message);
		dbus_error_free(&err);
	}

	replay_done = malloc(sizeof(int));
	*replay_done = 0;
	pthread_setspecific(ncntf_replay_done, (void*)replay_done);
}

void ncntf_stream_iter_finnish(const char* stream)
{
	char* dbus_filter = NULL;
	DBusError err;
	int *replay_done;

	/* unsubscribe DBus */
	if (asprintf(&dbus_filter, "type='signal',interface='%s',path='%s/%s',member='Event'",
			NC_NTF_DBUS_INTERFACE, NC_NTF_DBUS_PATH, stream) == -1) {
		WARN("asprintf() failed (%s:%d).", __FILE__, __LINE__);
	}
	dbus_error_init(&err);

	DBG_LOCK("dbus_mut");
	pthread_mutex_lock(dbus_mut);
	dbus_bus_remove_match(dbus, dbus_filter, &err);
	dbus_connection_flush(dbus);
	DBG_UNLOCK("dbus_mut");
	pthread_mutex_unlock(dbus_mut);

	free(dbus_filter);

	if (dbus_error_is_set(&err)) {
		WARN("%s", err.message);
		dbus_error_free(&err);
	}
	replay_done = (int*) pthread_getspecific(ncntf_replay_done);
	*replay_done = 0;
}

/*
 * Pop the next event record from the stream file.
 *
 * \todo: thread safety (?thread-specific variables)
 */
char* ncntf_stream_iter_next(const char* stream, time_t start, time_t stop, time_t *event_time)
{
	struct stream *s;
	int32_t len;
	uint64_t t;
	off_t offset;
	char* text = NULL;
	DBusMessage *signal;
	DBusMessageIter signal_args;
	int* replay_done;
	char* time_s;
	time_t tnow;
	int r;

	if (ncntf_config == NULL) {
		return (NULL);
	}

	/* check time boundaries */
	if ((start != -1) && (stop != -1) && (stop < start)) {
		return (NULL);
	}

	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);
	if ((s = ncntf_stream_get(stream)) == NULL) {
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		return (NULL);
	}

	replay_done = (int*) pthread_getspecific(ncntf_replay_done);
	if (start == -1) {
		*replay_done = 1;
	}

	while (1) {
		/* condition to read events from file (use replay):
		 * 1) startTime is specified
		 * 2) stream has replay option allowed
		 * 3) there are still some data to read from stream file
		 */
		if ((*replay_done == 0) && (start != -1) && (s->replay == 1) && (offset = lseek(s->fd_events, 0, SEEK_CUR)) < lseek(s->fd_events, 0, SEEK_END)) {
			/* there are still some data to read */
			lseek(s->fd_events, offset, SEEK_SET);
		} else {
			/* no more data */
			DBG_UNLOCK("streams_mut");
			pthread_mutex_unlock(streams_mut);
			if (*replay_done == 0) {
				/* send replayComplete notification */
				*replay_done = 1;
				if (asprintf(&text, "<notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\">"
							"<eventTime>%s</eventTime><replayComplete/></notification>", time_s = nc_time2datetime(tnow = time(NULL))) == -1) {
					ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
					WARN("Sending replayComplete failed due to previous error.");
					text = NULL;
				}
				free(time_s);
				if (event_time != NULL) {
					*event_time = tnow;
				}
				return (text);
			}

			/* try DBus */
			while (ncntf_config != NULL) {
				DBG_LOCK("dbus_mut");
				pthread_mutex_lock(dbus_mut);
				if (dbus_connection_read_write(dbus, 10) != 1) {
					/* dbus connection is closed */
					ERROR("DBus connection unexpectedly closed.");
					break;
				}
				signal = dbus_connection_pop_message(dbus);
				DBG_UNLOCK("dbus_mut");
				pthread_mutex_unlock(dbus_mut);

				if (signal != NULL && dbus_message_is_signal(signal, NC_NTF_DBUS_INTERFACE, "Event")) {
					/* parse the message, according to the
					 * filter set in nc_ntf_stream_iter_start(),
					 * we have Event signal from the stream
					 * interface of the specified stream
					 */
					/* read the parameters */
					if (dbus_message_iter_init(signal, &signal_args)) {
						if (DBUS_TYPE_UINT64 != dbus_message_iter_get_arg_type(&signal_args)) {
							WARN("Unexpected DBus Event signal (timestamp is missing).");
							dbus_message_unref(signal);
							continue;
						}
						dbus_message_iter_get_basic(&signal_args, &t);
						/* check boundaries */
						if ((start != -1) && (start > (time_t)t)) {
							/*
							 * we're not interested in this event, it
							 * happened before specified start time
							 */
							dbus_message_unref(signal);
							continue; /* try next signal */
						}
						if ((stop != -1) && (stop < (time_t)t)) {
							/*
							 * we're not interested in this event, it
							 * happened after specified stop time
							 */
							dbus_message_unref(signal);
							continue; /* try next signal */
						}
						/* we're interested, read content */
						dbus_message_iter_next (&signal_args);
						if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&signal_args)) {
							WARN("Unexpected DBus Event signal (content is missing).");
							dbus_message_unref(signal);
							continue;
						}
						dbus_message_iter_get_basic(&signal_args, &text);
						dbus_message_unref(signal);
						if (event_time != NULL) {
							*event_time = (time_t)t;
						}
						return(strdup(text));
					}
				}
				/* else signal == NULL */
				break;
			}

			/* no more events */
			return (NULL);
		}

		if (ncntf_stream_lock(s) == 0) {
			if ((r = read(s->fd_events, &len, sizeof(int32_t))) <= 0) {
				ERROR("Reading stream file failed (%s).", (r < 0) ? strerror(errno) : "Unexpected end of file");
				return (NULL);
			}
			if ((r = read(s->fd_events, &t, sizeof(uint64_t))) <= 0) {
				ERROR("Reading stream file failed (%s).", (r < 0) ? strerror(errno) : "Unexpected end of file");
				return (NULL);
			}

			/* check boundaries */
			if ((start != -1) && (start > (time_t)t)) {
				/*
				 * we're not interested in this event, it
				 * happened before specified start time
				 */
				lseek(s->fd_events, len, SEEK_CUR);
				/* read another event */
				ncntf_stream_unlock(s);
				continue;
			}
			if ((stop != -1) && (stop < (time_t)t)) {
				/*
				 * we're not interested in this event, it
				 * happened after specified stop time
				 */
				lseek(s->fd_events, len, SEEK_CUR);
				/* read another event */
				ncntf_stream_unlock(s);
				continue;
			}

			/* we're interested, read content */
			text = malloc(len * sizeof(char));
			if ((r = read(s->fd_events, text, len)) <= 0) {
				ERROR("Reading stream file failed (%s).", (r < 0) ? strerror(errno) : "Unexpected end of file");
				return (NULL);
			}
			ncntf_stream_unlock(s);
			break; /* end the reading loop */
		} else {
			ERROR("Unable to read event from stream file %s (locking failed).", s->name);
			DBG_UNLOCK("streams_mut");
			pthread_mutex_unlock(streams_mut);
			return (NULL);
		}
	}

	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);

	if (event_time != NULL) {
		*event_time = (time_t)t;
	}
	return (text);
}


static void ncntf_event_stdoutprint (time_t eventtime, const char* content)
{
	char* t = NULL;

	fprintf(stdout, "eventTime: %s\n%s\n", t = nc_time2datetime(eventtime), content);
	if (t != NULL) {
		free(t);
	}
}

static int ncntf_event_isallowed(const char* stream, const char* event)
{
	struct stream* s;
	char* token;
	char* rules;

	if (stream == NULL || event == NULL) {
		return (0);
	}

	if ((s = ncntf_stream_get(stream)) == NULL) {
		/* stream does not exist or some error occurred */
		return (0);
	}

	rules = strdup(s->rules);
	for (token = strtok(rules, "\n"); token != NULL; token = strtok(NULL, "\n")) {
		if (strcmp(event, token) == 0) {
			free(rules);
			return(1);
		}
	}
	free(rules);

	/* specified event is not allowed in the stream */
	return (0);
}

/**
 * @ingroup notifications
 * @brief Store new event into the specified stream. Parameters are specific
 * for different events.
 *
 * ### Event parameters:
 * - #NCNTF_GENERIC
 *  - **const char* content** Content of the notification as defined in RFC 5277.
 *  eventTime is added automatically. The string should be XML formatted.
 * - #NCNTF_BASE_CFG_CHANGE
 *  - #NC_DATASTORE **datastore** Specify which datastore has changed.
 *  - #NCNTF_EVENT_BY **changed_by** Specify the source of the change.
 *   - If the value is set to #NCNTF_EVENT_BY_USER, following parameter is
 *   required:
 *  - **const struct nc_session* session** Session required the configuration change.
 * - #NCNTF_BASE_CPBLT_CHANGE
 *  - **const struct nc_cpblts* old** Old list of capabilities.
 *  - **const struct nc_cpblts* new** New list of capabilities.
 *  - #NCNTF_EVENT_BY **changed_by** Specify the source of the change.
 *   - If the value is set to #NCNTF_EVENT_BY_USER, following parameter is
 *   required:
 *  - **const struct nc_session* session** Session required the configuration change.
 * - #NCNTF_BASE_SESSION_START
 *  - **const struct nc_session* session** Started session (#NC_SESSION_STATUS_DUMMY session is also allowed).
 * - #NCNTF_BASE_SESSION_END
 *  - **const struct nc_session* session** Finnished session (#NC_SESSION_STATUS_DUMMY session is also allowed).
 *  - #NC_SESSION_TERM_REASON **reason** Session termination reason.
 *   - If the value is set to #NC_SESSION_TERM_KILLED, following parameter is
 *   required.
 *  - **const char* killed-by-sid** The ID of the session that directly caused
 *  the session termination. If the session was terminated by a non-NETCONF
 *  process unknown to the server, use NULL as the value.
 *
 * ### Examples:
 * - nc_ntf_event_new("mystream", -1, NCNTF_GENERIC, "<event>something happend</event>");
 * - nc_ntf_event_new("netconf", -1, NCNTF_BASE_CFG_CHANGE, NC_DATASTORE_RUNNING, NCNTF_EVENT_BY_USER, my_session);
 * - nc_ntf_event_new("netconf", -1, NCNTF_BASE_CPBLT_CHANGE, old_cpblts, new_cpblts, NCNTF_EVENT_BY_SERVER);
 * - nc_ntf_event_new("netconf", -1, NCNTF_BASE_SESSION_START, my_session);
 * - nc_ntf_event_new("netconf", -1, NCNTF_BASE_SESSION_END, my_session, NC_SESSION_TERM_KILLED, "123456");
 *
 * @param[in] etime Time of the event, if set to -1, current time is used.
 * @param[in] event Event type to distinguish following parameters.
 * @param[in] ... Specific parameters for different event types as described
 * above.
 * @return 0 for success, non-zero value else.
 */
int ncntf_event_new(time_t etime, NCNTF_EVENT event, ...)
{
	char *event_time = NULL, *signal_object = NULL;
	char *content = NULL, *record = NULL, *ename = NULL;
	char *aux1 = NULL, *aux2 = NULL;
	NC_DATASTORE ds;
	NCNTF_EVENT_BY by;
	const struct nc_cpblts *old, *new;
	const struct nc_session *session;
	NC_SESSION_TERM_REASON reason;
	struct stream* s;
	int32_t len;
	int poffset, i, j;
	ssize_t r;
	off_t offset;
	uint64_t etime64;
	va_list params;
	DBusMessage *signal = NULL;
	DBusMessageIter signal_args;
	xmlDocPtr edoc;
	int ret = EXIT_SUCCESS;

	/* check the stream */
	if (ncntf_config == NULL) {
		return (EXIT_FAILURE);
	}

	va_start(params, event);

	/* get the event description */
	DBG("Adding new event (%d)", event);
	switch (event) {
	case NCNTF_GENERIC:
		content = va_arg(params, char *);
		if (content != NULL) {
			content = strdup(content);
		} else {
			ERROR("Missing parameter content to create GENERIC event record.");
			va_end(params);
			return (EXIT_FAILURE);
		}
		break;
	case NCNTF_BASE_CFG_CHANGE:
		ds = va_arg(params, NC_DATASTORE);
		by = va_arg(params, NCNTF_EVENT_BY);

		/* check datastore parameter */
		switch (ds) {
		case NC_DATASTORE_STARTUP:
			aux1 = "startup";
			break;
		case NC_DATASTORE_RUNNING:
			aux1 = "running";
			break;
		default:
			/* invalid value */
			ERROR("Invalid \'datastore\' parameter of %s.", __func__);
			va_end(params);
			return (EXIT_FAILURE);
			break;
		}

		/* check change-by parameter */
		switch (by) {
		case NCNTF_EVENT_BY_SERVER:
			/* BY_USER must be created dynamically, so allocate it
			 * dynamically also in this case to have single free();
			 */
			aux2 = strdup("<server/>");
			break;
		case NCNTF_EVENT_BY_USER:
			/* another parameter should be passed */
			session = va_arg(params, const struct nc_session*);
			if (session == NULL) {
				ERROR("Invalid \'session\' parameter of %s.", __func__);
				va_end(params);
				return (EXIT_FAILURE);
			}
			if (asprintf(&aux2, "<username>%s</username>"
					"<session-id>%s</session-id>"
					"<source-host>%s</source-host>",
					session->username,
					session->session_id,
					session->hostname) == -1) {
				ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
				aux2 = NULL;
			}
			break;
		}

		/* no more parameters */
		va_end(params);

		if (asprintf(&content, "<netconf-config-change><datastore>%s</datastore>"
				"%s</netconf-config-change>",
				aux1, (aux2 == NULL) ? "" : aux2) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			free(aux2);
			return (EXIT_FAILURE);
		}
		free(aux2);

		break;
	case NCNTF_BASE_CPBLT_CHANGE:
		/* \todo */
		old = va_arg(params, const struct nc_cpblts*);
		new = va_arg(params, const struct nc_cpblts*);
		by = va_arg(params, NCNTF_EVENT_BY);

		/* find created capabilities */
		for(i = 0; new->list[i] != NULL; i++) {
			/*
			 * check if the capability contains parameter (starting
			 * with '?'). Then the length of the capability URI
			 * without parameters is stored and used in comparison
			 */
			if ((aux1 = strchr(new->list[i], '?')) != NULL) {
				/* there are capability's parameters */
				poffset = (int)(aux1 - (new->list[i]));
			} else {
				poffset = strlen(new->list[i]);
			}
			/*
			 * traverse old capabilities and find out if the
			 * current new one is there
			 */
			j = 0;
			while(old->list[j] != NULL) {
				if (strncmp(new->list[i], old->list[j], poffset) == 0) {
					break;
				}
				j++;
			}

			aux1 = NULL;
			/* process the result of searching */
			if (old->list[j] != NULL) {
				/*
				 * new->list[i] can be the same as old->list[j]
				 * or there are modified parameters
				 */
				if ((old->list[j][poffset] == '?' || old->list[j][poffset] == '\0')
						&& (strcmp(new->list[i], old->list[j]) != 0)) {
					if (asprintf(&aux1, "<modified-capability>%s</modified-capability>", new->list[i]) == -1) {
						ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
						aux1 = NULL;
					}
				}
			} else {
				/* aux1 is a new capability */
				if (asprintf(&aux1, "<added-capability>%s</added-capability>", new->list[i]) == -1) {
					ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
					aux1 = NULL;
				}
			}

			if (aux1 != NULL) {
				/* add new information to the previous one */
				if (aux2 != NULL) {
					aux2 = realloc(aux2, strlen(aux2) + strlen(aux1) + 1);
				} else {
					aux2 = calloc(strlen(aux1) + 1, sizeof(char));
				}
				strncat(aux2, aux1, strlen(aux1));
				free(aux1);
			}
		}

		/* find deleted capabilities */
		for (i = 0; old->list[i] != NULL; i++) {
			/*
			 * find the end of the basic capability URI to not take
			 * parameters into acount (this case was processed in
			 * previous loop
			 */
			if ((aux1 = strchr(old->list[i], '?')) != NULL) {
				/* there are capability's parameters */
				poffset = (int) (aux1 - (old->list[i]));
			} else {
				poffset = strlen(old->list[i]);
			}

			/*
			 * traverse new capabilities and find out if some of the
			 * old capabilities is removed
			 */
			j = 0;
			while(new->list[j] != NULL) {
				if (strncmp(new->list[j], old->list[i], poffset) == 0) {
					break;
				}
				j++;
			}

			/* process the result */
			if (new->list[j] == NULL) {
				aux1 = NULL;
				/* old->list[i] is deleted capability */
				if (asprintf(&aux1, "<deleted-capability>%s</deleted-capability>", old->list[i]) == -1) {
					ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
				} else {
					if (aux2 != NULL) {
						aux2 = realloc(aux2, strlen(aux2) + strlen(aux1) + 1);
					} else {
						aux2 = calloc(strlen(aux1) + 1, sizeof(char));
					}
					strncat(aux2, aux1, strlen(aux1));
					free(aux1);
				}
			}
		}

		/* check change-by parameter */
		switch (by) {
		case NCNTF_EVENT_BY_SERVER:
			/* BY_USER must be created dynamically, so allocate it
			 * dynamically also in this case to have single free();
			 */
			aux1 = strdup("<server/>");
			break;
		case NCNTF_EVENT_BY_USER:
			/* another parameter should be passed */
			session = va_arg(params, const struct nc_session*);
			if (session == NULL) {
				ERROR("Invalid \'session\' parameter of %s.", __func__);
				va_end(params);
				return (EXIT_FAILURE);
			}
			if (asprintf(&aux1, "<username>%s</username>"
					"<session-id>%s</session-id>"
					"<source-host>%s</source-host>",
					session->username,
					session->session_id,
					session->hostname) == -1) {
				ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
				aux1 = NULL;
			}
			break;
		}

		/* no more parameters */
		va_end(params);

		if (asprintf(&content, "<netconf-capability-change>%s%s</netconf-capability-change>",
				(aux1 == NULL) ? "" : aux1,
				(aux2 == NULL) ? "" : aux2) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			free(aux1);
			free(aux2);
			return (EXIT_FAILURE);
		}
		free(aux1);
		free(aux2);
		break;
	case NCNTF_BASE_SESSION_START:
		session = va_arg(params, const struct nc_session*);
		if (session == NULL) {
			ERROR("Invalid \'session\' parameter of %s.", __func__);
			va_end(params);
			return (EXIT_FAILURE);
		}
		if (asprintf(&content, "<netconf-session-start><username>%s</username>"
				"<session-id>%s</session-id>"
				"<source-host>%s</source-host></netconf-session-start>",
				session->username,
				session->session_id,
				session->hostname) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			va_end(params);
			return (EXIT_FAILURE);
		}

		/* no more parameters - till here we need session parameter */
		va_end(params);

		break;
	case NCNTF_BASE_SESSION_END:
		session = va_arg(params, const struct nc_session*);
		reason = va_arg(params, NC_SESSION_TERM_REASON);

		/* check the result */
		if (session == NULL) {
			ERROR("Invalid \'session\' parameter of %s.", __func__);
			va_end(params);
			return (EXIT_FAILURE);
		}

		aux2 = NULL;
		if (reason == NC_SESSION_TERM_KILLED) {
			aux1 = va_arg(params, char*);
			if (aux1 != NULL) {
				if (asprintf(&aux2, "<killed-by>%s</killed-by>", aux1) == -1) {
					ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
					aux2 = strdup("<killed-by/>");
				}
			}
		}

		/* prepare part of the content for the specific termination reason */
		if (asprintf(&aux1, "<termination-reason>%s</termination-reason>", nc_session_term_string(reason)) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			aux1 = strdup("<termination-reason/>");
		}

		/* compound the event content */
		if (asprintf(&content, "<netconf-session-end><username>%s</username>"
				"<session-id>%s</session-id>"
				"<source-host>%s</source-host>"
				"%s%s</netconf-session-end>",
				session->username,
				session->session_id,
				session->hostname,
				(aux2 == NULL) ? "" : aux2,
				(aux1 == NULL) ? "" : aux1) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			free(aux2);
			free(aux1);
			return (EXIT_FAILURE);
		}
		free(aux2);
		free(aux1);

		break;
	default:
		ERROR("Adding unsupported type of event.");
		return (EXIT_FAILURE);
		break;
	}
	va_end(params);

	/* process the EventTime */
	if (etime == -1) {
		etime = time(NULL);
	}
	if (etime == -1) {
		ERROR("Setting event time failed (%s).", strerror(errno));
		ret = EXIT_FAILURE;
		goto cleanup;
	}
	if ((event_time = nc_time2datetime(etime)) == NULL) {
		ERROR("Internal error when converting time formats (%s:%d).", __FILE__, __LINE__);
		ret = EXIT_FAILURE;
		goto cleanup;
	}
	etime64 = (uint64_t)etime;

	/* get event name string for filter on streams */
	if ((edoc = xmlReadMemory(content, strlen(content), NULL, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING)) == NULL) {
		ERROR("xmlReadMemory failed (%s:%d)", __FILE__, __LINE__);
		ret = EXIT_FAILURE;
		goto cleanup;
	}
	if ((aux1 = (char*)((xmlDocGetRootElement(edoc))->name)) == NULL) {
		ERROR("xmlDocGetRootElement failed (%s:%d)", __FILE__, __LINE__);
		ret = EXIT_FAILURE;
		goto cleanup;
	}
	ename = strdup(aux1);
	xmlFreeDoc(edoc);

	/* complete the event text */
	len = (int32_t) asprintf(&record, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<notification xmlns=\"%s\"><eventTime>%s</eventTime>"
			"%s</notification>",
			NC_NS_NOTIFICATIONS,
			event_time,
			content);
	if (len == -1) {
		ERROR("Creating event record failed.");
		ret = EXIT_FAILURE;
		goto cleanup;
	}
	len++; /* include termination null byte */

	/* write the event into the stream file(s) */
	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);
	for (s = streams; s != NULL; s = s->next) {
		if (s->replay == 0) {
			continue;
		}

		if (ncntf_event_isallowed(s->name, ename) != 0) {
			/* log the event to the stream file */
			if (ncntf_stream_lock(s) == 0) {
				offset = lseek(s->fd_events, 0, SEEK_END);
				while (((r = write(s->fd_events, &len, sizeof(int32_t))) == -1) && (errno == EAGAIN ||errno == EINTR));
				if (r == -1) { goto write_failed; }
				while (((r = write(s->fd_events, &etime64, sizeof(uint64_t))) == -1) && (errno == EAGAIN ||errno == EINTR));
				if (r == -1) { goto write_failed; }
				while (((r = write(s->fd_events, record, len)) == -1) && (errno == EAGAIN ||errno == EINTR));
				if (r == -1) { goto write_failed; }

write_failed:
				if (r == -1) {
					WARN("Writing event into stream file failed (%s).", strerror(errno));
					/* revert changes */
					if (ftruncate(s->fd_events, offset) == -1) {
						ERROR("ftruncate() failed on stream file \'%s\' failed (%s).", s->name, strerror(errno));
					}
					lseek(s->fd_events, offset, SEEK_SET);
				}
				ncntf_stream_unlock(s);
			} else {
				WARN("Unable to write event %s into stream file %s (locking failed).", ename, s->name);
			}
		}
	}
	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);

	/* announce event via DBus */
	DBG_LOCK("dbus_mut");
	pthread_mutex_lock(dbus_mut);
	if (dbus != NULL) {
		for (s = streams; s != NULL; s = s->next) {
			if (ncntf_event_isallowed(s->name, ename) == 0) {
				continue;
			}
			/* create a signal and check for errors */
			if (asprintf(&signal_object, "%s/%s", NC_NTF_DBUS_PATH, s->name) == -1) {
				ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
				WARN("Announcing event via DBus failed due to previous error.");
				goto cleanup_dbus_mut;
				/* event already successfully stored, return SUCCESS */
				ret = EXIT_SUCCESS;
			}
			signal = dbus_message_new_signal(signal_object, NC_NTF_DBUS_INTERFACE, "Event");
			free(signal_object);
			if (signal == NULL) {
				WARN("Announcing event via DBus failed (creating DBus signal failed).");
				goto cleanup_dbus_mut;
				/* event already successfully stored, return SUCCESS */
				ret = EXIT_SUCCESS;
			}
			/* append arguments onto signal */
			dbus_message_iter_init_append(signal, &signal_args);
			if (!dbus_message_iter_append_basic(&signal_args, DBUS_TYPE_UINT64, &etime64)) {
				WARN("Announcing event via DBus failed (attaching event timestamp failed).");
				goto cleanup_dbus_mut;
				/* event already successfully stored, return SUCCESS */
				ret = EXIT_SUCCESS;
			}
			if (!dbus_message_iter_append_basic(&signal_args, DBUS_TYPE_STRING, &record)) {
				WARN("Announcing event via DBus failed (attaching event content failed).");
				goto cleanup_dbus_mut;
				/* event already successfully stored, return SUCCESS */
				ret = EXIT_SUCCESS;
			}

			/* send the message and flush the connection */
			if (!dbus_connection_send(dbus, signal, NULL)) {
				WARN("Announcing event via DBus failed (sending signal failed).");
				goto cleanup_dbus_mut;
				/* event already successfully stored, return SUCCESS */
				ret = EXIT_SUCCESS;
			}
			dbus_connection_flush(dbus);
		}
	}

cleanup_dbus_mut:
	DBG_UNLOCK("dbus_mut");
	pthread_mutex_unlock(dbus_mut);
cleanup:
	/* free DBus signal */
	if (signal != NULL) {
		dbus_message_unref(signal);
	}

	/* final cleanup */
	free(record);
	free(ename);
	free(content);
	free(event_time);

	return (ret);
}

nc_ntf* ncntf_notif_create(time_t event_time, const char* content)
{
	char* notif_data = NULL, *etime = NULL;
	xmlDocPtr notif_doc;
	nc_ntf* retval;

	if ((etime = nc_time2datetime(event_time)) == NULL) {
		ERROR("Converting time to string failed (%s:%d)", __FILE__, __LINE__);
		return (NULL);
	}

	if (asprintf(&notif_data, "<notification>%s</notification>", content) == -1) {
		ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
		free(etime);
		return (NULL);
	}
	notif_doc = xmlReadMemory(notif_data, strlen(notif_data), NULL, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (notif_doc == NULL) {
		ERROR("xmlReadMemory failed (%s:%d)", __FILE__, __LINE__);
		free(notif_data);
		free(etime);
		return (NULL);
	}
	free(notif_data);

	if (xmlNewChild(notif_doc->children, NULL, BAD_CAST "eventTime", BAD_CAST etime) == NULL) {
		ERROR("xmlAddChild failed: %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		xmlFreeDoc(notif_doc);
		free(etime);
		return NULL;
	}
	free(etime);

	retval = malloc(sizeof(nc_rpc));
	if (retval == NULL) {
		ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
		return (NULL);
	}
	retval->doc = notif_doc;
	retval->msgid = NULL;
	retval->error = NULL;
	retval->with_defaults = NCWD_MODE_DISABLED;

	return (retval);
}

void ncntf_notif_free(nc_ntf *ntf)
{
	nc_msg_free((struct nc_msg*) ntf);
}

NCNTF_EVENT ncntf_notif_get_type(nc_ntf* notif)
{
	xmlNodePtr root, node;

	if (notif == NULL || notif->doc == NULL) {
		ERROR("%s: Invalid input parameter.", __func__);
		return (NCNTF_ERROR);
	}

	if ((root = xmlDocGetRootElement (notif->doc)) == NULL) {
		ERROR("%s: Invalid message format, root element is missing.", __func__);
		return (NCNTF_ERROR);
	}

	if (xmlStrcmp(root->name, BAD_CAST "notification") == 0) {
		for (node = root->children; node != NULL; node = node->next) {
			if (node->name == NULL || xmlStrcmp(node->name, BAD_CAST "eventTime") == 0) {
				continue;
			}
			/* use first not eventTime element */
			break;
		}
		if (node == NULL) {
			ERROR("%s: Invalid Notification message - missing event description.", __func__);
			return (NCNTF_ERROR);
		}

		if (xmlStrcmp(node->name, BAD_CAST "replayComplete") == 0) {
			return (NCNTF_REPLAY_COMPLETE);
		} else if (xmlStrcmp(node->name, BAD_CAST "notificationComplete") == 0) {
			return (NCNTF_NTF_COMPLETE);
		} else if (xmlStrcmp(node->name, BAD_CAST "netconf-config-change") == 0) {
			return (NCNTF_BASE_CFG_CHANGE);
		} else if (xmlStrcmp(node->name, BAD_CAST "netconf-capability-change") == 0) {
			return (NCNTF_BASE_CPBLT_CHANGE);
		} else if (xmlStrcmp(node->name, BAD_CAST "netconf-session-start") == 0) {
			return (NCNTF_BASE_SESSION_START);
		} else if (xmlStrcmp(node->name, BAD_CAST "netconf-session-end") == 0) {
			return (NCNTF_BASE_SESSION_END);
		} else if (xmlStrcmp(node->name, BAD_CAST "netconf-configrmed-commit") == 0) {
			return (NCNTF_BASE_CONFIRMED_COMMIT);
		} else {
			return (NCNTF_GENERIC);
		}
	} else {
		ERROR("%s: Invalid Notification message - missing <notification> element.", __func__);
		return (NCNTF_ERROR);
	}
}

char* ncntf_notif_get_content(nc_ntf* notif)
{
	char * retval;
	xmlNodePtr root, node;
	xmlDocPtr aux_doc;
	xmlBufferPtr buffer;

	if (notif == NULL || notif->doc == NULL) {
		ERROR("%s: Invalid input parameter.", __func__);
		return (NULL);
	}

	if ((root = xmlDocGetRootElement (notif->doc)) == NULL) {
		ERROR("%s: Invalid message format, root element is missing.", __func__);
		return (NULL);
	}
	if (xmlStrcmp(root->name, BAD_CAST "notification") != 0) {
		ERROR("%s: Invalid message format, missing notification element.", __func__);
		return (NULL);
	}

	/* by copying node, move all needed namespaces into the content nodes */
	aux_doc = xmlNewDoc(BAD_CAST "1.0");
	xmlDocSetRootElement(aux_doc, xmlNewNode(NULL, BAD_CAST "content"));
	xmlAddChildList(aux_doc->children, xmlDocCopyNodeList(aux_doc, root->children));
	buffer = xmlBufferCreate ();
	for (node = aux_doc->children->children; node != NULL; node = node->next) {
		/* skip invalid nodes */
		if (node->name == NULL || node->ns == NULL || node->ns->href == NULL) {
			continue;
		}

		/* skip eventTime element */
		if (xmlStrcmp(node->name, BAD_CAST "eventTime") == 0 &&
				xmlStrcmp(node->ns->href, BAD_CAST NC_NS_NOTIFICATIONS) == 0) {
			continue;
		}

		/* dump content into the buffer */
		xmlNodeDump(buffer, aux_doc, node, 1, 1);
	}
	retval = strdup((char *)xmlBufferContent (buffer));
	xmlBufferFree (buffer);
	xmlFreeDoc(aux_doc);

	return (retval);
}

xmlNodePtr ncxmlntf_notif_get_content(nc_ntf* notif)
{
	xmlNodePtr root, retval = NULL, aux;

	if (notif == NULL || notif->doc == NULL) {
		ERROR("%s: Invalid input parameter.", __func__);
		return (NULL);
	}

	if ((root = xmlDocGetRootElement (notif->doc)) == NULL) {
		ERROR("%s: Invalid message format, root element is missing.", __func__);
		return (NULL);
	}
	if (xmlStrcmp(root->name, BAD_CAST "notification") != 0) {
		ERROR("%s: Invalid message format, missing notification element.", __func__);
		return (NULL);
	}

	for (aux = root->children; aux != NULL; aux = aux->next) {
		if (aux->type == XML_ELEMENT_NODE) {
			/* skip eventTime element */
			if (xmlStrcmp(aux->name, BAD_CAST "eventTime") == 0 &&
					xmlStrcmp(aux->ns->href, BAD_CAST NC_NS_NOTIFICATIONS) == 0) {
				continue;
			}

			if (retval == NULL) {
				retval = xmlCopyNode(aux, 1);
			} else {
				xmlAddSibling(retval, xmlCopyNode(aux, 1));
			}
		}
	}

	return (retval);
}

time_t ncntf_notif_get_time(nc_ntf* notif)
{
	xmlXPathContextPtr notif_ctxt = NULL;
	xmlXPathObjectPtr result = NULL;
	xmlChar* datetime;
	time_t t = -1;

	if (notif == NULL || notif->doc == NULL) {
		return (-1);
	}

	/* create xpath evaluation context */
	if ((notif_ctxt = xmlXPathNewContext(notif->doc)) == NULL) {
		WARN("%s: Creating XPath context failed.", __func__)
		/* with-defaults cannot be found */
		return (-1);
	}
	if (xmlXPathRegisterNs(notif_ctxt, BAD_CAST "ntf", BAD_CAST NC_NS_NOTIFICATIONS) != 0) {
		xmlXPathFreeContext(notif_ctxt);
		return (-1);
	}

	/* get eventTime value */
	result = xmlXPathEvalExpression(BAD_CAST "/ntf:notification/ntf:eventTime", notif_ctxt);
	if (result != NULL) {
		if (result->nodesetval->nodeNr != 1) {
			t = -1;
		} else {
			t = nc_datetime2time((char*)(datetime = xmlNodeGetContent(result->nodesetval->nodeTab[0])));
			if (datetime != NULL) {
				xmlFree(datetime);
			}
		}
		xmlXPathFreeObject(result);
	}

	xmlXPathFreeContext(notif_ctxt);

	return (t);
}

/**
 * @return 0 on success,\n -1 on general error (invalid rpc),\n -2 on filter error (filter set but it is invalid)
 */
static int ncntf_subscription_get_params(const nc_rpc* subscribe_rpc, char **stream, time_t *start, time_t *stop, struct nc_filter** filter)
{
	xmlXPathContextPtr srpc_ctxt = NULL;
	xmlXPathObjectPtr result = NULL;
	xmlChar* datetime;

	if (subscribe_rpc == NULL || nc_rpc_get_op(subscribe_rpc) != NC_OP_CREATESUBSCRIPTION) {
		return (-1);
	}

	/* create xpath evaluation context */
	if ((srpc_ctxt = xmlXPathNewContext(subscribe_rpc->doc)) == NULL) {
		ERROR("%s: Creating XPath context failed.", __func__);
		return (-1);
	}
	if (xmlXPathRegisterNs(srpc_ctxt, BAD_CAST "ntf", BAD_CAST NC_NS_NOTIFICATIONS) != 0) {
		ERROR("%s: Registering namespace for XPath context failed.", __func__);
		xmlXPathFreeContext(srpc_ctxt);
		return (-1);
	}

	/* get stream name from subscription */
	if (stream != NULL) {
		result = xmlXPathEvalExpression(BAD_CAST "//ntf:create-subscription/ntf:stream", srpc_ctxt);
		if (result == NULL || result->nodesetval == NULL || result->nodesetval->nodeNr != 1) {
			/* use default stream 'netconf' */
			*stream = strdup(NCNTF_STREAM_DEFAULT);
		} else {
			*stream = (char*) (xmlNodeGetContent(result->nodesetval->nodeTab[0]));
		}
		if (result != NULL) {
			xmlXPathFreeObject(result);
		}
	}

	/* get startTime from the subscription */
	if (start != NULL) {
		result = xmlXPathEvalExpression(BAD_CAST "//ntf:create-subscription/ntf:startTime", srpc_ctxt);
		if (result == NULL || result->nodesetval == NULL || result->nodesetval->nodeNr != 1) {
			*start = -1;
		} else {
			*start = nc_datetime2time((char*) (datetime = xmlNodeGetContent(result->nodesetval->nodeTab[0])));
			if (datetime != NULL) {
				xmlFree(datetime);
			}
		}
		if (result != NULL) {
			xmlXPathFreeObject(result);
		}
	}

	/* get stopTime from the subscription */
	if (stop != NULL) {
		result = xmlXPathEvalExpression(BAD_CAST "//ntf:create-subscription/ntf:stopTime", srpc_ctxt);
		if (result == NULL || result->nodesetval == NULL || result->nodesetval->nodeNr != 1) {
			*stop = -1;
		} else {
			*stop = nc_datetime2time((char*) (datetime = xmlNodeGetContent(result->nodesetval->nodeTab[0])));
			if (datetime != NULL) {
				xmlFree(datetime);
			}
		}
		if (result != NULL) {
			xmlXPathFreeObject(result);
		}
	}

	/* get filter from the subscription */
	if (filter != NULL) {
		result = xmlXPathEvalExpression(BAD_CAST "//ntf:create-subscription/ntf:filter", srpc_ctxt);
		if (result == NULL || result->nodesetval == NULL || result->nodesetval->nodeNr != 1) {
			/* do nothing - filter is not specified */
		} else {
			/* filter exist, check its correctness */
			if ((*filter = nc_rpc_get_filter(subscribe_rpc)) == NULL) {
				return (-2);
			}
		}
		if (result != NULL) {
			xmlXPathFreeObject(result);
		}
	}
	xmlXPathFreeContext(srpc_ctxt);
	return (0);
}

nc_reply *ncntf_subscription_check(const nc_rpc* subscribe_rpc)
{
	struct nc_err* e = NULL;
	char *stream = NULL, *auxs = NULL;
	struct nc_filter *filter = NULL;
	time_t start, stop;

	if (subscribe_rpc == NULL || nc_rpc_get_op(subscribe_rpc) != NC_OP_CREATESUBSCRIPTION) {
		return (nc_reply_error(nc_err_new(NC_ERR_INVALID_VALUE)));
	}

	switch(ncntf_subscription_get_params(subscribe_rpc, &stream, &start, &stop, &filter)) {
	case 0:
		/* everything ok */
		break;
	case -1:
		/* rpc is invalid */
		e = nc_err_new(NC_ERR_OP_FAILED);
		goto cleanup;
		break;
	case -2:
		/* filter is invalid */
		e = nc_err_new(NC_ERR_BAD_ELEM);
		nc_err_set(e, NC_ERR_PARAM_TYPE, "protocol");
		nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "filter");
		goto cleanup;
		break;
	default:
		/* unknown error */
		e = nc_err_new(NC_ERR_OP_FAILED);
		goto cleanup;
		break;
	}

	/* check existence of the stream */
	DBG_LOCK("stream_mut");
	pthread_mutex_lock(streams_mut);
	if (ncntf_stream_get(stream) == NULL) {
		DBG_UNLOCK("streams_mut");
		pthread_mutex_unlock(streams_mut);
		e = nc_err_new(NC_ERR_INVALID_VALUE);
		if (asprintf(&auxs, "Requested stream \'%s\' does not exist.", stream) == -1) {
			auxs = strdup("Requested stream does not exist");
		}
		nc_err_set(e, NC_ERR_PARAM_MSG, auxs);
		free(auxs);
		goto cleanup;
	}
	DBG_UNLOCK("streams_mut");
	pthread_mutex_unlock(streams_mut);

	/* check start and stop times */
	if ((stop != -1) && (start == -1)) {
		e = nc_err_new(NC_ERR_MISSING_ELEM);
		nc_err_set(e, NC_ERR_PARAM_TYPE, "protocol");
		nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "startTime");
		goto cleanup;
	}
	if ((stop != -1) && (start != -1) && (start > stop)) {
		e = nc_err_new(NC_ERR_BAD_ELEM);
		nc_err_set(e, NC_ERR_PARAM_TYPE, "protocol");
		nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "stopTime");
		goto cleanup;
	}
	if (start != -1 && start > time(NULL)) {
		e = nc_err_new(NC_ERR_BAD_ELEM);
		nc_err_set(e, NC_ERR_PARAM_TYPE, "protocol");
		nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "startTime");
		goto cleanup;
	}

cleanup:
	/* free unnecessary values */
	nc_filter_free(filter);
	free (stream);

	if (e == NULL) {
		/* all is checked and correct */
		return (nc_reply_ok());
	} else {
		return (nc_reply_error(e));
	}
}

/**
 * @ingroup notifications
 * @brief Start sending notification according to the given
 * \<create-subscription\> NETCONF RPC request. All events from the specified
 * stream are processed and sent to the client until the stop time is reached
 * or until the session is terminated.
 *
 * @param[in] session NETCONF session where the notifications will be sent.
 * @param[in] subscribe_rpc \<create-subscription\> RPC, if any other RPC is
 * given, -1 is returned.
 *
 * @return number of sent notifications (including 0), -1 on error.
 */
long long int ncntf_dispatch_send(struct nc_session* session, const nc_rpc* subscribe_rpc)
{
	long long int count = 0;
	char* stream = NULL, *event = NULL, *time_s = NULL;
	struct nc_filter *filter = NULL;
	time_t start, stop;
	xmlDocPtr event_doc, filter_doc;
	xmlNodePtr event_node, aux_node, nodelist = NULL;
	nc_ntf* ntf;
	nc_reply *reply;

	if (session == NULL ||
			session->status != NC_SESSION_STATUS_WORKING ||
			subscribe_rpc == NULL ||
			nc_rpc_get_op(subscribe_rpc) != NC_OP_CREATESUBSCRIPTION) {
		ERROR("%s: Invalid parameters.", __func__);
		return (-1);
	}

	/* check subscription rpc */
	reply = ncntf_subscription_check(subscribe_rpc);
	if (nc_reply_get_type(reply) != NC_REPLY_OK) {
		ERROR("%s: create-subscription check failed (%s).", __func__, nc_reply_get_errormsg(reply));
		nc_reply_free(reply);
		return (-1);
	}
	nc_reply_free(reply);

	/* get parameters from subscription */
	if (ncntf_subscription_get_params(subscribe_rpc, &stream, &start, &stop, &filter) != 0 ) {
		ERROR("Parsing create-subscription for parameters failed.");
		return (-1);
	}

	/* check if there is another notification subscription */
	DBG_LOCK("mut_session");
	pthread_mutex_lock(&(session->mut_session));
	if (nc_session_notif_allowed(session) == 0) {
		DBG_UNLOCK("mut_session");
		pthread_mutex_unlock(&(session->mut_session));
		WARN("%s: Notification subscription is not allowed on the given session.", __func__);
		nc_filter_free(filter);
		free(stream);
		return (-1);
	}
	/* set flag, notification subscription is now activated */
	session->ntf_active = 1;
	DBG_UNLOCK("mut_session");
	pthread_mutex_unlock(&(session->mut_session));

	/* prepare xml doc for filtering */
	filter_doc = xmlNewDoc(BAD_CAST "1.0");
	filter_doc->encoding = xmlStrdup(BAD_CAST UTF8);

	ncntf_stream_iter_start(stream);
	while(ncntf_config != NULL) {
		if ((event = ncntf_stream_iter_next(stream, start, stop, NULL)) == NULL) {
			if ((stop == -1) || ((stop != -1) && (stop > time(NULL)))) {
				continue;
			} else {
				DBG("stream iter end: stop=%ld, time=%ld", stop, time(NULL));
				break;
			}
		}
		if ((event_doc = xmlReadMemory(event, strlen(event), NULL, NULL, 0)) != NULL) {
			/* apply filter */
			if (filter != NULL) {

				/* filter all content nodes in notification */
				event_node = event_doc->children->children; /* doc -> <notification> -> <something> */
				while (event_node != NULL) {
					/* skip invalid nodes */
					if (event_node->name == NULL || event_node->ns == NULL || event_node->ns->href == NULL) {
						event_node = event_node->next;
						continue;
					}

					/* skip eventTime element */
					if (xmlStrcmp(event_node->name, BAD_CAST "eventTime") == 0 &&
							xmlStrcmp(event_node->ns->href, BAD_CAST NC_NS_NOTIFICATIONS) == 0) {
						event_node = event_node->next;
						continue;
					}

					/* filter the data */
					if (ncxml_filter(event_node, filter, &aux_node) != 0) {
						ERROR("Filter failed.");
						continue;
					}
					if (aux_node != NULL) {
						aux_node->next = nodelist;
						nodelist = aux_node;
					}

					/* detach and free currently filtered node from the original document */
					aux_node = event_node;
					event_node = event_node->next; /* find the next node to filter */
					xmlUnlinkNode(aux_node);
					xmlFreeNode(aux_node);
				}

				if (nodelist != NULL) {
					xmlAddChildList(event_doc->children, aux_node); /* into doc -> <notification> */
					nodelist = NULL;
				} else {
					/* nothing to send */
					xmlFreeDoc(event_doc);
					free(event);
					continue;
				}
			}

			ntf = malloc(sizeof(nc_rpc));
			if (ntf == NULL) {
				ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
				session->ntf_active = 0;
				nc_filter_free(filter);
				free(stream);
				return (-1);
			}
			ntf->doc = event_doc;
			ntf->msgid = NULL;
			ntf->error = NULL;
			ntf->with_defaults = NCWD_MODE_DISABLED;

			nc_session_send_notif(session, ntf);
			ncntf_notif_free(ntf);
		} else {
			WARN("Invalid format of stored event, skipping.");
		}
		free(event);
	}
	xmlFreeDoc(filter_doc);
	ncntf_stream_iter_finnish(stream);

	/* cleanup */
	nc_filter_free(filter);
	free(stream);

	/* send notificationComplete Notification */
	ntf = calloc(1, sizeof(nc_rpc));
	if (ntf == NULL) {
		ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
		session->ntf_active = 0;
		return (-1);
	}
	if (asprintf(&event, "<notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\">"
			"<eventTime>%s</eventTime><notificationComplete/></notification>", time_s = nc_time2datetime(time(NULL))) == -1) {
		ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
		WARN("Sending notificationComplete failed due to previous error.");
		ncntf_notif_free(ntf);
		session->ntf_active = 0;
		return(count);
	}
	free (time_s);
	ntf->doc = xmlReadMemory(event, strlen(event), NULL, NULL, 0);
	ntf->msgid = NULL;
	ntf->error = NULL;
	ntf->with_defaults = NCWD_MODE_DISABLED;
	nc_session_send_notif(session, ntf);
	ncntf_notif_free(ntf);
	free(event);

	session->ntf_active = 0;
	return (count);
}

/**
 * @ingroup notifications
 * @brief Subscribe for receiving notifications from the given session
 * according to parameters in the given subscribtion RPC. Received notifications
 * are processed by the given process_ntf callback function. Functions stops
 * when the final notification <notificationComplete> is received or when the
 * session is terminated.
 *
 * @param[in] session NETCONF session where the notifications will be sent.
 * @param[in] subscribe_rpc \<create-subscription\> RPC, if any other RPC is
 * given, -1 is returned.
 * @param[in] process_ntf Callback function to process content of the
 * notification. If NULL, content of the notification is printed on stdout.
 *
 * @return number of received notifications, -1 on error.
 */
long long int ncntf_dispatch_receive(struct nc_session *session, void (*process_ntf)(time_t eventtime, const char* content))
{
	long long int count = 0;
	nc_ntf* ntf = NULL;
	NC_MSG_TYPE type;
	int eventfd = -1, dispatch = 1;
	time_t event_time;
	char* content;

	if (session == NULL || session->status != NC_SESSION_STATUS_WORKING ) {
		ERROR("%s: Invalid parameters.", __func__);
		return (-1);
	}

	if ((eventfd = nc_session_get_eventfd(session)) == -1) {
		ERROR("Invalid NETCONF session input file descriptor.");
		return (-1);
	}

	if (nc_cpblts_enabled(session, NC_CAP_NOTIFICATION_ID) == 0) {
		ERROR("Given session does not support notifications capability.");
		return (-1);
	}

	DBG_LOCK("mut_session");
	pthread_mutex_lock(&(session->mut_session));
	if (session->ntf_active == 0) {
		session->ntf_active = 1;
	} else {
		DBG_UNLOCK("mut_session");
		pthread_mutex_unlock(&(session->mut_session));
		ERROR("Another ncntf_dispatch_receive() function active on the session.")
		return (-1);
	}
	DBG_UNLOCK("mut_session");
	pthread_mutex_unlock(&(session->mut_session));

	/* check function for notifications processing */
	if (process_ntf == NULL) {
		process_ntf = ncntf_event_stdoutprint;
	}

	/* main loop for receiving notifications */
	while(dispatch && session != NULL && session->status == NC_SESSION_STATUS_WORKING) {
		/* process current notification */
		switch (type = nc_session_recv_notif(session, 0, &ntf)) {
		case NC_MSG_UNKNOWN: /* error */
			dispatch = 0;
			continue; /* end the dispatch loop */
			break;
		case NC_MSG_NOTIFICATION:
			/* check for <notificationComplete> */
			if (ncntf_notif_get_type(ntf) == NCNTF_NTF_COMPLETE) {
				/* end of the Notification stream */
				dispatch = 0;
			}

			/* Parse XML to get parameters for callback function */
			event_time = ncntf_notif_get_time(ntf);
			content = ncntf_notif_get_content(ntf);
			ncntf_notif_free(ntf);
			ntf = NULL;
			if (event_time == -1 || content == NULL) {
				free(content);
				WARN("Invalid notification received. Ignoring.");
				continue; /* go for another notification */
			}
			process_ntf(event_time, content);
			free(content);
			break;
		default:
			/* no notification to read now */
			usleep(100);
			continue;
			break;
		}
	}

	session->ntf_active = 0;
	return (count);
}

