/*
 *  Copyright (C) 2011 - EATON
 *  Copyright (C) 2016 - EATON - IP addressed XML scan
 *  Copyright (C) 2016-2021 - EATON - Various threads-related improvements
 *  Copyright (C) 2020-2024 - Jim Klimov <jimklimov+nut@gmail.com> - support and modernization of codebase
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*! \file scan_xml_http.c
    \brief detect NUT supported XML HTTP devices
    \author Frederic Bohe <fredericbohe@eaton.com>
    \author Michal Vyskocil <MichalVyskocil@eaton.com>
    \author Jim Klimov <EvgenyKlimov@eaton.com>
*/

#include "common.h"
#include "nut-scan.h"
#include "nut_stdint.h"

/* externally visible to nutscan-init */
int nutscan_unload_neon_library(void);

#ifdef WITH_NEON

#ifndef WIN32
# include <sys/types.h>
# include <sys/socket.h>
# include <netdb.h>
# include <arpa/inet.h>
# include <netinet/in.h>
# include <sys/select.h>
# define SOCK_OPT_CAST
#else	/* WIN32 */
# define SOCK_OPT_CAST (char*)
/* Those 2 files for support of getaddrinfo, getnameinfo and freeaddrinfo
   on Windows 2000 and older versions */
# include <ws2tcpip.h>
# include <wspiapi.h>
# if ! HAVE_INET_PTON
#  include "wincompat.h"	/* fallback inet_ntop where needed */
# endif
#endif	/* WIN32 */

#include <string.h>
#include <stdio.h>
#include <ne_xml.h>
#include <ltdl.h>

/* dynamic link library stuff */
static lt_dlhandle dl_handle = NULL;
static const char *dl_error = NULL;
static char *dl_saved_libname = NULL;

static void (*nut_ne_xml_push_handler)(ne_xml_parser *p,
                            ne_xml_startelm_cb *startelm,
                            ne_xml_cdata_cb *cdata,
                            ne_xml_endelm_cb *endelm,
                            void *userdata);
static void (*nut_ne_xml_destroy)(ne_xml_parser *p);
static int (*nut_ne_xml_failed)(ne_xml_parser *p);
static ne_xml_parser * (*nut_ne_xml_create)(void);
static int (*nut_ne_xml_parse)(ne_xml_parser *p, const char *block, size_t len);

/* This variable collects device(s) from a sequential or parallel scan,
 * is returned to caller, and cleared to allow subsequent independent scans */
static nutscan_device_t * dev_ret = NULL;
#ifdef HAVE_PTHREAD
static pthread_mutex_t dev_mutex;
#endif

/* use explicit booleans */
#ifndef FALSE
typedef enum ebool { FALSE = 0, TRUE } bool_t;
#else
typedef int bool_t;
#endif

/* Return 0 on success, -1 on error e.g. "was not loaded";
 * other values may be possible if lt_dlclose() errors set them;
 * visible externally */
int nutscan_unload_library(int *avail, lt_dlhandle *pdl_handle, char **libpath);
int nutscan_unload_neon_library(void)
{
	return nutscan_unload_library(&nutscan_avail_xml_http, &dl_handle, &dl_saved_libname);
}

/* Return 0 on error; visible externally */
int nutscan_load_neon_library(const char *libname_path);
int nutscan_load_neon_library(const char *libname_path)
{
	if (dl_handle != NULL) {
		/* if previous init failed */
		if (dl_handle == (void *)1) {
				return 0;
		}
		/* init has already been done */
		return 1;
	}

	if (libname_path == NULL) {
		upsdebugx(0, "Neon library not found. XML search disabled.");
		return 0;
	}

	if (lt_dlinit() != 0) {
		upsdebugx(0, "%s: Error initializing lt_dlinit", __func__);
		return 0;
	}

	dl_handle = lt_dlopen(libname_path);
	if (!dl_handle) {
		dl_error = lt_dlerror();
		goto err;
	}

	/* Clear any existing error */
	lt_dlerror();

	*(void **) (&nut_ne_xml_push_handler) = lt_dlsym(dl_handle,
						"ne_xml_push_handler");
	if ((dl_error = lt_dlerror()) != NULL) {
		goto err;
	}

	*(void **) (&nut_ne_xml_destroy) = lt_dlsym(dl_handle, "ne_xml_destroy");
	if ((dl_error = lt_dlerror()) != NULL) {
		goto err;
	}

	*(void **) (&nut_ne_xml_create) = lt_dlsym(dl_handle, "ne_xml_create");
	if ((dl_error = lt_dlerror()) != NULL) {
		goto err;
	}

	*(void **) (&nut_ne_xml_parse) = lt_dlsym(dl_handle, "ne_xml_parse");
	if ((dl_error = lt_dlerror()) != NULL) {
		goto err;
	}

	*(void **) (&nut_ne_xml_failed) = lt_dlsym(dl_handle, "ne_xml_failed");
	if ((dl_error = lt_dlerror()) != NULL) {
		goto err;
	}

	if (dl_saved_libname)
		free(dl_saved_libname);
	dl_saved_libname = xstrdup(libname_path);

	return 1;

err:
	upsdebugx(0,
		"Cannot load XML library (%s) : %s. XML search disabled.",
		libname_path, dl_error);
	dl_handle = (void *)1;
	lt_dlexit();
	if (dl_saved_libname) {
		free(dl_saved_libname);
		dl_saved_libname = NULL;
	}
	return 0;
}
/* end of dynamic link library stuff */

/* A start-element callback for element with given namespace/name. */
static int startelm_cb(void *userdata, int parent, const char *nspace, const char *name, const char **atts) {
	nutscan_device_t * dev = (nutscan_device_t *)userdata;
	char buf[SMALLBUF];
	int i = 0;
	int result = -1;
	while (atts[i] != NULL) {
		upsdebugx(5, "startelm_cb() : parent=%d nspace='%s' name='%s' atts[%d]='%s' atts[%d]='%s'",
			parent, nspace, name, i, atts[i], (i + 1), atts[i + 1]);
		/* The Eaton/MGE ePDUs almost exclusively support only XMLv4 protocol
		 * (only the very first generation of G2/G3 NMCs supported an older
		 * protocol, but all should have been FW upgraded by now), which NUT
		 * drivers don't yet support. To avoid failing drivers later, the
		 * nut-scanner should not suggest netxml-ups configuration for ePDUs
		 * at this time. */
		if (strcmp(atts[i], "class") == 0 && strcmp(atts[i + 1], "DEV.PDU") == 0) {
			upsdebugx(3, "startelm_cb() : XML v4 protocol is not supported by current NUT drivers, skipping device!");
			/* netxml-ups currently only supports XML version 3 (for UPS),
			 * and not version 4 (for UPS and PDU)! */
			return -1;
		}
		if (strcmp(atts[i], "type") == 0) {
			snprintf(buf, sizeof(buf), "%s", atts[i + 1]);
			nutscan_add_option_to_device(dev, "desc", buf);
			result = 0;
		}
		i = i + 2;
	}
	return result;
}

/* Performs a (parallel-able) NetXML protocol scan of one remote host:port.
 * Returns NULL, updates global dev_ret when a scan is successful.
 * FREES the caller's copy of "arg" and "hostname" in it, if applicable.
 */
static void * nutscan_scan_xml_http_thready(void * arg)
{
	nutscan_xml_t * sec = (nutscan_xml_t *)arg;
	char *scanMsg = "<SCAN_REQUEST/>";
	/* Note: at this time the HTTP/XML scan is
	 * in fact not implemented - just the UDP part */
/*	uint16_t port_http = 80; */
	uint16_t port_udp = 4679;
	/* A NULL "ip" causes a broadcast scan; otherwise
	 * the single ip address is queried directly */
	char *ip = NULL;
	useconds_t usec_timeout = 0;
	int peerSocket = -1;
	int sockopt_on = 1;
	struct sockaddr_in sockAddress_udp;
	socklen_t sockAddressLength = sizeof(sockAddress_udp);
	fd_set fds;
	struct timeval timeout;
	int ret;
	char buf[SMALLBUF + 8];
	char string[SMALLBUF];
	ssize_t recv_size;
	int i;
	nutscan_device_t * nut_dev = NULL;

	memset(&sockAddress_udp, 0, sizeof(sockAddress_udp));

	if (sec != NULL) {
/*		if (sec->port_http > 0 && sec->port_http <= 65534)
 *			port_http = sec->port_http; */
		if (sec->port_udp > 0 && sec->port_udp <= 65534)
			port_udp = sec->port_udp;
		if (sec->usec_timeout > 0)
			usec_timeout = sec->usec_timeout;
		ip = sec->peername; /* NULL or not... */
	}

	if (usec_timeout <= 0)
		usec_timeout = 5000000; /* Driver default : 5sec */

	if (!nutscan_avail_xml_http) {
		goto end_free;
	}

	if ((peerSocket = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		upsdebugx(0, "%s: Error creating socket for %s",
			__func__, ip ? ip : "broadcast");
		goto end_free;
	}

	/* FIXME : Per http://stackoverflow.com/questions/683624/udp-broadcast-on-all-interfaces
	 * A single sendto() generates a single packet,
	 * so one must iterate all known interfaces... */
#define MAX_RETRIES 3
	for (i = 0; i != MAX_RETRIES ; i++) {
		/* Initialize socket */
		sockAddress_udp.sin_family = AF_INET;
		if (ip == NULL) {
			upsdebugx(2,
				"%s: scanning connected network segment(s) "
				"with a broadcast, attempt %d of %d with a timeout of %" PRIuMAX " usec",
				__func__, (i + 1), MAX_RETRIES, (uintmax_t)usec_timeout);
			sockAddress_udp.sin_addr.s_addr = INADDR_BROADCAST;
			setsockopt(peerSocket, SOL_SOCKET, SO_BROADCAST,
				SOCK_OPT_CAST &sockopt_on,
				sizeof(sockopt_on));
		} else {
			upsdebugx(2,
				"%s: scanning IP '%s' with a unicast, "
				"attempt %d of %d with a timeout of %" PRIuMAX " usec",
				__func__, ip, (i + 1), MAX_RETRIES, (uintmax_t)usec_timeout);
			inet_pton(AF_INET, ip, &(sockAddress_udp.sin_addr));
		}
		sockAddress_udp.sin_port = htons(port_udp);

		/* Send scan request */
		if (sendto(peerSocket, scanMsg, strlen(scanMsg), 0,
			(struct sockaddr *)&sockAddress_udp,
			sockAddressLength) <= 0
		) {
			upsdebugx(0, "%s: "
				"Error sending Eaton <SCAN_REQUEST/> to %s, #%d/%d",
				__func__,
				(ip ? ip : "<broadcast>"), (i + 1), MAX_RETRIES);
			usleep(usec_timeout);
			continue;
		}
		else
		{
			int retNum = 0;
			FD_ZERO(&fds);
			FD_SET(peerSocket, &fds);

			timeout.tv_sec = usec_timeout / 1000000;
			timeout.tv_usec = usec_timeout % 1000000;

			upsdebugx(5, "%s: sent request to %s, "
				"loop #%d/%d, waiting for responses",
				__func__, (ip ? ip : "<broadcast>"), (i + 1), MAX_RETRIES);
			while ((ret = select(peerSocket + 1, &fds, NULL, NULL,
						&timeout))
			) {
				ne_xml_parser	*parser;
				int	parserFailed;

				retNum ++;
				upsdebugx(5, "%s: request to %s, "
					"loop #%d/%d, response #%d",
					__func__, (ip ? ip : "<broadcast>"), (i + 1), MAX_RETRIES, retNum);

				timeout.tv_sec = usec_timeout / 1000000;
				timeout.tv_usec = usec_timeout % 1000000;

				if (ret == -1) {
					upsdebug_with_errno(0,
						"%s: Error waiting on socket",
						__func__);
					break;
				}

				sockAddressLength = sizeof(struct sockaddr_in);
				recv_size = recvfrom(peerSocket, buf,
					sizeof(buf), 0,
					(struct sockaddr *)&sockAddress_udp,
					&sockAddressLength);

				if (recv_size < 0) {
					upsdebug_with_errno(0, "%s: "
						"Error reading socket: #%d/%d",
						__func__, (i + 1), MAX_RETRIES);
					usleep(usec_timeout);
					continue;
				}

				if (getnameinfo(
					(struct sockaddr *)&sockAddress_udp,
					sizeof(struct sockaddr_in), string,
					sizeof(string), NULL, 0,
					NI_NUMERICHOST) != 0
				) {
					upsdebug_with_errno(0, "%s: "
						"Error converting IP address", __func__);
					usleep(usec_timeout);
					continue;
				}

				nut_dev = nutscan_new_device();
				if (nut_dev == NULL) {
					upsdebugx(0, "%s: Memory allocation error", __func__);
					goto end_abort;
				}

#ifdef HAVE_PTHREAD
				pthread_mutex_lock(&dev_mutex);
#endif
				upsdebugx(5,
					"%s: Some host at IP %s replied to NetXML UDP request on port %d, "
					"inspecting the response...",
					__func__, string, port_udp);
				nut_dev->type = TYPE_XML;
				/* Try to read device type */
				parser = (*nut_ne_xml_create)();
				(*nut_ne_xml_push_handler)(parser, startelm_cb,
							NULL, NULL, nut_dev);
				/* recv_size is a ssize_t, so in range of size_t */
				(*nut_ne_xml_parse)(parser, buf, (size_t)recv_size);
				parserFailed = (*nut_ne_xml_failed)(parser); /* 0 = ok, nonzero = fail */
				(*nut_ne_xml_destroy)(parser);

				if (parserFailed == 0) {
					nut_dev->driver = strdup("netxml-ups");
					sprintf(buf, "http://%s", string);
					/* FIXME: Should the IPv6 address here be bracketed?
					 *  Does our driver support the notation? */
					nut_dev->port = strdup(buf);
					upsdebugx(3,
						"%s: Adding configuration for driver='%s' port='%s'",
						__func__, nut_dev->driver, nut_dev->port);
					dev_ret = nutscan_add_device_to_device(
						dev_ret, nut_dev);
#ifdef HAVE_PTHREAD
					pthread_mutex_unlock(&dev_mutex);
#endif
				}
				else
				{
					upsdebugx(0, "WARNING: %s: "
						"Device at IP %s replied with NetXML but was not deemed compatible "
						"with 'netxml-ups' driver (unsupported protocol version, etc.)",
						__func__, string);
					nutscan_free_device(nut_dev);
					nut_dev = NULL;
#ifdef HAVE_PTHREAD
					pthread_mutex_unlock(&dev_mutex);
#endif
					if (ip == NULL) {
						/* skip this device; note that for a
						 * broadcast scan there may be more
						 * in the loop's queue */
						continue;
					}
				}

				if (ip != NULL) {
					upsdebugx(2,
						"%s: we collected one reply "
						"to unicast for %s (repsponse from %s), done",
						__func__, ip, string);
					goto end;
				}
			} /* while select() responses */
			if (ip == NULL && dev_ret != NULL) {
				upsdebugx(2,
					"%s: we collected one round of replies "
					"to broadcast with no errors, done", __func__);
				goto end;
			}
		}
	}
	upsdebugx(2,
		"%s: no replies collected for %s, done",
		__func__, (ip ? ip : "<broadcast>"));
	goto end;

end_abort:
	upsdebugx(1,
		"%s: Had to abort scan for %s, see fatal details above",
		__func__, (ip ? ip : "<broadcast>"));

end:
	/* Broadcast is also a socket! */
	if (peerSocket != -1)
		close(peerSocket);

end_free:
	/* free resources which come from the caller
	 * (in parallel runs, nobody else can reap them)
	 */
	if (ip != NULL)
		free(ip);
	if (sec != NULL)
		free(sec);

	return NULL;
}

nutscan_device_t * nutscan_scan_xml_http_range(const char * start_ip, const char * end_ip, useconds_t usec_timeout, nutscan_xml_t * sec)
{
	nutscan_device_t	*ndret;

	/* Are we scanning locally, or through the network? */
	if (start_ip || end_ip) {
		nutscan_ip_range_list_t irl;

		nutscan_init_ip_ranges(&irl);
		nutscan_add_ip_range(&irl, (char *)start_ip, (char *)end_ip);

		ndret = nutscan_scan_ip_range_xml_http(&irl, usec_timeout, sec);

		/* Avoid nuking caller's strings here */
		irl.ip_ranges->start_ip = NULL;
		irl.ip_ranges->end_ip = NULL;
		nutscan_free_ip_ranges(&irl);
	} else {
		/* Probe local device */
		ndret = nutscan_scan_ip_range_xml_http(NULL, usec_timeout, sec);
	}

	return ndret;
}

nutscan_device_t * nutscan_scan_ip_range_xml_http(nutscan_ip_range_list_t * irl, useconds_t usec_timeout, nutscan_xml_t * sec)
{
	bool_t pass = TRUE; /* Track that we may spawn a scanning thread */
	nutscan_device_t * result = NULL;
	nutscan_xml_t * tmp_sec = NULL;

	if (!nutscan_avail_xml_http) {
		return NULL;
	}

	/* We assume the list is maintained by our methods, so should not have
	 * null addresses. But just in case - check for it a little tiny once.
	 */
	if (irl == NULL || irl->ip_ranges == NULL
	 || irl->ip_ranges->start_ip == NULL || irl->ip_ranges->end_ip == NULL
	) {
		upsdebugx(1, "%s: Scanning XML/HTTP bus using broadcast.", __func__);
		/* Fall through to after the if/else clause */
	} else {
		/* Iterate the one or a range of IPs to scan */
		nutscan_ip_range_list_iter_t ip;
		char * ip_str = NULL;

#ifdef HAVE_PTHREAD
# if (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
		sem_t * semaphore = nutscan_semaphore();
#  if (defined HAVE_SEMAPHORE_UNNAMED)
		sem_t   semaphore_scantype_inst;
		sem_t * semaphore_scantype = &semaphore_scantype_inst;
#  elif (defined HAVE_SEMAPHORE_NAMED)
		sem_t * semaphore_scantype = NULL;
#  endif
# endif /* HAVE_SEMAPHORE_UNNAMED || HAVE_SEMAPHORE_NAMED */
		pthread_t thread;
		nutscan_thread_t * thread_array = NULL;
		size_t thread_count = 0, i;
# if (defined HAVE_PTHREAD_TRYJOIN) || (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
		size_t  max_threads_scantype = max_threads_netxml;
# endif
#endif	/* HAVE_PTHREAD */

		if (irl->ip_ranges_count == 1
		&& (irl->ip_ranges->start_ip == irl->ip_ranges->end_ip
		    || !strcmp(irl->ip_ranges->start_ip, irl->ip_ranges->end_ip)
		)) {
			upsdebugx(1, "%s: Scanning XML/HTTP bus for single IP address: %s",
				__func__, irl->ip_ranges->start_ip);
		} else {
			upsdebugx(1, "%s: Scanning XML/HTTP bus for IP address range(s): %s",
				__func__, nutscan_stringify_ip_ranges(irl));
		}

#ifdef HAVE_PTHREAD
		pthread_mutex_init(&dev_mutex, NULL);

# if (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
		if (max_threads_scantype > 0) {
#ifdef HAVE_PRAGMAS_FOR_GCC_DIAGNOSTIC_IGNORED_UNREACHABLE_CODE
#pragma GCC diagnostic push
#endif
#ifdef HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_UNREACHABLE_CODE
#pragma GCC diagnostic ignored "-Wunreachable-code"
#endif
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunreachable-code"
#endif
			/* Different platforms, different sizes, none fits all... */
			if (SIZE_MAX > UINT_MAX && max_threads_scantype > UINT_MAX) {
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#ifdef HAVE_PRAGMAS_FOR_GCC_DIAGNOSTIC_IGNORED_UNREACHABLE_CODE
#pragma GCC diagnostic pop
#endif
				upsdebugx(1,
					"WARNING: %s: Limiting max_threads_scantype to range acceptable for " REPORT_SEM_INIT_METHOD "()",
					__func__);
				max_threads_scantype = UINT_MAX - 1;
			}

			upsdebugx(4, "%s: " REPORT_SEM_INIT_METHOD "() for %" PRIuSIZE " threads", __func__, max_threads_scantype);
#  if (defined HAVE_SEMAPHORE_UNNAMED)
			if (sem_init(semaphore_scantype, 0, (unsigned int)max_threads_scantype)) {
				upsdebug_with_errno(4, "%s: " REPORT_SEM_INIT_METHOD "() failed", __func__);
				max_threads_scantype = 0;
			}
#  elif (defined HAVE_SEMAPHORE_NAMED)
			if (SEM_FAILED == (semaphore_scantype = sem_open(SEMNAME_NETXML, O_CREAT, 0644, (unsigned int)max_threads_scantype))) {
				upsdebug_with_errno(4, "%s: " REPORT_SEM_INIT_METHOD "() failed", __func__);
				semaphore_scantype = NULL;
				max_threads_scantype = 0;
			}
#  endif
		}
# endif /* HAVE_SEMAPHORE_UNNAMED || HAVE_SEMAPHORE_NAMED */

#endif /* HAVE_PTHREAD */

		ip_str = nutscan_ip_ranges_iter_init(&ip, irl);

		while (ip_str != NULL) {
#ifdef HAVE_PTHREAD
			/* NOTE: With many enough targets to scan, this can crash
			 * by spawning too many children; add a limit and loop to
			 * "reap" some already done with their work. And probably
			 * account them in thread_array[] as something to not wait
			 * for below in pthread_join()...
			 */

# if (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
			/* Just wait for someone to free a semaphored slot,
			 * if none are available, and then/otherwise grab one
			 */
			if (thread_array == NULL) {
				/* Starting point, or after a wait to complete
				 * all earlier runners */
				if (max_threads_scantype > 0)
					sem_wait(semaphore_scantype);
				sem_wait(semaphore);
				pass = TRUE;
			} else {
				/* If successful (the lock was acquired),
				 * sem_wait() and sem_trywait() will return 0.
				 * Otherwise, -1 is returned and errno is set,
				 * and the state of the semaphore is unchanged.
				 */
				int	stwST = sem_trywait(semaphore_scantype);
				int	stwS  = sem_trywait(semaphore);
				pass = ((max_threads_scantype == 0 || stwST == 0) && stwS == 0);
				upsdebugx(4, "%s: max_threads_scantype=%" PRIuSIZE
					" curr_threads=%" PRIuSIZE
					" thread_count=%" PRIuSIZE
					" stwST=%d stwS=%d pass=%u",
					__func__, max_threads_scantype,
					curr_threads, thread_count,
					stwST, stwS, pass
				);
			}
# else
#  ifdef HAVE_PTHREAD_TRYJOIN
			/* A somewhat naive and brute-force solution for
			 * systems without a semaphore.h. This may suffer
			 * some off-by-one errors, using a few more threads
			 * than intended (if we race a bit at the wrong time,
			 * probably up to one per enabled scanner routine).
			 */

			/* TOTHINK: Should there be a threadcount_mutex when
			 * we just read the value in if() and while() below?
			 * At worst we would overflow the limit a bit due to
			 * other protocol scanners...
			 */
			if (curr_threads >= max_threads
			|| (curr_threads >= max_threads_scantype && max_threads_scantype > 0)
			) {
				upsdebugx(2, "%s: already running %" PRIuSIZE " scanning threads "
					"(launched overall: %" PRIuSIZE "), "
					"waiting until some would finish",
					__func__, curr_threads, thread_count);

				while (curr_threads >= max_threads
				   || (curr_threads >= max_threads_scantype && max_threads_scantype > 0)
				) {
					for (i = 0; i < thread_count ; i++) {
						int ret;

						if (!thread_array[i].active) continue;

						pthread_mutex_lock(&threadcount_mutex);
						upsdebugx(3, "%s: Trying to join thread #%" PRIuSIZE "...", __func__, i);
						ret = pthread_tryjoin_np(thread_array[i].thread, NULL);
						switch (ret) {
							case ESRCH:     /* No thread with the ID thread could be found - already "joined"? */
								upsdebugx(5, "%s: Was thread #%" PRIuSIZE " joined earlier?", __func__, i);
								break;
							case 0:         /* thread exited */
								if (curr_threads > 0) {
									curr_threads --;
									upsdebugx(4, "%s: Joined a finished thread #%" PRIuSIZE, __func__, i);
								} else {
									/* threadcount_mutex fault? */
									upsdebugx(0, "WARNING: %s: Accounting of thread count "
										"says we are already at 0", __func__);
								}
								thread_array[i].active = FALSE;
								break;
							case EBUSY:     /* actively running */
								upsdebugx(6, "%s: thread #%" PRIuSIZE " still busy (%i)",
									__func__, i, ret);
								break;
							case EDEADLK:   /* Errors with thread interactions... bail out? */
							case EINVAL:    /* Errors with thread interactions... bail out? */
							default:        /* new pthreads abilities? */
								upsdebugx(5, "%s: thread #%" PRIuSIZE " reported code %i",
									__func__, i, ret);
								break;
						}
						pthread_mutex_unlock(&threadcount_mutex);
					}

					if (curr_threads >= max_threads
					|| (curr_threads >= max_threads_scantype && max_threads_scantype > 0)
					) {
							usleep (10000); /* microSec's, so 0.01s here */
					}
				}
				upsdebugx(2, "%s: proceeding with scan", __func__);
			}

			/* NOTE: No change to default "pass" in this ifdef:
			 * if we got to this line, we have a slot to use */
#  endif /* HAVE_PTHREAD_TRYJOIN */
# endif  /* HAVE_SEMAPHORE_UNNAMED || HAVE_SEMAPHORE_NAMED */
#endif   /* HAVE_PTHREAD */

			if (pass) {
				tmp_sec = malloc(sizeof(nutscan_xml_t));
				if (tmp_sec == NULL) {
					upsdebugx(0, "%s: Memory allocation error", __func__);
					break;
				}

				memcpy(tmp_sec, sec, sizeof(nutscan_xml_t));
				tmp_sec->peername = ip_str;
				if (tmp_sec->usec_timeout <= 0) {
					tmp_sec->usec_timeout = usec_timeout;
				}

#ifdef HAVE_PTHREAD
				if (pthread_create(&thread, NULL, nutscan_scan_xml_http_thready, (void*)tmp_sec) == 0) {
					nutscan_thread_t	*new_thread_array;
# ifdef HAVE_PTHREAD_TRYJOIN
					pthread_mutex_lock(&threadcount_mutex);
					curr_threads++;
# endif /* HAVE_PTHREAD_TRYJOIN */

					thread_count++;
					new_thread_array = realloc(thread_array,
						thread_count * sizeof(nutscan_thread_t));
					if (new_thread_array == NULL) {
						upsdebugx(1, "%s: Failed to realloc thread array", __func__);
						break;
					}
					else {
						thread_array = new_thread_array;
					}
					thread_array[thread_count - 1].thread = thread;
					thread_array[thread_count - 1].active = TRUE;

# ifdef HAVE_PTHREAD_TRYJOIN
					pthread_mutex_unlock(&threadcount_mutex);
# endif /* HAVE_PTHREAD_TRYJOIN */
				}
#else	/* if not HAVE_PTHREAD */
				nutscan_scan_xml_http_thready(tmp_sec);
#endif	/* if HAVE_PTHREAD */

				/* Prepare the next iteration; note that
				 * nutscan_scan_xml_http_thready()
				 * takes care of freeing "tmp_sec" and its
				 * reference (NOT strdup!) to "ip_str" as
				 * peername.
				 */
				ip_str = nutscan_ip_ranges_iter_inc(&ip);
			} else { /* if not pass -- all slots busy */
#ifdef HAVE_PTHREAD
# if (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
				/* Wait for all current scans to complete */
				if (thread_array != NULL) {
					upsdebugx (2, "%s: Running too many scanning threads (%"
						PRIuSIZE "), "
						"waiting until older ones would finish",
						__func__, thread_count);
					for (i = 0; i < thread_count ; i++) {
						int ret;
						if (!thread_array[i].active) {
							/* Probably should not get here,
							 * but handle it just in case */
							upsdebugx(0, "WARNING: %s: Midway clean-up: did not expect thread %" PRIuSIZE " to be not active",
								__func__, i);
							sem_post(semaphore);
							if (max_threads_scantype > 0)
								sem_post(semaphore_scantype);
							continue;
						}
						thread_array[i].active = FALSE;
						ret = pthread_join(thread_array[i].thread, NULL);
						if (ret != 0) {
							upsdebugx(0, "WARNING: %s: Midway clean-up: pthread_join() returned code %i",
								__func__, ret);
						}
						sem_post(semaphore);
						if (max_threads_scantype > 0)
							sem_post(semaphore_scantype);
					}
					thread_count = 0;
					free(thread_array);
					thread_array = NULL;
				}
# else
#  ifdef HAVE_PTHREAD_TRYJOIN
				/* TODO: Move the wait-loop for TRYJOIN here? */
#  endif /* HAVE_PTHREAD_TRYJOIN */
# endif  /* HAVE_SEMAPHORE_UNNAMED || HAVE_SEMAPHORE_NAMED */
#endif   /* HAVE_PTHREAD */
			} /* if: could we "pass" or not? */
		} /* while */

#ifdef HAVE_PTHREAD
		if (thread_array != NULL) {
			upsdebugx(2, "%s: all planned scans launched, waiting for threads to complete", __func__);
			for (i = 0; i < thread_count; i++) {
				int ret;

				if (!thread_array[i].active) continue;

				ret = pthread_join(thread_array[i].thread, NULL);
				if (ret != 0) {
					upsdebugx(0, "WARNING: %s: Clean-up: pthread_join() returned code %i",
						__func__, ret);
				}
				thread_array[i].active = FALSE;
# if (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
				sem_post(semaphore);
				if (max_threads_scantype > 0)
					sem_post(semaphore_scantype);
# else
#  ifdef HAVE_PTHREAD_TRYJOIN
				pthread_mutex_lock(&threadcount_mutex);
				if (curr_threads > 0) {
					curr_threads --;
					upsdebugx(5, "%s: Clean-up: Joined a finished thread #%" PRIuSIZE,
						__func__, i);
				} else {
					upsdebugx(0, "WARNING: %s: Clean-up: Accounting of thread count "
						"says we are already at 0", __func__);
				}
				pthread_mutex_unlock(&threadcount_mutex);
#  endif /* HAVE_PTHREAD_TRYJOIN */
# endif /* HAVE_SEMAPHORE_UNNAMED || HAVE_SEMAPHORE_NAMED */
			}
			free(thread_array);
			upsdebugx(2, "%s: all threads freed", __func__);
		}
		pthread_mutex_destroy(&dev_mutex);

# if (defined HAVE_SEMAPHORE_UNNAMED) || (defined HAVE_SEMAPHORE_NAMED)
		if (max_threads_scantype > 0) {
#  if (defined HAVE_SEMAPHORE_UNNAMED)
			sem_destroy(semaphore_scantype);
#  elif (defined HAVE_SEMAPHORE_NAMED)
			if (semaphore_scantype) {
				sem_unlink(SEMNAME_NETXML);
				sem_close(semaphore_scantype);
				semaphore_scantype = NULL;
			}
#  endif
		}
# endif /* HAVE_SEMAPHORE_UNNAMED || HAVE_SEMAPHORE_NAMED */
#endif /* HAVE_PTHREAD */

		result = nutscan_rewind_device(dev_ret);
		dev_ret = NULL;
		return result;
	}	/* end of: scan range of 1+ IP address(es), maybe in parallel */

	/* both start_ip == end_ip == NULL, scan broadcast */
	tmp_sec = malloc(sizeof(nutscan_xml_t));
	if (tmp_sec == NULL) {
		upsdebugx(0, "%s: Memory allocation error", __func__);
		return NULL;
	}

	memcpy(tmp_sec, sec, sizeof(nutscan_xml_t));
	if (irl == NULL || irl->ip_ranges == NULL || irl->ip_ranges->start_ip == NULL) {
		tmp_sec->peername = NULL;
	} else {
		/* Legacy code path for single-IP scan; should not get here */
		tmp_sec->peername = strdup(irl->ip_ranges->start_ip);
	}

	if (tmp_sec->usec_timeout <= 0) {
		tmp_sec->usec_timeout = usec_timeout;
	}

	/* Note: the thready method releases the resources */
	nutscan_scan_xml_http_thready(tmp_sec);
	result = nutscan_rewind_device(dev_ret);
	dev_ret = NULL;
	return result;
}

#else /* not WITH_NEON */

/* stub function */
nutscan_device_t * nutscan_scan_xml_http_range(const char * start_ip, const char * end_ip, useconds_t usec_timeout, nutscan_xml_t * sec)
{
	NUT_UNUSED_VARIABLE(start_ip);
	NUT_UNUSED_VARIABLE(end_ip);
	NUT_UNUSED_VARIABLE(usec_timeout);
	NUT_UNUSED_VARIABLE(sec);

	return NULL;
}

/* stub function */
nutscan_device_t * nutscan_scan_ip_range_xml_http(nutscan_ip_range_list_t * irl, useconds_t usec_timeout, nutscan_xml_t * sec)
{
	NUT_UNUSED_VARIABLE(irl);
	NUT_UNUSED_VARIABLE(usec_timeout);
	NUT_UNUSED_VARIABLE(sec);

	return NULL;
}

int nutscan_unload_neon_library(void)
{
	return 0;
}
#endif /* WITH_NEON */
