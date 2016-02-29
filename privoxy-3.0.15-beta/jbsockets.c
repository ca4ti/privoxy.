const char jbsockets_rcs[] = "$Id: jbsockets.c,v 1.68 2009/10/04 16:38:26 fabiankeil Exp $";
/*********************************************************************
 *
 * File        :  $Source: /cvsroot/ijbswa/current/jbsockets.c,v $
 *
 * Purpose     :  Contains wrappers for system-specific sockets code,
 *                so that the rest of Junkbuster can be more
 *                OS-independent.  Contains #ifdefs to make this work
 *                on many platforms.
 *
 * Copyright   :  Written by and Copyright (C) 2001-2009 the
 *                Privoxy team. http://www.privoxy.org/
 *
 *                Based on the Internet Junkbuster originally written
 *                by and Copyright (C) 1997 Anonymous Coders and 
 *                Junkbusters Corporation.  http://www.junkbusters.com
 *
 *                This program is free software; you can redistribute it 
 *                and/or modify it under the terms of the GNU General
 *                Public License as published by the Free Software
 *                Foundation; either version 2 of the License, or (at
 *                your option) any later version.
 *
 *                This program is distributed in the hope that it will
 *                be useful, but WITHOUT ANY WARRANTY; without even the
 *                implied warranty of MERCHANTABILITY or FITNESS FOR A
 *                PARTICULAR PURPOSE.  See the GNU General Public
 *                License for more details.
 *
 *                The GNU General Public License should be included with
 *                this file.  If not, you can view it at
 *                http://www.gnu.org/copyleft/gpl.html
 *                or write to the Free Software Foundation, Inc., 59
 *                Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *********************************************************************/


#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>

#ifdef _WIN32

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <sys/timeb.h>
#include <io.h>

#else

#ifndef __OS2__
#include <unistd.h>
#endif
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <sys/socket.h>

#ifndef __BEOS__
#include <netinet/tcp.h>
#ifndef __OS2__
#include <arpa/inet.h>
#endif
#else
#include <socket.h>
#endif

#if defined(__EMX__) || defined (__OS2__)
#include <sys/select.h>  /* OS/2/EMX needs a little help with select */
#ifdef __OS2__
#include <nerrno.h>
#endif
#endif

#endif

#ifdef FEATURE_CONNECTION_KEEP_ALIVE
#ifdef HAVE_POLL
#ifdef __GLIBC__
#include <sys/poll.h>
#else
#include <poll.h>
#endif /* def __GLIBC__ */
#endif /* HAVE_POLL */
#endif /* def FEATURE_CONNECTION_KEEP_ALIVE */

#include "project.h"

/* For mutex semaphores only */
#include "jcc.h"

#include "jbsockets.h"
#include "filters.h"
#include "errlog.h"
#include "miscutil.h"

/* Mac OSX doesn't define AI_NUMERICSESRV */
#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif

const char jbsockets_h_rcs[] = JBSOCKETS_H_VERSION;

/*
 * Maximum number of gethostbyname(_r) retries in case of
 * soft errors (TRY_AGAIN).
 * XXX: Does it make sense to make this a config option?
 */
#define MAX_DNS_RETRIES 10

#define MAX_LISTEN_BACKLOG 128


/*********************************************************************
 *
 * Function    :  connect_to
 *
 * Description :  Open a socket and connect to it.  Will check
 *                that this is allowed according to ACL.
 *
 * Parameters  :
 *          1  :  host = hostname to connect to
 *          2  :  portnum = port to connent on (XXX: should be unsigned)
 *          3  :  csp = Current client state (buffers, headers, etc...)
 *                      Not modified, only used for source IP and ACL.
 *
 * Returns     :  JB_INVALID_SOCKET => failure, else it is the socket
 *                file descriptor.
 *
 *********************************************************************/
#ifdef HAVE_RFC2553
/* Getaddrinfo implementation */
jb_socket connect_to(const char *host, int portnum, struct client_state *csp)
{
   struct addrinfo hints, *result, *rp;
   char service[6];
   int retval;
   jb_socket fd;
   fd_set wfds;
   struct timeval tv[1];
#if !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA)
   int   flags;
#endif /* !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) */
   int connect_failed;

#ifdef FEATURE_ACL
   struct access_control_addr dst[1];
#endif /* def FEATURE_ACL */

   retval = snprintf(service, sizeof(service), "%d", portnum);
   if ((-1 == retval) || (sizeof(service) <= retval))
   {
      log_error(LOG_LEVEL_ERROR,
         "Port number (%d) ASCII decimal representation doesn't fit into 6 bytes",
         portnum);
      csp->http->host_ip_addr_str = strdup("unknown");
      return(JB_INVALID_SOCKET);
   }

   memset((char *)&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_NUMERICSERV; /* avoid service look-up */
#ifdef AI_ADDRCONFIG
   hints.ai_flags |= AI_ADDRCONFIG;
#endif
   if ((retval = getaddrinfo(host, service, &hints, &result)))
   {
      log_error(LOG_LEVEL_INFO,
         "Can not resolve %s: %s", host, gai_strerror(retval));
      csp->http->host_ip_addr_str = strdup("unknown");
      return(JB_INVALID_SOCKET);
   }

   for (rp = result; rp != NULL; rp = rp->ai_next)
   {

#ifdef FEATURE_ACL
      memcpy(&dst->addr, rp->ai_addr, rp->ai_addrlen);

      if (block_acl(dst, csp))
      {
#ifdef __OS2__
         errno = SOCEPERM;
#else
         errno = EPERM;
#endif
         continue;
      }
#endif /* def FEATURE_ACL */

      csp->http->host_ip_addr_str = malloc(NI_MAXHOST);
      if (NULL == csp->http->host_ip_addr_str)
      {
         log_error(LOG_LEVEL_ERROR,
            "Out of memory while getting the server IP address.");
         return JB_INVALID_SOCKET;
      }
      retval = getnameinfo(rp->ai_addr, rp->ai_addrlen,
         csp->http->host_ip_addr_str, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
      if (!csp->http->host_ip_addr_str || retval)
      {
         log_error(LOG_LEVEL_ERROR,
            "Can not save csp->http->host_ip_addr_str: %s",
            (csp->http->host_ip_addr_str) ?
            gai_strerror(retval) : "Insufficient memory");
         freez(csp->http->host_ip_addr_str);
         continue;
      }

#ifdef _WIN32
      if ((fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) ==
            JB_INVALID_SOCKET)
#else
      if ((fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0)
#endif
      {
         continue;
      }

#ifdef TCP_NODELAY
      {  /* turn off TCP coalescence */
         int mi = 1;
         setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &mi, sizeof (int));
      }
#endif /* def TCP_NODELAY */

#if !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__)
      if ((flags = fcntl(fd, F_GETFL, 0)) != -1)
      {
         flags |= O_NDELAY;
         fcntl(fd, F_SETFL, flags);
      }
#endif /* !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__) */

      connect_failed = 0;
      while (connect(fd, rp->ai_addr, rp->ai_addrlen) == JB_INVALID_SOCKET)
      {
#ifdef _WIN32
         if (errno == WSAEINPROGRESS)
#elif __OS2__
         if (sock_errno() == EINPROGRESS)
#else /* ifndef _WIN32 */
         if (errno == EINPROGRESS)
#endif /* ndef _WIN32 || __OS2__ */
         {
            break;
         }

#ifdef __OS2__
         if (sock_errno() != EINTR)
#else
         if (errno != EINTR)
#endif /* __OS2__ */
         {
            close_socket(fd);
            connect_failed = 1;
            break;
         }
      }
      if (connect_failed)
      {
         continue;
      }

#if !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__)
      if (flags != -1)
      {
         flags &= ~O_NDELAY;
         fcntl(fd, F_SETFL, flags);
      }
#endif /* !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__) */

      /* wait for connection to complete */
      FD_ZERO(&wfds);
      FD_SET(fd, &wfds);

      tv->tv_sec  = 30;
      tv->tv_usec = 0;

      /* MS Windows uses int, not SOCKET, for the 1st arg of select(). Wierd! */
      if ((select((int)fd + 1, NULL, &wfds, NULL, tv) > 0)
         && FD_ISSET(fd, &wfds))
      {
         /*
          * See Linux connect(2) man page for more info
          * about connecting on non-blocking socket.
          */
         int socket_in_error;
         socklen_t optlen = sizeof(socket_in_error);
         if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_in_error, &optlen))
         {
            if (!socket_in_error)
            {
               /* Connection established, no need to try other addresses. */
               break;
            }
            log_error(LOG_LEVEL_CONNECT, "Could not connect to [%s]:%s: %s.",
               csp->http->host_ip_addr_str, service, strerror(socket_in_error));
         }
         else
         {
            log_error(LOG_LEVEL_ERROR, "Could not get the state of "
               "the connection to [%s]:%s: %s; dropping connection.",
               csp->http->host_ip_addr_str, service, strerror(errno));
         }
      }

      /* Connection failed, try next address */
      close_socket(fd);
   }

   freeaddrinfo(result);
   if (!rp)
   {
      log_error(LOG_LEVEL_CONNECT, "Could not connect to [%s]:%s.",
         host, service);
      return(JB_INVALID_SOCKET);
   }
   log_error(LOG_LEVEL_CONNECT, "Connected to %s[%s]:%s.",
      host, csp->http->host_ip_addr_str, service);

   return(fd);

}

#else /* ndef HAVE_RFC2553 */
/* Pre-getaddrinfo implementation */

jb_socket connect_to(const char *host, int portnum, struct client_state *csp)
{
   struct sockaddr_in inaddr;
   jb_socket fd;
   unsigned int addr;
   fd_set wfds;
   struct timeval tv[1];
#if !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA)
   int   flags;
#endif /* !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) */

#ifdef FEATURE_ACL
   struct access_control_addr dst[1];
#endif /* def FEATURE_ACL */

   memset((char *)&inaddr, 0, sizeof inaddr);

   if ((addr = resolve_hostname_to_ip(host)) == INADDR_NONE)
   {
      csp->http->host_ip_addr_str = strdup("unknown");
      return(JB_INVALID_SOCKET);
   }

#ifdef FEATURE_ACL
   dst->addr = ntohl(addr);
   dst->port = portnum;

   if (block_acl(dst, csp))
   {
#ifdef __OS2__
      errno = SOCEPERM;
#else
      errno = EPERM;
#endif
      return(JB_INVALID_SOCKET);
   }
#endif /* def FEATURE_ACL */

   inaddr.sin_addr.s_addr = addr;
   inaddr.sin_family      = AF_INET;
   csp->http->host_ip_addr_str = strdup(inet_ntoa(inaddr.sin_addr));

#ifndef _WIN32
   if (sizeof(inaddr.sin_port) == sizeof(short))
#endif /* ndef _WIN32 */
   {
      inaddr.sin_port = htons((unsigned short) portnum);
   }
#ifndef _WIN32
   else
   {
      inaddr.sin_port = htonl((unsigned long)portnum);
   }
#endif /* ndef _WIN32 */

#ifdef _WIN32
   if ((fd = socket(inaddr.sin_family, SOCK_STREAM, 0)) == JB_INVALID_SOCKET)
#else
   if ((fd = socket(inaddr.sin_family, SOCK_STREAM, 0)) < 0)
#endif
   {
      return(JB_INVALID_SOCKET);
   }

#ifdef TCP_NODELAY
   {  /* turn off TCP coalescence */
      int mi = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &mi, sizeof (int));
   }
#endif /* def TCP_NODELAY */

#if !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__)
   if ((flags = fcntl(fd, F_GETFL, 0)) != -1)
   {
      flags |= O_NDELAY;
      fcntl(fd, F_SETFL, flags);
   }
#endif /* !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__) */

   while (connect(fd, (struct sockaddr *) & inaddr, sizeof inaddr) == JB_INVALID_SOCKET)
   {
#ifdef _WIN32
      if (errno == WSAEINPROGRESS)
#elif __OS2__ 
      if (sock_errno() == EINPROGRESS)
#else /* ifndef _WIN32 */
      if (errno == EINPROGRESS)
#endif /* ndef _WIN32 || __OS2__ */
      {
         break;
      }

#ifdef __OS2__ 
      if (sock_errno() != EINTR)
#else
      if (errno != EINTR)
#endif /* __OS2__ */
      {
         close_socket(fd);
         return(JB_INVALID_SOCKET);
      }
   }

#if !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__)
   if (flags != -1)
   {
      flags &= ~O_NDELAY;
      fcntl(fd, F_SETFL, flags);
   }
#endif /* !defined(_WIN32) && !defined(__BEOS__) && !defined(AMIGA) && !defined(__OS2__) */

   /* wait for connection to complete */
   FD_ZERO(&wfds);
   FD_SET(fd, &wfds);

   tv->tv_sec  = 30;
   tv->tv_usec = 0;

   /* MS Windows uses int, not SOCKET, for the 1st arg of select(). Wierd! */
   if (select((int)fd + 1, NULL, &wfds, NULL, tv) <= 0)
   {
      close_socket(fd);
      return(JB_INVALID_SOCKET);
   }
   return(fd);

}
#endif /* ndef HAVE_RFC2553 */


/*********************************************************************
 *
 * Function    :  write_socket
 *
 * Description :  Write the contents of buf (for n bytes) to socket fd.
 *
 * Parameters  :
 *          1  :  fd = file descriptor (aka. handle) of socket to write to.
 *          2  :  buf = pointer to data to be written.
 *          3  :  len = length of data to be written to the socket "fd".
 *
 * Returns     :  0 on success (entire buffer sent).
 *                nonzero on error.
 *
 *********************************************************************/
#ifdef AMIGA
int write_socket(jb_socket fd, const char *buf, ssize_t len)
#else
int write_socket(jb_socket fd, const char *buf, size_t len)
#endif
{
   if (len == 0)
   {
      return 0;
   }

   if (len < 0) /* constant condition - size_t isn't ever negative */ 
   {
      return 1;
   }

   log_error(LOG_LEVEL_LOG, "%N", len, buf);

#if defined(_WIN32)
   return (send(fd, buf, (int)len, 0) != (int)len);
#elif defined(__BEOS__) || defined(AMIGA)
   return (send(fd, buf, len, 0) != len);
#elif defined(__OS2__)
   /*
    * Break the data up into SOCKET_SEND_MAX chunks for sending...
    * OS/2 seemed to complain when the chunks were too large.
    */
#define SOCKET_SEND_MAX 65000
   {
      int write_len = 0, send_len, send_rc = 0, i = 0;
      while ((i < len) && (send_rc != -1))
      {
         if ((i + SOCKET_SEND_MAX) > len)
            send_len = len - i;
         else
            send_len = SOCKET_SEND_MAX;
         send_rc = send(fd,(char*)buf + i, send_len, 0);
         if (send_rc == -1)
            return 1;
         i = i + send_len;
      }
      return 0;
   }
#else
   return (write(fd, buf, len) != len);
#endif

}


/*********************************************************************
 *
 * Function    :  read_socket
 *
 * Description :  Read from a TCP/IP socket in a platform independent way.
 *
 * Parameters  :
 *          1  :  fd = file descriptor of the socket to read
 *          2  :  buf = pointer to buffer where data will be written
 *                Must be >= len bytes long.
 *          3  :  len = maximum number of bytes to read
 *
 * Returns     :  On success, the number of bytes read is returned (zero
 *                indicates end of file), and the file position is advanced
 *                by this number.  It is not an error if this number is
 *                smaller than the number of bytes requested; this may hap-
 *                pen for example because fewer bytes are actually available
 *                right now (maybe because we were close to end-of-file, or
 *                because we are reading from a pipe, or from a terminal,
 *                or because read() was interrupted by a signal).  On error,
 *                -1 is returned, and errno is set appropriately.  In this
 *                case it is left unspecified whether the file position (if
 *                any) changes.
 *
 *********************************************************************/
int read_socket(jb_socket fd, char *buf, int len)
{
   if (len <= 0)
   {
      return(0);
   }

#if defined(_WIN32)
   return(recv(fd, buf, len, 0));
#elif defined(__BEOS__) || defined(AMIGA) || defined(__OS2__)
   return(recv(fd, buf, (size_t)len, 0));
#else
   return((int)read(fd, buf, (size_t)len));
#endif
}


/*********************************************************************
 *
 * Function    :  data_is_available
 *
 * Description :  Waits for data to arrive on a socket.
 *
 * Parameters  :
 *          1  :  fd = file descriptor of the socket to read
 *          2  :  seconds_to_wait = number of seconds after which we give up.
 *
 * Returns     :  TRUE if data arrived in time,
 *                FALSE otherwise.
 *
 *********************************************************************/
int data_is_available(jb_socket fd, int seconds_to_wait)
{
   char buf[10];
   fd_set rfds;
   struct timeval timeout;
   int n;

   memset(&timeout, 0, sizeof(timeout));
   timeout.tv_sec = seconds_to_wait;

#ifdef __OS2__
   /* Copy and pasted from jcc.c ... */
   memset(&rfds, 0, sizeof(fd_set));
#else
   FD_ZERO(&rfds);
#endif
   FD_SET(fd, &rfds);

   n = select(fd+1, &rfds, NULL, NULL, &timeout);

   /*
    * XXX: Do we care about the different error conditions?
    */
   return ((n == 1) && (1 == recv(fd, buf, 1, MSG_PEEK)));
}


/*********************************************************************
 *
 * Function    :  close_socket
 *
 * Description :  Closes a TCP/IP socket
 *
 * Parameters  :
 *          1  :  fd = file descriptor of socket to be closed
 *
 * Returns     :  void
 *
 *********************************************************************/
void close_socket(jb_socket fd)
{
#if defined(_WIN32) || defined(__BEOS__)
   closesocket(fd);
#elif defined(AMIGA)
   CloseSocket(fd); 
#elif defined(__OS2__)
   soclose(fd);
#else
   close(fd);
#endif

}


/*********************************************************************
 *
 * Function    :  bind_port
 *
 * Description :  Call socket, set socket options, and listen.
 *                Called by listen_loop to "boot up" our proxy address.
 *
 * Parameters  :
 *          1  :  hostnam = TCP/IP address to bind/listen to
 *          2  :  portnum = port to listen on
 *          3  :  pfd = pointer used to return file descriptor.
 *
 * Returns     :  if success, returns 0 and sets *pfd.
 *                if failure, returns -3 if address is in use,
 *                                    -2 if address unresolvable,
 *                                    -1 otherwise
 *********************************************************************/
int bind_port(const char *hostnam, int portnum, jb_socket *pfd)
{
#ifdef HAVE_RFC2553
   struct addrinfo hints;
   struct addrinfo *result, *rp;
   /*
    * XXX: portnum should be a string to allow symbolic service
    * names in the configuration file and to avoid the following
    * int2string.
    */
   char servnam[6];
   int retval;
#else
   struct sockaddr_in inaddr;
#endif /* def HAVE_RFC2553 */
   jb_socket fd;
#ifndef _WIN32
   int one = 1;
#endif /* ndef _WIN32 */

   *pfd = JB_INVALID_SOCKET;

#ifdef HAVE_RFC2553
   retval = snprintf(servnam, sizeof(servnam), "%d", portnum);
   if ((-1 == retval) || (sizeof(servnam) <= retval))
   {
      log_error(LOG_LEVEL_ERROR,
         "Port number (%d) ASCII decimal representation doesn't fit into 6 bytes",
         portnum);
      return -1;
   }

   memset(&hints, 0, sizeof(struct addrinfo));
   if ((hostnam == NULL) || !strcmpic(hostnam, "localhost"))
   {
      /*
       * XXX: This is a hack. The right thing to do
       * would be to bind to both AF_INET and AF_INET6.
       * This will also fail if there is no AF_INET
       * version available.
       */
      hints.ai_family = AF_INET;
   }
   else
   {
      hints.ai_family = AF_UNSPEC;
   }
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
   hints.ai_flags |= AI_ADDRCONFIG;
#endif
   hints.ai_protocol = 0; /* Realy any stream protocol or TCP only */
   hints.ai_canonname = NULL;
   hints.ai_addr = NULL;
   hints.ai_next = NULL;

   if ((retval = getaddrinfo(hostnam, servnam, &hints, &result)))
   {
      log_error(LOG_LEVEL_ERROR,
         "Can not resolve %s: %s", hostnam, gai_strerror(retval));
      return -2;
   }
#else
   memset((char *)&inaddr, '\0', sizeof inaddr);

   inaddr.sin_family      = AF_INET;
   inaddr.sin_addr.s_addr = resolve_hostname_to_ip(hostnam);

   if (inaddr.sin_addr.s_addr == INADDR_NONE)
   {
      return(-2);
   }

#ifndef _WIN32
   if (sizeof(inaddr.sin_port) == sizeof(short))
#endif /* ndef _WIN32 */
   {
      inaddr.sin_port = htons((unsigned short) portnum);
   }
#ifndef _WIN32
   else
   {
      inaddr.sin_port = htonl((unsigned long) portnum);
   }
#endif /* ndef _WIN32 */
#endif /* def HAVE_RFC2553 */

#ifdef HAVE_RFC2553
   for (rp = result; rp != NULL; rp = rp->ai_next)
   {
      fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
#else
   fd = socket(AF_INET, SOCK_STREAM, 0);
#endif /* def HAVE_RFC2553 */

#ifdef _WIN32
   if (fd == JB_INVALID_SOCKET)
#else
   if (fd < 0)
#endif
   {
#ifdef HAVE_RFC2553
      continue;
#else
      return(-1);
#endif
   }

#ifndef _WIN32
   /*
    * This is not needed for Win32 - in fact, it stops
    * duplicate instances of Privoxy from being caught.
    *
    * On UNIX, we assume the user is sensible enough not
    * to start Privoxy multiple times on the same IP.
    * Without this, stopping and restarting Privoxy
    * from a script fails.
    * Note: SO_REUSEADDR is meant to only take over
    * sockets which are *not* in listen state in Linux,
    * e.g. sockets in TIME_WAIT. YMMV.
    */
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
#endif /* ndef _WIN32 */

#ifdef HAVE_RFC2553
   if (bind(fd, rp->ai_addr, rp->ai_addrlen) < 0)
#else
   if (bind(fd, (struct sockaddr *)&inaddr, sizeof(inaddr)) < 0)
#endif
   {
#ifdef _WIN32
      errno = WSAGetLastError();
      if (errno == WSAEADDRINUSE)
#else
      if (errno == EADDRINUSE)
#endif
      {
#ifdef HAVE_RFC2553
         freeaddrinfo(result);
#endif
         close_socket(fd);
         return(-3);
      }
      else
      {
         close_socket(fd);
#ifndef HAVE_RFC2553
         return(-1);
      }
   }
#else
      }
   }
   else
   {
      /* bind() succeeded, escape from for-loop */
      /*
       * XXX: Support multiple listening sockets (e.g. localhost
       * resolves to AF_INET and AF_INET6, but only the first address
       * is used
       */
      break;
   }
   }

   freeaddrinfo(result);
   if (rp == NULL)
   {
      /* All bind()s failed */
      return(-1);
   }
#endif /* ndef HAVE_RFC2553 */

   while (listen(fd, MAX_LISTEN_BACKLOG) == -1)
   {
      if (errno != EINTR)
      {
         return(-1);
      }
   }

   *pfd = fd;
   return 0;

}


/*********************************************************************
 *
 * Function    :  get_host_information
 *
 * Description :  Determines the IP address the client used to
 *                reach us and the hostname associated with it.
 *
 *                XXX: Most of the code has been copy and pasted
 *                from accept_connection() and not all of the
 *                ifdefs paths have been tested afterwards.
 *
 * Parameters  :
 *          1  :  afd = File descriptor returned from accept().
 *          2  :  ip_address = Pointer to return the pointer to
 *                             the ip address string.
 *          3  :  hostname =   Pointer to return the pointer to
 *                             the hostname or NULL if the caller
 *                             isn't interested in it.
 *
 * Returns     :  void.
 *
 *********************************************************************/
void get_host_information(jb_socket afd, char **ip_address, char **hostname)
{
#ifdef HAVE_RFC2553
   struct sockaddr_storage server;
   int retval;
#else
   struct sockaddr_in server;
   struct hostent *host = NULL;
#endif /* HAVE_RFC2553 */
#if defined(_WIN32) || defined(__OS2__) || defined(__APPLE_CC__) || defined(AMIGA)
   /* according to accept_connection() this fixes a warning. */
   int s_length, s_length_provided;
#else
   socklen_t s_length, s_length_provided;
#endif
#ifndef HAVE_RFC2553
#if defined(HAVE_GETHOSTBYADDR_R_8_ARGS) ||  defined(HAVE_GETHOSTBYADDR_R_7_ARGS) || defined(HAVE_GETHOSTBYADDR_R_5_ARGS)
   struct hostent result;
#if defined(HAVE_GETHOSTBYADDR_R_5_ARGS)
   struct hostent_data hdata;
#else
   char hbuf[HOSTENT_BUFFER_SIZE];
   int thd_err;
#endif /* def HAVE_GETHOSTBYADDR_R_5_ARGS */
#endif /* def HAVE_GETHOSTBYADDR_R_(8|7|5)_ARGS */
#endif /* ifndef HAVE_RFC2553 */
   s_length = s_length_provided = sizeof(server);

   if (NULL != hostname)
   {
      *hostname = NULL;
   }
   *ip_address = NULL;

   if (!getsockname(afd, (struct sockaddr *) &server, &s_length))
   {
      if (s_length > s_length_provided)
      {
         log_error(LOG_LEVEL_ERROR, "getsockname() truncated server address");
         return;
      }
#ifdef HAVE_RFC2553
      *ip_address = malloc(NI_MAXHOST);
      if (NULL == *ip_address)
      {
         log_error(LOG_LEVEL_ERROR,
            "Out of memory while getting the client's IP address.");
         return;
      }
      retval = getnameinfo((struct sockaddr *) &server, s_length,
         *ip_address, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
      if (retval)
      {
         log_error(LOG_LEVEL_ERROR,
            "Unable to print my own IP address: %s", gai_strerror(retval));
         freez(*ip_address);
         return;
      }
#else
      *ip_address = strdup(inet_ntoa(server.sin_addr));
#endif /* HAVE_RFC2553 */
      if (NULL == hostname)
      {
         /*
          * We're done here, the caller isn't
          * interested in knowing the hostname.
          */
         return;
      }

#ifdef HAVE_RFC2553
      *hostname = malloc(NI_MAXHOST);
      if (NULL == *hostname)
      {
         log_error(LOG_LEVEL_ERROR,
            "Out of memory while getting the client's hostname.");
         return;
      }
      retval = getnameinfo((struct sockaddr *) &server, s_length,
         *hostname, NI_MAXHOST, NULL, 0, NI_NAMEREQD);
      if (retval)
      {
         log_error(LOG_LEVEL_ERROR,
            "Unable to resolve my own IP address: %s", gai_strerror(retval));
         freez(*hostname);
      }
#else
#if defined(HAVE_GETHOSTBYADDR_R_8_ARGS)
      gethostbyaddr_r((const char *)&server.sin_addr,
                      sizeof(server.sin_addr), AF_INET,
                      &result, hbuf, HOSTENT_BUFFER_SIZE,
                      &host, &thd_err);
#elif defined(HAVE_GETHOSTBYADDR_R_7_ARGS)
      host = gethostbyaddr_r((const char *)&server.sin_addr,
                      sizeof(server.sin_addr), AF_INET,
                      &result, hbuf, HOSTENT_BUFFER_SIZE, &thd_err);
#elif defined(HAVE_GETHOSTBYADDR_R_5_ARGS)
      if (0 == gethostbyaddr_r((const char *)&server.sin_addr,
                               sizeof(server.sin_addr), AF_INET,
                               &result, &hdata))
      {
         host = &result;
      }
      else
      {
         host = NULL;
      }
#elif defined(MUTEX_LOCKS_AVAILABLE)
      privoxy_mutex_lock(&resolver_mutex);
      host = gethostbyaddr((const char *)&server.sin_addr, 
                           sizeof(server.sin_addr), AF_INET);
      privoxy_mutex_unlock(&resolver_mutex);
#else
      host = gethostbyaddr((const char *)&server.sin_addr, 
                           sizeof(server.sin_addr), AF_INET);
#endif
      if (host == NULL)
      {
         log_error(LOG_LEVEL_ERROR, "Unable to get my own hostname: %E\n");
      }
      else
      {
         *hostname = strdup(host->h_name);
      }
#endif /* else def HAVE_RFC2553 */
   }

   return;
}


/*********************************************************************
 *
 * Function    :  accept_connection
 *
 * Description :  Accepts a connection on a socket.  Socket must have
 *                been created using bind_port().
 *
 * Parameters  :
 *          1  :  csp = Client state, cfd, ip_addr_str, and 
 *                ip_addr_long will be set by this routine.
 *          2  :  fd  = file descriptor returned from bind_port
 *
 * Returns     :  when a connection is accepted, it returns 1 (TRUE).
 *                On an error it returns 0 (FALSE).
 *
 *********************************************************************/
int accept_connection(struct client_state * csp, jb_socket fd)
{
#ifdef HAVE_RFC2553
   /* XXX: client is stored directly into csp->tcp_addr */
#define client (csp->tcp_addr)
   int retval;
#else
   struct sockaddr_in client;
#endif
   jb_socket afd;
#if defined(_WIN32) || defined(__OS2__) || defined(__APPLE_CC__) || defined(AMIGA)
   /* Wierdness - fix a warning. */
   int c_length;
#else
   socklen_t c_length;
#endif

   c_length = sizeof(client);

#ifdef _WIN32
   afd = accept (fd, (struct sockaddr *) &client, &c_length);
   if (afd == JB_INVALID_SOCKET)
   {
      return 0;
   }
#else
   do
   {
      afd = accept (fd, (struct sockaddr *) &client, &c_length);
   } while (afd < 1 && errno == EINTR);
   if (afd < 0)
   {
      return 0;
   }
#endif

   csp->cfd = afd;
#ifdef HAVE_RFC2553
   csp->ip_addr_str = malloc(NI_MAXHOST);
   if (NULL == csp->ip_addr_str)
   {
      log_error(LOG_LEVEL_ERROR,
         "Out of memory while getting the client's IP address.");
      return 0;
   }
   retval = getnameinfo((struct sockaddr *) &client, c_length,
         csp->ip_addr_str, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
   if (!csp->ip_addr_str || retval)
   {
      log_error(LOG_LEVEL_ERROR, "Can not save csp->ip_addr_str: %s",
            (csp->ip_addr_str) ? gai_strerror(retval) : "Insuffcient memory");
      freez(csp->ip_addr_str);
   }
#undef client
#else
   csp->ip_addr_str  = strdup(inet_ntoa(client.sin_addr));
   csp->ip_addr_long = ntohl(client.sin_addr.s_addr);
#endif /* def HAVE_RFC2553 */

   return 1;

}


/*********************************************************************
 *
 * Function    :  resolve_hostname_to_ip
 *
 * Description :  Resolve a hostname to an internet tcp/ip address.
 *                NULL or an empty string resolve to INADDR_ANY.
 *
 * Parameters  :
 *          1  :  host = hostname to resolve
 *
 * Returns     :  INADDR_NONE => failure, INADDR_ANY or tcp/ip address if succesful.
 *
 *********************************************************************/
unsigned long resolve_hostname_to_ip(const char *host)
{
   struct sockaddr_in inaddr;
   struct hostent *hostp;
   unsigned int dns_retries = 0;
#if defined(HAVE_GETHOSTBYNAME_R_6_ARGS) || defined(HAVE_GETHOSTBYNAME_R_5_ARGS) || defined(HAVE_GETHOSTBYNAME_R_3_ARGS)
   struct hostent result;
#if defined(HAVE_GETHOSTBYNAME_R_6_ARGS) || defined(HAVE_GETHOSTBYNAME_R_5_ARGS)
   char hbuf[HOSTENT_BUFFER_SIZE];
   int thd_err;
#else /* defined(HAVE_GETHOSTBYNAME_R_3_ARGS) */
   struct hostent_data hdata;
#endif /* def HAVE_GETHOSTBYNAME_R_(6|5)_ARGS */
#endif /* def HAVE_GETHOSTBYNAME_R_(6|5|3)_ARGS */

   if ((host == NULL) || (*host == '\0'))
   {
      return(INADDR_ANY);
   }

   memset((char *) &inaddr, 0, sizeof inaddr);

   if ((inaddr.sin_addr.s_addr = inet_addr(host)) == -1)
   {
#if defined(HAVE_GETHOSTBYNAME_R_6_ARGS)
      while (gethostbyname_r(host, &result, hbuf,
                HOSTENT_BUFFER_SIZE, &hostp, &thd_err)
             && (thd_err == TRY_AGAIN) && (dns_retries++ < MAX_DNS_RETRIES))
      {   
         log_error(LOG_LEVEL_ERROR,
            "Timeout #%u while trying to resolve %s. Trying again.",
            dns_retries, host);
      }
#elif defined(HAVE_GETHOSTBYNAME_R_5_ARGS)
      while (NULL == (hostp = gethostbyname_r(host, &result,
                                 hbuf, HOSTENT_BUFFER_SIZE, &thd_err))
             && (thd_err == TRY_AGAIN) && (dns_retries++ < MAX_DNS_RETRIES))
      {   
         log_error(LOG_LEVEL_ERROR,
            "Timeout #%u while trying to resolve %s. Trying again.",
            dns_retries, host);
      }
#elif defined(HAVE_GETHOSTBYNAME_R_3_ARGS)
      /*
       * XXX: Doesn't retry in case of soft errors.
       * Does this gethostbyname_r version set h_errno?
       */
      if (0 == gethostbyname_r(host, &result, &hdata))
      {
         hostp = &result;
      }
      else
      {
         hostp = NULL;
      }
#elif defined(MUTEX_LOCKS_AVAILABLE)
      privoxy_mutex_lock(&resolver_mutex);
      while (NULL == (hostp = gethostbyname(host))
             && (h_errno == TRY_AGAIN) && (dns_retries++ < MAX_DNS_RETRIES))
      {   
         log_error(LOG_LEVEL_ERROR,
            "Timeout #%u while trying to resolve %s. Trying again.",
            dns_retries, host);
      }
      privoxy_mutex_unlock(&resolver_mutex);
#else
      while (NULL == (hostp = gethostbyname(host))
             && (h_errno == TRY_AGAIN) && (dns_retries++ < MAX_DNS_RETRIES))
      {
         log_error(LOG_LEVEL_ERROR,
            "Timeout #%u while trying to resolve %s. Trying again.",
            dns_retries, host);
      }
#endif /* def HAVE_GETHOSTBYNAME_R_(6|5|3)_ARGS */
      /*
       * On Mac OSX, if a domain exists but doesn't have a type A
       * record associated with it, the h_addr member of the struct
       * hostent returned by gethostbyname is NULL, even if h_length
       * is 4. Therefore the second test below.
       */
      if (hostp == NULL || hostp->h_addr == NULL)
      {
         errno = EINVAL;
         log_error(LOG_LEVEL_ERROR, "could not resolve hostname %s", host);
         return(INADDR_NONE);
      }
      if (hostp->h_addrtype != AF_INET)
      {
#ifdef _WIN32
         errno = WSAEPROTOTYPE;
#else
         errno = EPROTOTYPE;
#endif 
         log_error(LOG_LEVEL_ERROR, "hostname %s resolves to unknown address type.", host);
         return(INADDR_NONE);
      }
      memcpy(
         (char *) &inaddr.sin_addr,
         (char *) hostp->h_addr,
         sizeof(inaddr.sin_addr)
      );
   }
   return(inaddr.sin_addr.s_addr);

}


#ifdef FEATURE_CONNECTION_KEEP_ALIVE
/*********************************************************************
 *
 * Function    :  socket_is_still_usable
 *
 * Description :  Decides whether or not an open socket is still usable.
 *
 * Parameters  :
 *          1  :  sfd = The socket to check.
 *
 * Returns     :  TRUE for yes, otherwise FALSE.
 *
 *********************************************************************/
int socket_is_still_usable(jb_socket sfd)
{
   char buf[10];
   int no_data_waiting;

#ifdef HAVE_POLL
   int poll_result;
   struct pollfd poll_fd[1];

   memset(poll_fd, 0, sizeof(poll_fd));
   poll_fd[0].fd = sfd;
   poll_fd[0].events = POLLIN;

   poll_result = poll(poll_fd, 1, 0);

   if (-1 == poll_result)
   {
      log_error(LOG_LEVEL_CONNECT, "Polling socket %d failed.", sfd);
      return FALSE;
   }
   no_data_waiting = !(poll_fd[0].revents & POLLIN);
#else
   fd_set readable_fds;
   struct timeval timeout;
   int ret;

   memset(&timeout, '\0', sizeof(timeout));
   FD_ZERO(&readable_fds);
   FD_SET(sfd, &readable_fds);

   ret = select((int)sfd+1, &readable_fds, NULL, NULL, &timeout);
   if (ret < 0)
   {
      log_error(LOG_LEVEL_CONNECT, "select() on socket %d failed: %E", sfd);
      return FALSE;
   }
   no_data_waiting = !FD_ISSET(sfd, &readable_fds);
#endif /* def HAVE_POLL */

   return (no_data_waiting || (1 == recv(sfd, buf, 1, MSG_PEEK)));
}
#endif /* def FEATURE_CONNECTION_KEEP_ALIVE */


/*
  Local Variables:
  tab-width: 3
  end:
*/