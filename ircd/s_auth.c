/************************************************************************
 *   IRC - Internet Relay Chat, ircd/s_auth.c
 *   Copyright (C) 1992 Darren Reed
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef lint
static  char rcsid[] = "@(#)$Id: s_auth.c,v 1.7 1998/08/04 15:28:25 kalt Exp $";
#endif

#include "os.h"
#include "s_defines.h"
#define S_AUTH_C
#include "s_externs.h"
#undef S_AUTH_C

#if defined(USE_IAUTH)
/*
 * sendto_iauth
 *
 *	Send the buffer to the authentication slave process.
 *	Return 0 if everything went well, -1 otherwise.
 */
int
sendto_iauth(buf)
char *buf;
{
    if (adfd < 0)
	    return -1;
    if (write(adfd, buf, strlen(buf)) != strlen(buf))
	{
	    sendto_flag(SCH_AUTH, "Aiiie! lost slave authentication process");
	    close(adfd);
	    adfd = -1;
	    /*
	    ** this should not happen.. but if it does.. shall we try to
	    ** restart the thing ? (afterall, iauth is almost stateless)
	    ** (and can now be restarted.. but is it wise?)
	    */
	    return -1;
	}
    return 0;
}

/*
 * read_iauth
 *
 *	read and process data from the authentication slave process.
 */
void
read_iauth()
{
    static char obuf[READBUF_SIZE];
    static int olen = 0;
    char buf[READBUF_SIZE], *start, *end, tbuf[BUFSIZ];
    aClient *cptr;
    int i;

    while (1)
	{
	    if (olen)
		    bcopy(obuf, buf, olen);
	    if ((i = recv(adfd, buf+olen, READBUF_SIZE-olen, 0)) <= 0)
		{
		    if (errno != EAGAIN && errno != EWOULDBLOCK)
			{
			    sendto_flag(SCH_AUTH, "Aiiie! lost slave authentication process (errno = %d)", errno);
			    close(adfd);
			    adfd = -1;
			}
		    break;
		}
	    olen += i;
	    buf[olen] = '\0';
	    start = buf;
	    while (end = index(start, '\n'))
		{
		    *end++ = '\0';
		    if (*buf == '>')
			    sendto_flag(SCH_AUTH, "%s", start+1);
		    else if (*buf != 'U' && *buf != 'u' &&
			     *buf != 'K' && *buf != 'D')
			{
			    sendto_flag(SCH_AUTH, "Garbage from iauth [%.*]",
					start);
			    start = end;
			    continue;
			}
		    if ((cptr = local[i = atoi(buf+2)]) == NULL)
			{
			    sendto_flag(SCH_DEBUG, "client gone");
			    start = end;
			    continue;
			}
		    sprintf(tbuf, "%c %d %s %u ", buf[0], i,
			    inetntoa((char *)&cptr->ip), cptr->port);
		    if (strncmp(tbuf, buf, strlen(tbuf)))
			{
			    sendto_flag(SCH_DEBUG, "mismatch");
			    start = end;
			    continue;
			}
		    if (buf[0] == 'U')
			{
			    if (cptr->auth != cptr->username)
				{   
				    istat.is_authmem -= sizeof(cptr->auth);
				    istat.is_auth -= 1;
				    MyFree(cptr->auth);
				}
			    cptr->auth =cptr->username;
			    strncpy(cptr->username, start+strlen(tbuf),
				    USERLEN+1);
			    cptr->username[USERLEN] = '\0';
			    cptr->flags |= FLAGS_GOTID;
			}
		    else if (buf[0] == 'u')
			{
			    if (cptr->auth != cptr->username)
				{
				    istat.is_authmem -= sizeof(cptr->auth);
				    istat.is_auth -= 1;
				    MyFree(cptr->auth);
				}
			    cptr->username[0] = '-';
			    strncpy(cptr->username+1, start+strlen(tbuf),
				    USERLEN);
			    cptr->username[USERLEN] = '\0';
			    cptr->auth = MyMalloc(strlen(start+strlen(tbuf))
						  + 2);
			    *cptr->auth = '-';
			    strcpy(cptr->auth+1, start+strlen(tbuf));
			    istat.is_authmem += sizeof(cptr->auth);
			    istat.is_auth += 1;
			    cptr->flags |= FLAGS_GOTID;
			}
		    else if (buf[0] == 'D')
			    /*authentication finished*/
			    ClearXAuth(cptr);
		    else
			{
			    /*
			    ** mark for kill, because it cannot be killed
			    ** yet: we don't even know if this is a server
			    ** or a user connection!
			    */
			    cptr->exitc = EXITC_AREF;
			}
		    start = end;
		}
	    if (start != buf+olen)
		    bcopy(start, obuf, olen = (buf+olen)-start);
	    else
		    olen = 0;
	}
    if (olen)
	    bcopy(buf, obuf, olen); /* for next time */		    
}
#endif

/*
 * start_auth
 *
 * Flag the client to show that an attempt to contact the ident server on
 * the client's host.  The connect and subsequently the socket are all put
 * into 'non-blocking' mode.  Should the connect or any later phase of the
 * identifing process fail, it is aborted and the user is given a username
 * of "unknown".
 */
void	start_auth(cptr)
Reg	aClient	*cptr;
{
#ifndef	NO_IDENT
	struct	sockaddr_in	us, them;
	SOCK_LEN_TYPE ulen, tlen;

	Debug((DEBUG_NOTICE,"start_auth(%x) fd %d status %d",
		cptr, cptr->fd, cptr->status));
	if ((cptr->authfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	    {
#ifdef	USE_SYSLOG
		syslog(LOG_ERR, "Unable to create auth socket for %s:%m",
			get_client_name(cptr,TRUE));
#endif
		Debug((DEBUG_ERROR, "Unable to create auth socket for %s:%s",
			get_client_name(cptr, TRUE),
			strerror(get_sockerr(cptr))));
		if (!DoingDNS(cptr))
			SetAccess(cptr);
		ircstp->is_abad++;
		return;
	    }
	if (cptr->authfd >= (MAXCONNECTIONS - 2))
	    {
		sendto_flag(SCH_ERROR, "Can't allocate fd for auth on %s",
			   get_client_name(cptr, TRUE));
		(void)close(cptr->authfd);
		return;
	    }

	set_non_blocking(cptr->authfd, cptr);

	/* get remote host peer - so that we get right interface -- jrg */
	tlen = ulen = sizeof(us);
	(void)getpeername(cptr->fd, (struct sockaddr *)&them, &tlen);
	them.sin_family = AF_INET;

	/* We must bind the local end to the interface that they connected
	   to: The local system might have more than one network address,
	   and RFC931 check only sends port numbers: server takes IP addresses
	   from query socket -- jrg */
	(void)getsockname(cptr->fd, (struct sockaddr *)&us, &ulen);
	us.sin_family = AF_INET;
#if defined(USE_IAUTH)
	if (adfd >= 0)
	    {
		char abuf[BUFSIZ];

		sprintf(abuf, "%d C %s %u ", cptr->fd,
			inetntoa((char *)&them.sin_addr),ntohs(them.sin_port));
		sprintf(abuf+strlen(abuf), "%s %u\n",
			inetntoa((char *)&us.sin_addr), ntohs(us.sin_port));
		if (sendto_iauth(abuf) == 0)
		    {
			close(cptr->authfd);
			cptr->authfd = -1;
			cptr->flags |= FLAGS_XAUTH;
			return;
		    }
	    }
#endif
	them.sin_port = htons(113);
	us.sin_port = htons(0);  /* bind assigns us a port */
	Debug((DEBUG_NOTICE,"auth(%x) from %s",
	       cptr, inetntoa((char *)&us.sin_addr)));
	if (bind(cptr->authfd, (struct sockaddr *)&us, ulen) >= 0)
	    {
		(void)getsockname(cptr->fd, (struct sockaddr *)&us, &ulen);
		Debug((DEBUG_NOTICE,"auth(%x) to %s",
			cptr, inetntoa((char *)&them.sin_addr)));
		(void)alarm((unsigned)4);
		if (connect(cptr->authfd, (struct sockaddr *)&them,
			    tlen) == -1 && errno != EINPROGRESS)
		    {
			Debug((DEBUG_ERROR,
				"auth(%x) connect failed to %s - %d", cptr,
				inetntoa((char *)&them.sin_addr), errno));
			ircstp->is_abad++;
			/*
			 * No error report from this...
			 */
			(void)alarm((unsigned)0);
			(void)close(cptr->authfd);
			cptr->authfd = -1;
			if (!DoingDNS(cptr))
				SetAccess(cptr);
			return;
		    }
		(void)alarm((unsigned)0);
	    }
	else
	    {
		report_error("binding stream socket for auth request %s:%s",
			     cptr);
		Debug((DEBUG_ERROR,"auth(%x) bind failed on %s port %d - %d",
		      cptr, inetntoa((char *)&us.sin_addr),
		      ntohs(us.sin_port), errno));
	    }

	cptr->flags |= (FLAGS_WRAUTH|FLAGS_AUTH);
	if (cptr->authfd > highest_fd)
		highest_fd = cptr->authfd;
#endif
	return;
}

/*
 * send_authports
 *
 * Send the ident server a query giving "theirport , ourport".
 * The write is only attempted *once* so it is deemed to be a fail if the
 * entire write doesn't write all the data given.  This shouldnt be a
 * problem since the socket should have a write buffer far greater than
 * this message to store it in should problems arise. -avalon
 */
void	send_authports(cptr)
aClient	*cptr;
{
	struct	sockaddr_in	us, them;
	char	authbuf[32];
	SOCK_LEN_TYPE ulen, tlen;

	Debug((DEBUG_NOTICE,"write_authports(%x) fd %d authfd %d stat %d",
		cptr, cptr->fd, cptr->authfd, cptr->status));
	tlen = ulen = sizeof(us);
	if (getsockname(cptr->fd, (struct sockaddr *)&us, &ulen) ||
	    getpeername(cptr->fd, (struct sockaddr *)&them, &tlen))
	    {
#ifdef	USE_SYSLOG
		syslog(LOG_ERR, "auth get{sock,peer}name error for %s:%m",
			get_client_name(cptr, TRUE));
#endif
		goto authsenderr;
	    }

	SPRINTF(authbuf, "%u , %u\r\n",
		(unsigned int)ntohs(them.sin_port),
		(unsigned int)ntohs(us.sin_port));

	Debug((DEBUG_SEND, "sending [%s] to auth port %s.113",
		authbuf, inetntoa((char *)&them.sin_addr)));
	if (write(cptr->authfd, authbuf, strlen(authbuf)) != strlen(authbuf))
	    {
authsenderr:
		ircstp->is_abad++;
		(void)close(cptr->authfd);
		if (cptr->authfd == highest_fd)
			while (!local[highest_fd])
				highest_fd--;
		cptr->authfd = -1;
		cptr->flags &= ~(FLAGS_AUTH|FLAGS_WRAUTH);
		if (!DoingDNS(cptr))
			SetAccess(cptr);
		return;
	    }
	cptr->flags &= ~FLAGS_WRAUTH;
	return;
}

/*
 * read_authports
 *
 * read the reply (if any) from the ident server we connected to.
 * The actual read processijng here is pretty weak - no handling of the reply
 * if it is fragmented by IP.
 */
void	read_authports(cptr)
Reg	aClient	*cptr;
{
	Reg	char	*s, *t;
	Reg	int	len;
	char	ruser[513], system[8];
	u_short	remp = 0, locp = 0;

	*system = *ruser = '\0';
	Debug((DEBUG_NOTICE,"read_authports(%x) fd %d authfd %d stat %d",
		cptr, cptr->fd, cptr->authfd, cptr->status));
	/*
	 * Nasty.  Cant allow any other reads from client fd while we're
	 * waiting on the authfd to return a full valid string.  Use the
	 * client's input buffer to buffer the authd reply.
	 * Oh. this is needed because an authd reply may come back in more
	 * than 1 read! -avalon
	 */
	if ((len = read(cptr->authfd, cptr->buffer + cptr->count,
			sizeof(cptr->buffer) - 1 - cptr->count)) >= 0)
	    {
		cptr->count += len;
		cptr->buffer[cptr->count] = '\0';
	    }

	if ((len > 0) && (cptr->count != (sizeof(cptr->buffer) - 1)) &&
	    (sscanf(cptr->buffer, "%hd , %hd : USERID : %*[^:]: %512s",
		    &remp, &locp, ruser) == 3))
	    {
		s = rindex(cptr->buffer, ':');
		*s++ = '\0';
		for (t = (rindex(cptr->buffer, ':') + 1); *t; t++)
			if (!isspace(*t))
				break;
		strncpyzt(system, t, sizeof(system));
		for (t = ruser; *s && (t < ruser + sizeof(ruser)); s++)
			if (!isspace(*s) && *s != ':' && *s != '@')
				*t++ = *s;
		*t = '\0';
		Debug((DEBUG_INFO,"auth reply ok [%s] [%s]", system, ruser));
	    }
	else if (len != 0)
	    {
		if (!index(cptr->buffer, '\n') && !index(cptr->buffer, '\r'))
			return;
		Debug((DEBUG_ERROR,"local %d remote %d s %x",
				locp, remp, ruser));
		Debug((DEBUG_ERROR,"bad auth reply in [%s]", cptr->buffer));
		*ruser = '\0';
	    }
	(void)close(cptr->authfd);
	if (cptr->authfd == highest_fd)
		while (!local[highest_fd])
			highest_fd--;
	cptr->count = 0;
	cptr->authfd = -1;
	ClearAuth(cptr);
	if (!DoingDNS(cptr))
		SetAccess(cptr);
	if (len > 0)
		Debug((DEBUG_INFO,"ident reply: [%s]", cptr->buffer));

	if (!locp || !remp || !*ruser)
	    {
		ircstp->is_abad++;
		return;
	    }
	ircstp->is_asuc++;
  	if (strncmp(system, "OTHER", 5))
 		strncpy(cptr->username, ruser, USERLEN+1);
 	else
	    { /* OTHER type of identifier */
 		*cptr->username = '-';	/* -> add '-' prefix into ident */
 		strncpy(&cptr->username[1], ruser, USERLEN);
		if ((unsigned)strlen(ruser) > USERLEN)
		    {
			if (cptr->auth != cptr->username)/*impossible, but...*/
			    {
				istat.is_authmem -= sizeof(cptr->auth);
				istat.is_auth -= 1;
				MyFree(cptr->auth);
			    }
			cptr->auth = MyMalloc(strlen(ruser) + 2);
			*cptr->auth = '-';
			strcpy(cptr->auth+1, ruser);
			istat.is_authmem += sizeof(cptr->auth);
			istat.is_auth += 1;
		    }
		else
			cptr->auth = cptr->username;
	    }
 	cptr->username[USERLEN] = '\0';
 	cptr->flags |= FLAGS_GOTID;
	Debug((DEBUG_INFO, "got username [%s]", ruser));
	return;
}
