/* FreeTDS - Library of routines accessing Sybase and Microsoft databases
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003  Brian Bruns
 * Copyright (C) 2004  Ziglio Frediano
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#if TIME_WITH_SYS_TIME
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# endif
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#if HAVE_ERRNO_H
#include <errno.h>
#endif /* HAVE_ERRNO_H */

#include <stdio.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */

#if HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */

#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif /* HAVE_SYS_SOCKET_H */

#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif /* HAVE_NETINET_IN_H */

#if HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif /* HAVE_NETINET_TCP_H */

#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif /* HAVE_ARPA_INET_H */

#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif /* HAVE_SYS_IOCTL_H */

#if HAVE_SELECT_H
#include <sys/select.h>
#endif /* HAVE_SELECT_H */

#include "tds.h"
#include "tdsstring.h"

#include <signal.h>
#include <assert.h>

#ifdef DMALLOC
#include <dmalloc.h>
#endif

static char software_version[] = "$Id: net.c,v 1.5 2004/12/07 22:39:21 jklowden Exp $";
static void *no_unused_var_warn[] = { software_version, no_unused_var_warn };

/** \addtogroup network
 *  \@{ 
 */

#if !defined(SOL_TCP) && defined(IPPROTO_TCP)
#define SOL_TCP IPPROTO_TCP
#endif

/* Optimize the way we send packets */
#undef USE_MSGMORE
#undef USE_CORK
#undef USE_NODELAY
/* On Linux 2.4.x we can use MSG_MORE */
#if defined(__linux__) && defined(MSG_MORE)
#define USE_MSGMORE 1
/* On early Linux use TCP_CORK if available */
#elif defined(__linux__) && defined(TCP_CORK)
#define USE_CORK 1
/* On *BSD use TCP_NOPUSH (same bahavior of TCP_CORK) */
#elif (defined(__FreeBSD__) || defined(__GNU_FreeBSD__) || defined(__OpenBSD__)) && defined(TCP_NOPUSH)
#define USE_CORK 1
#define TCP_CORK TCP_NOPUSH
/* otherwise use NODELAY */
#elif defined(TCP_NODELAY) && defined(SOL_TCP)
#define USE_NODELAY 1
#endif

int
tds_open_socket(TDSSOCKET * tds, const char *ip_addr, unsigned int port, int timeout)
{
	struct sockaddr_in sin;
	unsigned long ioctl_blocking = 1;
	struct timeval selecttimeout;
	fd_set fds;
	time_t start, now;
	int len, retval;

	FD_ZERO(&fds);

	sin.sin_addr.s_addr = inet_addr(ip_addr);
	if (sin.sin_addr.s_addr == INADDR_NONE) {
		tdsdump_log(TDS_DBG_ERROR, "inet_addr() failed, IP = %s\n", ip_addr);
		return TDS_FAIL;
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

	tdsdump_log(TDS_DBG_INFO1, "Connecting to %s port %d.\n", inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));

	if (TDS_IS_SOCKET_INVALID(tds->s = socket(AF_INET, SOCK_STREAM, 0))) {
		tdsdump_log(TDS_DBG_ERROR, "socket creation error: %s\n", strerror(sock_errno));
		return TDS_FAIL;
	}

#ifdef SO_KEEPALIVE
	len = 1;
	setsockopt(tds->s, SOL_SOCKET, SO_KEEPALIVE, (const void *) &len, sizeof(len));
#endif

	len = 1;
#if defined(USE_NODELAY) || defined(USE_MSGMORE)
	setsockopt(tds->s, SOL_TCP, TCP_NODELAY, (const void *) &len, sizeof(len));
#elif defined(USE_CORK)
	if (setsockopt(tds->s, SOL_TCP, TCP_CORK, (const void *) &len, sizeof(len)) < 0)
		setsockopt(tds->s, SOL_TCP, TCP_NODELAY, (const void *) &len, sizeof(len));
#else
#error One should be defined
#endif
	
	/* Jeff's hack *** START OF NEW CODE *** */
	if (timeout) {
		start = time(NULL);
		ioctl_blocking = 1;	/* ~0; //TRUE; */
		if (IOCTLSOCKET(tds->s, FIONBIO, &ioctl_blocking) < 0)
			return TDS_FAIL;

		retval = connect(tds->s, (struct sockaddr *) &sin, sizeof(sin));
		if (retval < 0 && sock_errno == TDSSOCK_EINPROGRESS)
			retval = 0;
		if (retval < 0) {
			tdsdump_log(TDS_DBG_ERROR, "tds_open_socket (timed): %s\n", strerror(sock_errno));
			return TDS_FAIL;
		}
		/* Select on writeability for connect_timeout */
		now = start;
		while ((retval == 0) && ((now - start) < timeout)) {
			FD_SET(tds->s, &fds);
			selecttimeout.tv_sec = timeout - (now - start);
			selecttimeout.tv_usec = 0;
			retval = select(tds->s + 1, NULL, &fds, NULL, &selecttimeout);
			if (retval < 0 && sock_errno == TDSSOCK_EINTR)
				retval = 0;
			now = time(NULL);
		}

		if ((now - start) >= timeout) {
			tds_client_msg(tds->tds_ctx, tds, 20009, 9, 0, 0, "Server is unavailable or does not exist.");
			return TDS_FAIL;
		}
	} else if (connect(tds->s, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		tdsdump_log(TDS_DBG_ERROR, "tds_open_socket: %s:%d: %s\n", inet_ntoa(sin.sin_addr), ntohs(sin.sin_port), strerror(sock_errno));
		tds_client_msg(tds->tds_ctx, tds, 20009, 9, 0, 0, "Server is unavailable or does not exist.");
		return TDS_FAIL;
	}
	/* END OF NEW CODE */

	return TDS_SUCCEED;
}

int
tds_close_socket(TDSSOCKET * tds)
{
	int rc = -1;

	if (!IS_TDSDEAD(tds)) {
		rc = CLOSESOCKET(tds->s);
		tds->s = INVALID_SOCKET;
		tds_set_state(tds, TDS_DEAD);
	}
	return rc;
}

/**
 * Loops until we have received buflen characters 
 * return -1 on failure 
 */
static int
goodread(TDSSOCKET * tds, unsigned char *buf, int buflen)
{
	int got = 0;
	int len, retcode;
	fd_set fds;
	time_t start, now;
	struct timeval selecttimeout;
	struct timeval *timeout;

	assert(tds);

	FD_ZERO(&fds);
	start = time(NULL);
	now = start;
	while ((buflen > 0) && ((tds->timeout == 0) || ((now - start) < tds->timeout))) {
		if (IS_TDSDEAD(tds))
			return -1;

		/* set right timeout */
		FD_SET(tds->s, &fds);
		timeout = NULL;
		if (tds->chkintr || tds->timeout) {
			selecttimeout.tv_sec = tds->chkintr ? 1 : tds->timeout - (now - start);
			selecttimeout.tv_usec = 0;
			timeout = &selecttimeout;
		}

		retcode = select(tds->s + 1, &fds, NULL, NULL, timeout);	/* retcode == 0 indicates a timeout, OK */

		if( retcode != 0 ) {
			if( retcode < 0 ) {
				if (sock_errno != TDSSOCK_EINTR) {
					char *msg = strerror(sock_errno);
					tdsdump_log(TDS_DBG_NETWORK, "goodread select: errno=%d, \"%s\", returning -1\n", sock_errno, (msg)? msg : "(unknown)");
					return -1;
				}
				goto OK_TIMEOUT;
			} 
			/*
			 * select succeeded: let's read.
			 */
#			ifndef MSG_NOSIGNAL
			len = READSOCKET(tds->s, buf + got, buflen);
# 			else
			len = recv(tds->s, buf + got, buflen, MSG_NOSIGNAL);
# 			endif

			if (len < 0) {
				char *msg = strerror(sock_errno);
				tdsdump_log(TDS_DBG_NETWORK, "goodread: errno=%d, \"%s\"\n", sock_errno, (msg)? msg : "(unknown)");
				
				switch (sock_errno) {
				case EAGAIN:		/* If O_NONBLOCK is set, read(2) returns -1 and sets errno to [EAGAIN]. */
				case TDSSOCK_EINTR:		/* If interrupted by a signal before it reads any data. */
				case TDSSOCK_EINPROGRESS:	/* A lengthy operation on a non-blocking object is in progress. */
					/* EINPROGRESS is not a documented errno for read(2), afaict.  Remove following assert if it trips.  --jkl */
					assert(sock_errno != TDSSOCK_EINPROGRESS);
					goto OK_TIMEOUT; /* try again */
					break;

				case EBADF:
				/*   EBADMSG: not always defined */
				case EDEADLK:
				case EFAULT:
				case EINVAL:
				case EIO:
				case ENOLCK:
				case ENOSPC:
				case ENXIO:
				default:
					return -1;
					break;
				}
			}

			/* this means a disconnection from server, exit */
			/* TODO close sockets too ?? */
			if (len == 0) 
				return -1;

			buflen -= len;
			got += len;
		} 


	OK_TIMEOUT:
		now = time(NULL);
		if (tds->longquery_func && tds->queryStarttime && tds->longquery_timeout) {
			if ((now - (tds->queryStarttime)) >= tds->longquery_timeout) {
				(*tds->longquery_func) (tds->longquery_param);
				return got;
			}
		}
		if ((tds->chkintr) && ((*tds->chkintr) (tds)) && (tds->hndlintr)) {
			switch ((*tds->hndlintr) (tds)) {
			case TDS_INT_EXIT:
				exit(EXIT_FAILURE);
				break;
			case TDS_INT_CANCEL:
				/* TODO should we process cancellation ?? */
				tds_send_cancel(tds);
				break;
			case TDS_INT_CONTINUE:
			default:
				break;
			}
		}

	}			/* while buflen... */

	/* here buflen <= 0 || (tds->timeout != 0 && (now - start) >= tds->timeout) */
		
	/* TODO always false ?? */
	if (tds->timeout > 0 && now - start < tds->timeout && buflen > 0)
		return -1;

	/* FIXME on timeout this assert got true... */
	assert(buflen == 0);
	return (got);
}

/**
 * Read in one 'packet' from the server.  This is a wrapped outer packet of
 * the protocol (they bundle result packets into chunks and wrap them at
 * what appears to be 512 bytes regardless of how that breaks internal packet
 * up.   (tetherow\@nol.org)
 * @return bytes read or -1 on failure
 */
int
tds_read_packet(TDSSOCKET * tds)
{
	unsigned char header[8];
	int len;
	int x = 0, have, need;

	if (IS_TDSDEAD(tds)) {
		tdsdump_log(TDS_DBG_NETWORK, "Read attempt when state is TDS_DEAD");
		return -1;
	}

	/*
	 * Read in the packet header.  We use this to figure out our packet
	 * length
	 */

	/*
	 * Cast to int are needed because some compiler seem to convert
	 * len to unsigned (as FreeBSD 4.5 one)
	 */
	if ((len = goodread(tds, header, sizeof(header))) < (int) sizeof(header)) {
		/* GW ADDED */
		if (len < 0) {
			tds_client_msg(tds->tds_ctx, tds, 20004, 9, 0, 0, "Read from SQL server failed.");
			tds_close_socket(tds);
			tds->in_len = 0;
			tds->in_pos = 0;
			return -1;
		}

		/* GW ADDED */
		/*
		 * Not sure if this is the best way to do the error
		 * handling here but this is the way it is currently
		 * being done.
		 */

		tds->in_len = 0;
		tds->in_pos = 0;
		tds->last_packet = 1;
		if (tds->state != TDS_IDLE && len == 0) {
			tds_close_socket(tds);
		}
		return -1;
	}
	tdsdump_dump_buf(TDS_DBG_NETWORK, "Received header", header, sizeof(header));

#if 0
	/*
	 * Note:
	 * this was done by Gregg, I don't think its the real solution (it breaks
	 * under 5.0, but I haven't gotten a result big enough to test this yet.
 	 */
	if (IS_TDS42(tds)) {
		if (header[0] != 0x04 && header[0] != 0x0f) {
			tdsdump_log(TDS_DBG_ERROR, "Invalid packet header %d\n", header[0]);
			/*  Not sure if this is the best way to do the error 
			 *  handling here but this is the way it is currently 
			 *  being done. */
			tds->in_len = 0;
			tds->in_pos = 0;
			tds->last_packet = 1;
			return (-1);
		}
	}
#endif

	/* Convert our packet length from network to host byte order */
	len = ((((unsigned int) header[2]) << 8) | header[3]) - 8;
	need = len;

	/*
	 * If this packet size is the largest we have gotten allocate
	 * space for it
	 */
	if (len > tds->in_buf_max) {
		unsigned char *p;

		if (!tds->in_buf) {
			p = (unsigned char *) malloc(len);
		} else {
			p = (unsigned char *) realloc(tds->in_buf, len);
		}
		if (!p)
			return -1;	/* FIXME should close socket too */
		tds->in_buf = p;
		/* Set the new maximum packet size */
		tds->in_buf_max = len;
	}

	/* Clean out the in_buf so we don't use old stuff by mistake */
	memset(tds->in_buf, 0, tds->in_buf_max);

	/* Now get exactly how many bytes the server told us to get */
	have = 0;
	while (need > 0) {
		if ((x = goodread(tds, tds->in_buf + have, need)) < 1) {
			/*
			 * Not sure if this is the best way to do the error
			 * handling here but this is the way it is currently
			 * being done.
			 */
			tds->in_len = 0;
			tds->in_pos = 0;
			tds->last_packet = 1;
			/* FIXME should this be "if (x == 0)" ? */
			if (len == 0) {
				tds_close_socket(tds);
			}
			return (-1);
		}
		have += x;
		need -= x;
	}
	if (x < 1) {
		/*
		 * Not sure if this is the best way to do the error handling
		 * here but this is the way it is currently being done.
		 */
		tds->in_len = 0;
		tds->in_pos = 0;
		tds->last_packet = 1;
		/* return 0 if header found but no payload */
		return len ? -1 : 0;
	}

	/* Set the last packet flag */
	if (header[1]) {
		tds->last_packet = 1;
	} else {
		tds->last_packet = 0;
	}

	/* set the received packet type flag */
	tds->in_flag = header[0];

	/* Set the length and pos (not sure what pos is used for now */
	tds->in_len = have;
	tds->in_pos = 0;
	tdsdump_dump_buf(TDS_DBG_NETWORK, "Received packet", tds->in_buf, tds->in_len);

	return (tds->in_len);
}

/* TODO this code should be similar to read one... */
static int
tds_check_socket_write(TDSSOCKET * tds)
{
	int retcode = 0;
	struct timeval selecttimeout;
	time_t start, now;
	fd_set fds;

	/* Jeffs hack *** START OF NEW CODE */
	FD_ZERO(&fds);

	if (!tds->timeout) {
		for (;;) {
			FD_SET(tds->s, &fds);
			retcode = select(tds->s + 1, NULL, &fds, NULL, NULL);
			/* write available */
			if (retcode >= 0)
				return 0;
			/* interrupted */
			if (sock_errno == TDSSOCK_EINTR)
				continue;
			/* error, leave caller handle problems */
			return -1;
		}
	}
	start = time(NULL);
	now = start;

	while ((retcode == 0) && ((now - start) < tds->timeout)) {
		FD_SET(tds->s, &fds);
		selecttimeout.tv_sec = tds->timeout - (now - start);
		selecttimeout.tv_usec = 0;
		retcode = select(tds->s + 1, NULL, &fds, NULL, &selecttimeout);
		if (retcode < 0 && sock_errno == TDSSOCK_EINTR) {
			retcode = 0;
		}

		now = time(NULL);
	}

	return retcode;
	/* Jeffs hack *** END OF NEW CODE */
}

/* goodwrite function adapted from patch by freddy77 */
static int
goodwrite(TDSSOCKET * tds, unsigned char last)
{
	int left;
	unsigned char *p;
	int retval;

	left = tds->out_pos;
	p = tds->out_buf;

	while (left > 0) {
		/*
		 * If there's a timeout, we need to sit and wait for socket
		 * writability
		 * moved socket writability check to own function -- bsb
		 */
		/* 
		 * TODO we can avoid calling select for every send using 
		 * no-blocking socket... This will reduce syscalls
		 */
		tds_check_socket_write(tds);

#ifdef USE_MSGMORE
		retval = send(tds->s, p, left, last ? MSG_NOSIGNAL : MSG_NOSIGNAL|MSG_MORE);
#elif !defined(MSG_NOSIGNAL)
		retval = WRITESOCKET(tds->s, p, left);
#else
		retval = send(tds->s, p, left, MSG_NOSIGNAL);
#endif

		if (retval <= 0) {
			tdsdump_log(TDS_DBG_NETWORK, "TDS: Write failed in tds_write_packet\nError: %d (%s)\n", sock_errno, strerror(sock_errno));
			tds_client_msg(tds->tds_ctx, tds, 20006, 9, 0, 0, "Write to SQL Server failed.");
			tds->in_pos = 0;
			tds->in_len = 0;
			tds_close_socket(tds);
			return TDS_FAIL;
		}
		left -= retval;
		p += retval;
	}

#ifdef USE_CORK
	/* force packet flush */
	if (last) {
		int opt;
		opt = 0;
		setsockopt(tds->s, SOL_TCP, TCP_CORK, (const void *) &opt, sizeof(opt));
		opt = 1;
		setsockopt(tds->s, SOL_TCP, TCP_CORK, (const void *) &opt, sizeof(opt));
	}
#endif

	return TDS_SUCCEED;
}

int
tds_write_packet(TDSSOCKET * tds, unsigned char final)
{
	int retcode;

#if !defined(WIN32) && !defined(MSG_NOSIGNAL)
	void (*oldsig) (int);
#endif

	tds->out_buf[0] = tds->out_flag;
	tds->out_buf[1] = final;
	tds->out_buf[2] = (tds->out_pos) / 256u;
	tds->out_buf[3] = (tds->out_pos) % 256u;
	if (IS_TDS70(tds) || IS_TDS80(tds)) {
		tds->out_buf[6] = 0x01;
	}

	tdsdump_dump_buf(TDS_DBG_NETWORK, "Sending packet", tds->out_buf, tds->out_pos);

#if !defined(WIN32) && !defined(MSG_NOSIGNAL)
	oldsig = signal(SIGPIPE, SIG_IGN);
	if (oldsig == SIG_ERR) {
		tdsdump_log(TDS_DBG_WARN, "TDS: Warning: Couldn't set SIGPIPE signal to be ignored\n");
	}
#endif

	retcode = goodwrite(tds, final);

#if !defined(WIN32) && !defined(MSG_NOSIGNAL)
	if (signal(SIGPIPE, oldsig) == SIG_ERR) {
		tdsdump_log(TDS_DBG_WARN, "TDS: Warning: Couldn't reset SIGPIPE signal to previous value\n");
	}
#endif

	/* GW added in check for write() returning <0 and SIGPIPE checking */
	return retcode;
}

/** \@} */
