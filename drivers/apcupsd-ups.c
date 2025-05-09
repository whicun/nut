/* apcupsd-ups.c - client for apcupsd

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

#include "config.h"

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#else	/* WIN32 */
#include "wincompat.h"
#endif	/* WIN32 */

#ifdef HAVE_POLL_H
# include <poll.h> /* nfds_t */
#else
typedef unsigned long int nfds_t;

# ifndef POLLRDNORM
#  define POLLRDNORM	0x0100
# endif
# ifndef POLLRDBAND
#  define POLLRDBAND	0x0200
# endif
# ifndef POLLIN
#  define POLLIN	(POLLRDNORM | POLLRDBAND)
# endif
# if ! HAVE_STRUCT_POLLFD
typedef struct pollfd {
  SOCKET fd;
  short  events;
  short  revents;
} pollfd_t;
#  define HAVE_STRUCT_POLLFD 1
# endif
#endif	/* !HAVE_POLL_H */

#include "main.h"
#include "apcupsd-ups.h"
#include "attribute.h"
#include "nut_stdint.h"

#define DRIVER_NAME	"apcupsd network client UPS driver"
#define DRIVER_VERSION	"0.73"

#define POLL_INTERVAL_MIN 10

/* driver description structure */
upsdrv_info_t upsdrv_info = {
	DRIVER_NAME,
	DRIVER_VERSION,
	"Andreas Steinmetz <ast@domdv.de>",
	DRV_STABLE,
	{ NULL }
};

static uint16_t port=3551;
static struct sockaddr_in host;

static void process(char *item,char *data)
{
	int i;
	char *p1;
	char *p2;

	for(i=0;nut_data[i].info_type;i++)if(!(nut_data[i].apcupsd_item))
		dstate_setinfo(nut_data[i].info_type,"%s",
			nut_data[i].default_value);
	else if(!strcmp(nut_data[i].apcupsd_item,item))
			switch(nut_data[i].drv_flags&~DU_FLAG_INIT)
	{
	case DU_FLAG_STATUS:
		status_init();
		if(!strcmp(data,"COMMLOST")||!strcmp(data,"NETWORK ERROR")||
		   !strcmp(data,"ERROR"))status_set("OFF");
		else if(!strcmp(data,"SELFTEST"))status_set("OB");
		else for(;(data=strtok(data," "));data=NULL)
		{
			if(!strcmp(data,"CAL"))status_set("CAL");
			else if(!strcmp(data,"TRIM"))status_set("TRIM");
			else if(!strcmp(data,"BOOST"))status_set("BOOST");
			else if(!strcmp(data,"ONLINE"))status_set("OL");
			else if(!strcmp(data,"ONBATT"))status_set("OB");
			else if(!strcmp(data,"OVERLOAD"))status_set("OVER");
			else if(!strcmp(data,"SHUTTING DOWN")||
				!strcmp(data,"LOWBATT"))status_set("LB");
			else if(!strcmp(data,"REPLACEBATT"))status_set("RB");
			else if(!strcmp(data,"NOBATT"))status_set("BYPASS");
		}
		status_commit();
		break;

	case DU_FLAG_DATE:
		if((p1=strchr(data,' ')))
		{
			*p1=0;
			dstate_setinfo(nut_data[i].info_type,"%s",data);
			*p1=' ';
		}
		else dstate_setinfo(nut_data[i].info_type,"%s",data);
		break;

	case DU_FLAG_TIME:
		if((p1=strchr(data,' ')))
		{
			*p1=0;
			if((p2=strchr(p1+1,' ')))
			{
				*p2=0;
				dstate_setinfo(nut_data[i].info_type,"%s",p1+1);
				*p2=' ';
			}
			else dstate_setinfo(nut_data[i].info_type,"%s",p1+1);
			*p1=' ';
		}
		break;

	case DU_FLAG_FW1:
		if((p1=strchr(data,'/')))
		{
			for(;p1!=data;p1--)if(p1[-1]!=' ')break;
			if(*p1==' ')
			{
				*p1=0;
				dstate_setinfo(nut_data[i].info_type,"%s",data);
				*p1=' ';
			}
			else dstate_setinfo(nut_data[i].info_type,"%s",data);
		}
		else dstate_setinfo(nut_data[i].info_type,"%s",data);
		break;

	case DU_FLAG_FW2:
		if((p1=strchr(data,'/')))
		{
			for(;*p1;p1++)if(p1[1]!=' ')break;
			if(*p1&&p1[1])dstate_setinfo(nut_data[i].info_type,"%s",
				p1+1);
		}
		break;

	default:if(nut_data[i].info_flags&ST_FLAG_STRING)
		{
			if((int)strlen(data)>(int)nut_data[i].info_len)
				data[(int)nut_data[i].info_len]=0;
			dstate_setinfo(nut_data[i].info_type,"%s",data);
		}
		else
		{
			/* default_value acts as a format string in this case */
			dstate_setinfo_dynamic(nut_data[i].info_type,
				nut_data[i].default_value,
				"%f", atof(data)*nut_data[i].info_len);
		}
		break;
	}
}

static int getdata(void)
{
	ssize_t x;
	uint16_t n;
	char *item;
	char *data;
	struct pollfd p;
	char bfr[1024];
	st_tree_timespec_t start;
	int ret = -1;
#ifndef WIN32
	int fd_flags;
#else	/* WIN32 */
	/* Note: while the code below uses "pollfd" for simplicity as it is
	 * available in mingw headers (although poll() method usually is not),
	 * WIN32 builds use WaitForMultipleObjects(); see also similar code
	 * in upsd.c for networking.
	 */
	HANDLE event = NULL;
#endif	/* WIN32 */

	state_get_timestamp((st_tree_timespec_t *)&start);

	if (INVALID_FD_SOCK( (p.fd = socket(AF_INET, SOCK_STREAM, 0)) ))
	{
		upsdebugx(1,"socket error");
		/* return -1; */
		ret = -1;
		goto getdata_return;
	}

	if(connect(p.fd,(struct sockaddr *)&host,sizeof(host)))
	{
		upsdebugx(1,"can't connect to apcupsd");
		/* close(p.fd);
		return -1; */
		ret = -1;
		goto getdata_return;
	}

#ifndef WIN32
	/* WSAEventSelect automatically sets the socket to nonblocking mode */
	fd_flags = fcntl(p.fd, F_GETFL);
	if (fd_flags == -1) {
		upsdebugx(1,"unexpected fcntl(fd, F_GETFL) failure");
		/* close(p.fd);
		return -1; */
		ret = -1;
		goto getdata_return;
	}
	fd_flags |= O_NONBLOCK;
	if(fcntl(p.fd, F_SETFL, fd_flags) == -1)
	{
		upsdebugx(1,"unexpected fcntl(fd, F_SETFL, fd_flags|O_NONBLOCK) failure");
		/* close(p.fd);
		return -1; */
		ret = -1;
		goto getdata_return;
	}
#else	/* WIN32 */
	event = CreateEvent(
		NULL,  /* Security */
		FALSE, /* auto-reset */
		FALSE, /* initial state */
		NULL); /* no name */

	/* Associate socket event to the socket via its Event object */
	WSAEventSelect( p.fd, event, FD_CONNECT );
#endif	/* WIN32 */

	p.events=POLLIN;

	n=htons(6);
	x=write(p.fd,&n,2);
	x=write(p.fd,"status",6);

	/* TODO: double-check for poll() in configure script */
#ifndef WIN32
	while(poll(&p,1,15000)==1)
#else	/* WIN32 */
	while (WaitForMultipleObjects(1, &event, FALSE, 15000) == WAIT_TIMEOUT)
#endif	/* WIN32 */
	{
		if(read(p.fd,&n,2)!=2)
		{
			upsdebugx(1,"apcupsd communication error");
			ret = -1;
			goto getdata_return;
		}

		if(!(x=ntohs(n)))
		{
			ret = 0;
			goto getdata_return;
		}
		else if(x<0||x>=(int)sizeof(bfr))
		/* Note: LGTM.com suggests "Comparison is always false because x >= 0"
		 * for the line above, probably because ntohs() returns an uint type.
		 * I am reluctant to fix this one, because googling for headers from
		 * random OSes showed various types used as the return value (uint16_t,
		 * unsigned_short, u_short, in_port_t...)
		 */
		{
			upsdebugx(1,"apcupsd communication error");
			ret = -1;
			goto getdata_return;
		}

#ifndef WIN32
		if(poll(&p,1,15000)!=1)break;
#else	/* WIN32 */
		if (WaitForMultipleObjects(1, &event, FALSE, 15000) != WAIT_OBJECT_0) break;
#endif	/* WIN32 */

		if(read(p.fd,bfr,(size_t)x)!=x)
		{
			upsdebugx(1,"apcupsd communication error");
			ret = -1;
			goto getdata_return;
		}

		bfr[x]=0;

		if(!(item=strtok(bfr," \t:\r\n")))
		{
			upsdebugx(1,"apcupsd communication error");
			ret = -1;
			goto getdata_return;
		}

		if(!(data=strtok(NULL,"\r\n")))
		{
			upsdebugx(1,"apcupsd communication error");
			ret = -1;
			goto getdata_return;
		}
		while(*data==' '||*data=='\t'||*data==':')data++;

		process(item,data);
	}

	upsdebugx(1,"unexpected connection close by apcupsd");
	ret = -1;

getdata_return:
	if (VALID_FD_SOCK(p.fd))
		close(p.fd);
#ifdef WIN32
	if (event != NULL)
		CloseHandle(event);
#endif	/* WIN32 */

	/* Remove any unprotected entries not refreshed in this run */
	for(x=0;nut_data[x].info_type;x++)
		if(!(nut_data[x].drv_flags & DU_FLAG_INIT) && !(nut_data[x].drv_flags & DU_FLAG_PRESERVE))
			dstate_delinfo_olderthan(nut_data[x].info_type, &start);

	return ret;
}

void upsdrv_initinfo(void)
{
	if(!port)fatalx(EXIT_FAILURE,"invalid host or port specified!");
	if(getdata())fatalx(EXIT_FAILURE,"can't communicate with apcupsd!");
	else dstate_dataok();

	poll_interval = (poll_interval < POLL_INTERVAL_MIN) ? POLL_INTERVAL_MIN : poll_interval;
}

void upsdrv_updateinfo(void)
{
	if(getdata())upslogx(LOG_ERR,"can't communicate with apcupsd!");
	else dstate_dataok();

	poll_interval = (poll_interval < POLL_INTERVAL_MIN) ? POLL_INTERVAL_MIN : poll_interval;
}

void upsdrv_shutdown(void)
{
	/* Only implement "shutdown.default"; do not invoke
	 * general handling of other `sdcommands` here */

	/* replace with a proper shutdown function */
	upslogx(LOG_ERR, "shutdown not supported");
	if (handling_upsdrv_shutdown > 0)
		set_exit_flag(EF_EXIT_FAILURE);
}

void upsdrv_help(void)
{
}

void upsdrv_makevartable(void)
{
}

void upsdrv_initups(void)
{
	char *p;
	struct hostent *h;

#ifdef WIN32
	WSADATA WSAdata;
	WSAStartup(2,&WSAdata);
	atexit((void(*)(void))WSACleanup);
#endif	/* WIN32 */

	if(device_path&&*device_path)
	{
		/* TODO: fix parsing since bare IPv6 addresses contain colons */
		if((p=strchr(device_path,':')))
		{
			int i;
			*p++=0;
			i=atoi(p);
			if(i<1||i>65535)i=0;
			port = (uint16_t)i;
		}
	}
	else device_path="localhost";

	if(!(h=gethostbyname(device_path)))port=0;
	else memcpy(&host.sin_addr,h->h_addr,4);

	/* TODO: add IPv6 support */
	host.sin_family=AF_INET;
	host.sin_port=htons(port);
}

void upsdrv_cleanup(void)
{
}
