/************************************************************************
 *   IRC - Internet Relay Chat, ircd/res.c
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

#include "os.h"
#include "s_defines.h"
#define RES_C
#include "s_externs.h"
#undef RES_C

#ifndef lint
static  char rcsid[] = "@(#)$Id: res.c,v 1.19 1999/02/19 15:12:35 kalt Exp $";
#endif

/* #undef	DEBUG	/* because there is a lot of debug code in here :-) */

static	char	hostbuf[HOSTLEN+1+100]; /* +100 for INET6 */
static	char	dot[] = ".";
static	int	incache = 0;
static	CacheTable	hashtable[ARES_CACSIZE];
static	aCache	*cachetop = NULL;
static	ResRQ	*last, *first;

static	void	rem_cache __P((aCache *));
static	void	rem_request __P((ResRQ *));
static	int	do_query_name __P((Link *, char *, ResRQ *));
static	int	do_query_number __P((Link *, struct IN_ADDR *, ResRQ *));
static	void	resend_query __P((ResRQ *));
static	int	proc_answer __P((ResRQ *, HEADER *, char *, char *));
static	int	query_name __P((char *, int, int, ResRQ *));
static	aCache	*make_cache __P((ResRQ *)), *rem_list __P((aCache *));
static	aCache	*find_cache_name __P((char *));
static	aCache	*find_cache_number __P((ResRQ *, char *));
static	int	add_request __P((ResRQ *));
static	ResRQ	*make_request __P((Link *));
static	int	send_res_msg __P((char *, int, int));
static	ResRQ	*find_id __P((int));
static	int	hash_number __P((unsigned char *));
static	void	update_list __P((ResRQ *, aCache *));
static	int	hash_name __P((char *));
static	int	bad_hostname __P((char *, int));

static	struct cacheinfo {
	int	ca_adds;
	int	ca_dels;
	int	ca_expires;
	int	ca_lookups;
	int	ca_na_hits;
	int	ca_nu_hits;
	int	ca_updates;
} cainfo;

static	struct	resinfo {
	int	re_errors;
	int	re_nu_look;
	int	re_na_look;
	int	re_replies;
	int	re_requests;
	int	re_resends;
	int	re_sent;
	int	re_timeouts;
	int	re_shortttl;
	int	re_unkrep;
} reinfo;

int	init_resolver(op)
int	op;
{
	int	ret = 0;

#ifdef	LRAND48
	srand48(time(NULL));
#endif
	if (op & RES_INITLIST)
	    {
		bzero((char *)&reinfo, sizeof(reinfo));
		first = last = NULL;
	    }
	if (op & RES_CALLINIT)
	    {
		ret = ircd_res_init();
		if (!ircd_res.nscount)
		    {
			ircd_res.nscount = 1;
#ifdef INET6
			/* still IPv4 */
			ircd_res.nsaddr_list[0].sin_addr.s_addr =
			  inet_pton(AF_INET, "127.0.0.1",
				    &ircd_res.nsaddr_list[0].sin_addr.s_addr);
#else
			ircd_res.nsaddr_list[0].sin_addr.s_addr =
				inetaddr("127.0.0.1");
#endif
		    }
	    }

	if (op & RES_INITSOCK)
	    {
		int	on = 0;

#ifdef INET6
		/* still IPv4 */
		ret = resfd = socket(AF_INET, SOCK_DGRAM, 0);
#else
		ret = resfd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
		(void) SETSOCKOPT(ret, SOL_SOCKET, SO_BROADCAST, &on, on);
	    }
#ifdef DEBUG
	if (op & RES_INITDEBG);
		ircd_res.options |= RES_DEBUG;
#endif
	if (op & RES_INITCACH)
	    {
		bzero((char *)&cainfo, sizeof(cainfo));
		bzero((char *)hashtable, sizeof(hashtable));
	    }
	if (op == 0)
		ret = resfd;
	return ret;
}

static	int	add_request(new)
ResRQ *new;
{
	if (!new)
		return -1;
	if (!first)
		first = last = new;
	else
	    {
		last->next = new;
		last = new;
	    }
	new->next = NULL;
	reinfo.re_requests++;
	return 0;
}

/*
 * remove a request from the list. This must also free any memory that has
 * been allocated for temporary storage of DNS results.
 */
static	void	rem_request(old)
ResRQ	*old;
{
	Reg	ResRQ	**rptr, *r2ptr = NULL;
	Reg	int	i;
	Reg	char	*s;

	if (!old)
		return;
	for (rptr = &first; *rptr; r2ptr = *rptr, rptr = &(*rptr)->next)
		if (*rptr == old)
		    {
			*rptr = old->next;
			if (last == old)
				last = r2ptr;
			break;
		    }
#ifdef	DEBUG
	Debug((DEBUG_INFO,"rem_request:Remove %#x at %#x %#x",
		old, *rptr, r2ptr));
#endif
	r2ptr = old;
	if (r2ptr->he.h_name)
		MyFree((char *)r2ptr->he.h_name);
	for (i = 0; i < MAXALIASES; i++)
		if ((s = r2ptr->he.h_aliases[i]))
			MyFree(s);
	if (r2ptr->name)
		MyFree(r2ptr->name);
	MyFree((char *)r2ptr);

	return;
}

/*
 * Create a DNS request record for the server.
 */
static	ResRQ	*make_request(lp)
Link	*lp;
{
	Reg	ResRQ	*nreq;

	nreq = (ResRQ *)MyMalloc(sizeof(ResRQ));
	bzero((char *)nreq, sizeof(ResRQ));
	nreq->next = NULL; /* where NULL is non-zero ;) */
	nreq->sentat = timeofday;
	nreq->retries = 3;
	nreq->resend = 1;
	nreq->srch = -1;
	if (lp)
		bcopy((char *)lp, (char *)&nreq->cinfo, sizeof(Link));
	else
		bzero((char *)&nreq->cinfo, sizeof(Link));
	nreq->timeout = 4;	/* start at 4 and exponential inc. */
	nreq->he.h_addrtype = AFINET;
	nreq->he.h_name = NULL;
	nreq->he.h_aliases[0] = NULL;
	(void)add_request(nreq);
	return nreq;
}

/*
 * Remove queries from the list which have been there too long without
 * being resolved.
 */
time_t	timeout_query_list(now)
time_t	now;
{
	Reg	ResRQ	*rptr, *r2ptr;
	Reg	time_t	next = 0, tout;
	aClient	*cptr;

	Debug((DEBUG_DNS,"timeout_query_list at %s",myctime(now)));
	for (rptr = first; rptr; rptr = r2ptr)
	    {
		r2ptr = rptr->next;
		tout = rptr->sentat + rptr->timeout;
		if (now >= tout)
			if (--rptr->retries <= 0)
			    {
#ifdef DEBUG
				Debug((DEBUG_ERROR,"timeout %x now %d cptr %x",
				       rptr, now, rptr->cinfo.value.cptr));
#endif
				reinfo.re_timeouts++;
				cptr = rptr->cinfo.value.cptr;
				switch (rptr->cinfo.flags)
				    {
				case ASYNC_CLIENT :
#if defined(USE_IAUTH)
					sendto_iauth("%d d", cptr->fd);
#endif
					ClearDNS(cptr);
					if (!DoingAuth(cptr))	/* XAuth? */
						SetAccess(cptr); /* unused */
					break;
				case ASYNC_CONNECT :
					sendto_flag(SCH_ERROR,
						    "Host %s unknown",
						    rptr->name);
					break;
				    }
				rem_request(rptr);
				continue;
			    }
			else
			    {
				rptr->sentat = now;
				rptr->timeout += rptr->timeout;
				resend_query(rptr);
				tout = now + rptr->timeout;
#ifdef DEBUG
				Debug((DEBUG_INFO,"r %x now %d retry %d c %x",
				       rptr, now, rptr->retries,
				       rptr->cinfo.value.cptr));
#endif
			    }
		if (!next || tout < next)
			next = tout;
	    }
	return (next > now) ? next : (now + AR_TTL);
}

/*
 * del_queries - called by the server to cleanup outstanding queries for
 * which there no longer exist clients or conf lines.
 */
void	del_queries(cp)
char	*cp;
{
	Reg	ResRQ	*rptr, *r2ptr;

	for (rptr = first; rptr; rptr = r2ptr)
	    {
		r2ptr = rptr->next;
		if (cp == rptr->cinfo.value.cp)
			rem_request(rptr);
	    }
}

/*
 * sends msg to all nameservers found in the "ircd_res" structure.
 * This should reflect /etc/resolv.conf. We will get responses
 * which arent needed but is easier than checking to see if nameserver
 * isnt present. Returns number of messages successfully sent to 
 * nameservers or -1 if no successful sends.
 */
static	int	send_res_msg(msg, len, rcount)
char	*msg;
int	len, rcount;
{
	Reg	int	i;
	int	sent = 0, max;

	if (!msg)
		return -1;

	max = MIN(ircd_res.nscount, rcount);
	if (ircd_res.options & RES_PRIMARY)
		max = 1;
	if (!max)
		max = 1;

	for (i = 0; i < max; i++)
	    {
#ifdef INET6
		/* still IPv4 */
		ircd_res.nsaddr_list[i].sin_family = AF_INET;
#else
		ircd_res.nsaddr_list[i].sin_family = AF_INET;
#endif
#ifdef INET6
		if (sendto(resfd, msg, len, 0,
			   (struct sockaddr *)&(ircd_res.nsaddr_list[i]),
			   sizeof(struct sockaddr)) == len)
#else
		if (sendto(resfd, msg, len, 0,
			   (struct sockaddr *)&(ircd_res.nsaddr_list[i]),
			   sizeof(struct sockaddr)) == len)
#endif

		    {
			reinfo.re_sent++;
			sent++;
		    }
		else
			Debug((DEBUG_ERROR,"s_r_m:sendto: %d on %d",
				errno, resfd));
	    }

	return (sent) ? sent : -1;
}


/*
 * find a dns request id (id is determined by dn_mkquery)
 */
static	ResRQ	*find_id(id)
int	id;
{
	Reg	ResRQ	*rptr;

	for (rptr = first; rptr; rptr = rptr->next)
		if (rptr->id == id)
			return rptr;
	return NULL;
}

struct	hostent	*gethost_byname(name, lp)
char	*name;
Link	*lp;
{
	Reg	aCache	*cp;

	reinfo.re_na_look++;
	if ((cp = find_cache_name(name)))
		return (struct hostent *)&(cp->he);
	if (!lp)
		return NULL;
	(void)do_query_name(lp, name, NULL);
	return NULL;
}

struct	hostent	*gethost_byaddr(addr, lp)
char	*addr;
Link	*lp;
{
	aCache	*cp;

	reinfo.re_nu_look++;
	if ((cp = find_cache_number(NULL, addr)))
		return (struct hostent *)&(cp->he);
	if (!lp)
		return NULL;
	(void)do_query_number(lp, (struct IN_ADDR *)addr, NULL);
	return NULL;
}

static	int	do_query_name(lp, name, rptr)
Link	*lp;
char	*name;
Reg	ResRQ	*rptr;
{
	char	hname[HOSTLEN+1];
	int	len;

	strncpyzt(hname, name, sizeof(hname));
	len = strlen(hname);

	if (rptr && !index(hname, '.') && ircd_res.options & RES_DEFNAMES)
	    {
		(void)strncat(hname, dot, sizeof(hname) - len - 1);
		len++;
		(void)strncat(hname, ircd_res.defdname, sizeof(hname) - len -1);
	    }

	/*
	 * Store the name passed as the one to lookup and generate other host
	 * names to pass onto the nameserver(s) for lookups.
	 */
	if (!rptr)
	    {
		rptr = make_request(lp);
#ifdef INET6
		rptr->type = T_AAAA;
#else
		rptr->type = T_A;
#endif
		rptr->name = (char *)MyMalloc(strlen(name) + 1);
		(void)strcpy(rptr->name, name);
	    }
	Debug((DEBUG_DNS,"do_query_name(): %s ", hname));
#ifdef INET6
	return (query_name(hname, C_IN, T_AAAA, rptr));
#else
	return (query_name(hname, C_IN, T_A, rptr));
#endif
}

/*
 * Use this to do reverse IP# lookups.
 */
static	int	do_query_number(lp, numb, rptr)
Link	*lp;
struct	IN_ADDR	*numb;
Reg	ResRQ	*rptr;
{
	char	ipbuf[128];
	Reg	u_char	*cp;

#ifdef INET6
	cp = (u_char *)numb->s6_addr;
	if (cp[0]==0 && cp[1]==0 && cp[2]==0 && cp[3]==0 && cp[4]==0 && 
	    cp[5]==0 && cp[6]==0 && cp[7]==0 && cp[8]==0 && cp[9]==0 && 
	    ((cp[10]==0 && cp[11]==0) || (cp[10]==0xff && cp[11]==0xff)))
	    {
		(void)sprintf(ipbuf, "%u.%u.%u.%u.in-addr.arpa.",
			      (u_int)(cp[15]), (u_int)(cp[14]),
			      (u_int)(cp[13]), (u_int)(cp[12]));
	    }
	else
	    {
		(void)sprintf(ipbuf, "%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.ip6.int.",
		(u_int)(cp[15]&0xf), (u_int)(cp[15]>>4),
		(u_int)(cp[14]&0xf), (u_int)(cp[14]>>4),
		(u_int)(cp[13]&0xf), (u_int)(cp[13]>>4),
		(u_int)(cp[12]&0xf), (u_int)(cp[12]>>4),
		(u_int)(cp[11]&0xf), (u_int)(cp[11]>>4),
		(u_int)(cp[10]&0xf), (u_int)(cp[10]>>4),
		(u_int)(cp[9]&0xf), (u_int)(cp[9]>>4),
		(u_int)(cp[8]&0xf), (u_int)(cp[8]>>4),
		(u_int)(cp[7]&0xf), (u_int)(cp[7]>>4),
		(u_int)(cp[6]&0xf), (u_int)(cp[6]>>4),
		(u_int)(cp[5]&0xf), (u_int)(cp[5]>>4),
		(u_int)(cp[4]&0xf), (u_int)(cp[4]>>4),
		(u_int)(cp[3]&0xf), (u_int)(cp[3]>>4),
		(u_int)(cp[2]&0xf), (u_int)(cp[2]>>4),
		(u_int)(cp[1]&0xf), (u_int)(cp[1]>>4),
		(u_int)(cp[0]&0xf), (u_int)(cp[0]>>4));
	    }
#else
	cp = (u_char *)&numb->s_addr;
	(void)sprintf(ipbuf, "%u.%u.%u.%u.in-addr.arpa.",
		      (u_int)(cp[3]), (u_int)(cp[2]),
		      (u_int)(cp[1]), (u_int)(cp[0]));
#endif

	if (!rptr)
	    {
		rptr = make_request(lp);
		rptr->type = T_PTR;
#ifdef INET6
		bcopy(numb->s6_addr, rptr->addr.s6_addr, IN6ADDRSZ);
		bcopy((char *)numb->s6_addr,
			(char *)&rptr->he.h_addr, sizeof(struct in6_addr));
#else
		rptr->addr.s_addr = numb->s_addr;
		bcopy((char *)&numb->s_addr,
			(char *)&rptr->he.h_addr, sizeof(struct in_addr));
#endif
		rptr->he.h_length = sizeof(struct IN_ADDR);
	    }
	return (query_name(ipbuf, C_IN, T_PTR, rptr));
}

/*
 * generate a query based on class, type and name.
 */
static	int	query_name(name, class, type, rptr)
char	*name;
int	class, type;
ResRQ	*rptr;
{
	struct	timeval	tv;
	char	buf[MAXPACKET];
	int	r,s,k = 0;
	HEADER	*hptr;

	bzero(buf, sizeof(buf));
	r = ircd_res_mkquery(QUERY, name, class, type, NULL, 0, NULL,
			(u_char *)buf, sizeof(buf));
	if (r <= 0)
	    {
		h_errno = NO_RECOVERY;
		return r;
	    }
	hptr = (HEADER *)buf;
#ifdef LRAND48
        do {
		hptr->id = htons(ntohs(hptr->id) + k + lrand48() & 0xffff);
#else
	(void) gettimeofday(&tv, NULL);
	do {
		/* htons/ntohs can be assembler macros, which cannot
		   be nested. Thus two lines.	-Vesa		    */
		u_short nstmp = ntohs(hptr->id) + k +
				(u_short)(tv.tv_usec & 0xffff);
		hptr->id = htons(nstmp);
#endif /* LRAND48 */
		k++;
	} while (find_id(ntohs(hptr->id)));
	rptr->id = ntohs(hptr->id);
	rptr->sends++;
	s = send_res_msg(buf, r, rptr->sends);
	if (s == -1)
	    {
		h_errno = TRY_AGAIN;
		return -1;
	    }
	else
		rptr->sent += s;
	return 0;
}

static	void	resend_query(rptr)
ResRQ	*rptr;
{
	if (rptr->resend == 0)
		return;
	reinfo.re_resends++;
	switch(rptr->type)
	{
	case T_PTR:
		(void)do_query_number(NULL, &rptr->addr, rptr);
		break;
#ifdef INET6
	case T_AAAA:
#endif
	case T_A:
		(void)do_query_name(NULL, rptr->name, rptr);
		break;
	default:
		break;
	}
	return;
}

/*
 * process name server reply.
 */
static	int	proc_answer(rptr, hptr, buf, eob)
ResRQ	*rptr;
char	*buf, *eob;
HEADER	*hptr;
{
	Reg	char	*cp, **alias;
	Reg	struct	hent	*hp;
	int	class, type, dlen, len, ans = 0, n;
	struct	IN_ADDR	dr, *adr;

	cp = buf + sizeof(HEADER);
	hp = (struct hent *)&(rptr->he);
	adr = &hp->h_addr;
#ifdef INET6
	while (adr->s6_laddr[0] | adr->s6_laddr[1] | adr->s6_laddr[2] |
	       adr->s6_laddr[3])
#else
	while (adr->s_addr)
#endif
		adr++;
	alias = hp->h_aliases;
	while (*alias)
		alias++;
#if SOLARIS_2 && !defined(__GNUC__) /* brain damaged compiler it seems */
	for (; hptr->qdcount > 0; hptr->qdcount--)
#else
	while (hptr->qdcount-- > 0)
#endif
		if ((n = __ircd_dn_skipname((u_char *)cp, (u_char *)eob)) == -1)
			break;
		else
			cp += (n + QFIXEDSZ);
	/*
	 * proccess each answer sent to us blech.
	 */
	while (hptr->ancount-- > 0 && cp && cp < eob) {
		n = ircd_dn_expand((u_char *)buf, (u_char *)eob, (u_char *)cp,
			      hostbuf, sizeof(hostbuf));
		if (n <= 0)
			break;

		cp += n;
		type = (int)ircd_getshort((u_char *)cp);
		cp += sizeof(short);
		class = (int)ircd_getshort((u_char *)cp);
		cp += sizeof(short);
		rptr->ttl = ircd_getlong((u_char *)cp);
		cp += sizeof(rptr->ttl);
		dlen =  (int)ircd_getshort((u_char *)cp);
		cp += sizeof(short);
		rptr->type = type;

		len = strlen(hostbuf);
		/* name server never returns with trailing '.' */
		if (!index(hostbuf,'.') && (ircd_res.options & RES_DEFNAMES))
		    {
			(void)strcat(hostbuf, dot);
			len++;
			(void)strncat(hostbuf, ircd_res.defdname,
				sizeof(hostbuf) - 1 - len);
			len = MIN(len + strlen(ircd_res.defdname),
				  sizeof(hostbuf) - 1);
		    }

		switch(type)
		{
#ifdef INET6
		case T_AAAA :
#endif
		case T_A :
#ifdef INET6
			if (dlen != ((type==T_AAAA) ? sizeof(dr) : 
				     sizeof(struct in_addr)))
#else
			if (dlen != sizeof(dr))
#endif
			    {
				sendto_flag(SCH_ERROR,
				    "Bad IP length (%d) returned for %s", dlen,
					    hostbuf);
				Debug((DEBUG_DNS,
				       "Bad IP length (%d) returned for %s",
				       dlen, hostbuf));
				return -2;
			    }
			hp->h_length = dlen;
			if (ans == 1)
				hp->h_addrtype =  (class == C_IN) ?
							 AFINET: AF_UNSPEC;
#ifdef INET6
			if (type == T_AAAA)
				bcopy(cp, (char *)&dr, dlen);
			else {
				dr.s6_laddr[0]=dr.s6_laddr[1]=0;
				dr.s6_laddr[2]=htonl(0xffff);
				bcopy(cp, &dr.s6_laddr[3], INADDRSZ);
			}
			bcopy(dr.s6_addr, adr->s6_addr, IN6ADDRSZ);
#else
			bcopy(cp, (char *)&dr, dlen);
			adr->s_addr = dr.s_addr;
#endif
#ifdef INET6
			Debug((DEBUG_INFO,"got ip # %s for %s",
			       inet_ntop(AF_INET6, (char *)adr, mydummy,
					 MYDUMMY_SIZE),
			       hostbuf));
#else
			Debug((DEBUG_INFO,"got ip # %s for %s",
			       inetntoa((char *)adr),
			       hostbuf));
#endif
			if (!hp->h_name)
			    {
				hp->h_name =(char *)MyMalloc(len+1);
				(void)strcpy(hp->h_name, hostbuf);
			    }
			ans++;
			adr++;
			cp += dlen;
 			break;
		case T_PTR :
			if((n = ircd_dn_expand((u_char *)buf, (u_char *)eob,
					  (u_char *)cp, hostbuf,
					  sizeof(hostbuf) )) < 0)
			    {
				cp = NULL;
				break;
			    }
			cp += n;
			len = strlen(hostbuf);
			Debug((DEBUG_INFO, "got host %s (%d vs %d)",
				hostbuf, len, strlen(hostbuf)));
			if (bad_hostname(hostbuf, len))
				return -1;
			/*
			 * copy the returned hostname into the host name
			 * or alias field if there is a known hostname
			 * already.
			 */
			if (hp->h_name)
			    {
				Debug((DEBUG_INFO, "duplicate PTR ignored"));
			    }
			else
			    {
				hp->h_name = (char *)MyMalloc(len + 1);
				(void)strcpy(hp->h_name, hostbuf);
			    }
			ans++;
			break;
		case T_CNAME :
			cp += dlen;
			Debug((DEBUG_INFO,"got cname %s",hostbuf));
			if (bad_hostname(hostbuf, len))
				return -1; /* a break would be enough here */
			if (alias >= &(hp->h_aliases[MAXALIASES-1]))
				break;
			*alias = (char *)MyMalloc(len + 1);
			(void)strcpy(*alias++, hostbuf);
			*alias = NULL;
			ans++;
			break;
		default :
#ifdef DEBUG
			Debug((DEBUG_INFO,"proc_answer: type:%d for:%s",
			      type,hostbuf));
#endif
			break;
		}
	}
	return ans;
}

/*
 * read a dns reply from the nameserver and process it.
 */
struct	hostent	*get_res(lp)
char	*lp;
{
	static	char	buf[sizeof(HEADER) + MAXPACKET];
	Reg	HEADER	*hptr;
	Reg	ResRQ	*rptr = NULL;
	aCache	*cp = NULL;
#ifdef INET6
	struct	sockaddr_in	sin;
#else
	struct	sockaddr_in	sin;
#endif
	int	rc, a, max;
	SOCK_LEN_TYPE len = sizeof(sin);

	(void)alarm((unsigned)4);
#ifdef INET6
	rc = recvfrom(resfd, buf, sizeof(buf), 0, (struct sockaddr *)&sin, &len);
#else
	rc = recvfrom(resfd, buf, sizeof(buf), 0, (struct sockaddr *)&sin, &len);
#endif

	(void)alarm((unsigned)0);
	if (rc <= sizeof(HEADER))
		goto getres_err;
	/*
	 * convert DNS reply reader from Network byte order to CPU byte order.
	 */
	hptr = (HEADER *)buf;
	hptr->id = ntohs(hptr->id);
	hptr->ancount = ntohs(hptr->ancount);
	hptr->qdcount = ntohs(hptr->qdcount);
	hptr->nscount = ntohs(hptr->nscount);
	hptr->arcount = ntohs(hptr->arcount);
#ifdef	DEBUG
	Debug((DEBUG_NOTICE, "get_res:id = %d rcode = %d ancount = %d",
		hptr->id, hptr->rcode, hptr->ancount));
#endif
	reinfo.re_replies++;
	/*
	 * response for an id which we have already received an answer for
	 * just ignore this response.
	 */
	rptr = find_id(hptr->id);
	if (!rptr)
		goto getres_err;
	/*
	 * check against possibly fake replies
	 */
	max = MIN(ircd_res.nscount, rptr->sends);
	if (!max)
		max = 1;

	for (a = 0; a < max; a++)
#ifdef INET6
		if (!ircd_res.nsaddr_list[a].sin_addr.s_addr ||
		    !bcmp((char *)&sin.sin_addr,
			  (char *)&ircd_res.nsaddr_list[a].sin_addr,
			  sizeof(struct in_addr)))
#else
		if (!ircd_res.nsaddr_list[a].sin_addr.s_addr ||
		    !bcmp((char *)&sin.sin_addr,
			  (char *)&ircd_res.nsaddr_list[a].sin_addr,
			  sizeof(struct in_addr)))
#endif
			break;
	if (a == max)
	    {
		reinfo.re_unkrep++;
		goto getres_err;
	    }

	if ((hptr->rcode != NOERROR) || (hptr->ancount == 0))
	    {
		switch (hptr->rcode)
		{
		case NXDOMAIN:
			h_errno = TRY_AGAIN;
			break;
		case SERVFAIL:
			h_errno = TRY_AGAIN;
			break;
		case NOERROR:
			h_errno = NO_DATA;
			break;
		case FORMERR:
		case NOTIMP:
		case REFUSED:
		default:
			h_errno = NO_RECOVERY;
			break;
		}
		reinfo.re_errors++;
		/*
		** If a bad error was returned, we stop here and dont send
		** send any more (no retries granted).
		*/
		if (h_errno != TRY_AGAIN)
		    {
			Debug((DEBUG_DNS, "Fatal DNS error %d for %d",
				h_errno, hptr->rcode));
			rptr->resend = 0;
			rptr->retries = 0;
		    }
		goto getres_err;
	    }
	a = proc_answer(rptr, hptr, buf, buf+rc);
	if (a == -1) {
		sendto_flag(SCH_ERROR, "Bad hostname returned from %s for %s",
#ifdef INET6
			    inetntop(AF_INET, &sin.sin_addr, mydummy2,
				      MYDUMMY_SIZE),
			    inetntop(AF_INET6, rptr->he.h_addr.s6_addr,
				      mydummy, MYDUMMY_SIZE));
#else
			    inetntoa((char *)&sin.sin_addr),
			    inetntoa((char *)&rptr->he.h_addr));
#endif
#ifdef INET6
		Debug((DEBUG_DNS, "Bad hostname returned from %s for %s",
		       inet_ntop(AF_INET, &sin.sin_addr,mydummy2,MYDUMMY_SIZE),
		       inet_ntop(AF_INET6, rptr->he.h_addr.s6_addr, mydummy,
				 MYDUMMY_SIZE)));
#else
		Debug((DEBUG_DNS, "Bad hostname returned from %s for %s",
		       inetntoa((char *)&sin.sin_addr),
		       inetntoa((char *)&rptr->he.h_addr)));
#endif
	}
#ifdef DEBUG
	Debug((DEBUG_INFO,"get_res:Proc answer = %d",a));
#endif
	if (a > 0 && rptr->type == T_PTR)
	    {
		struct	hostent	*hp2 = NULL;

		if (BadPtr(rptr->he.h_name))	/* Kludge!	960907/Vesa */
			goto getres_err;

#ifdef INET6
		Debug((DEBUG_DNS, "relookup %s <-> %s",
			rptr->he.h_name, inet_ntop(AF_INET6,
						   (char *)&rptr->he.h_addr,
						   mydummy, MYDUMMY_SIZE)));
#else
		Debug((DEBUG_DNS, "relookup %s <-> %s",
			rptr->he.h_name, inetntoa((char *)&rptr->he.h_addr)));
#endif
		/*
		 * Lookup the 'authoritive' name that we were given for the
		 * ip#.  By using this call rather than regenerating the
		 * type we automatically gain the use of the cache with no
		 * extra kludges.
		 */
		if ((hp2 = gethost_byname(rptr->he.h_name, &rptr->cinfo)))
			if (lp)
				bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
		/*
		 * If name wasn't found, a request has been queued and it will
		 * be the last one queued.  This is rather nasty way to keep
		 * a host alias with the query. -avalon
		 */
		if (!hp2 && rptr->he.h_aliases[0])
			for (a = 0; rptr->he.h_aliases[a]; a++)
			    {
				Debug((DEBUG_DNS, "Copied CNAME %s for %s",
					rptr->he.h_aliases[a],
					rptr->he.h_name));
				last->he.h_aliases[a] = rptr->he.h_aliases[a];
				rptr->he.h_aliases[a] = NULL;
			    }

		rem_request(rptr);
		return hp2;
	    }

	if (a > 0)
	    {
		if (lp)
			bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
		cp = make_cache(rptr);
#ifdef	DEBUG
	Debug((DEBUG_INFO,"get_res:cp=%#x rptr=%#x (made)",cp,rptr));
#endif

		rem_request(rptr);
	    }
	else
		if (!rptr->sent)
			rem_request(rptr);
	return cp ? (struct hostent *)&cp->he : NULL;

getres_err:
	/*
	 * Reprocess an error if the nameserver didnt tell us to "TRY_AGAIN".
	 */
	if (rptr)
	    {
		if (h_errno != TRY_AGAIN)
		    {
			/*
			 * If we havent tried with the default domain and its
			 * set, then give it a try next.
			 */
			if (ircd_res.options & RES_DEFNAMES && ++rptr->srch == 0)
			    {
				rptr->retries = ircd_res.retry;
				rptr->sends = 0;
				rptr->resend = 1;
#ifdef INET6
/* Comment out this ifdef to get names like ::ffff:a.b.c.d */
				if(rptr->type == T_AAAA)
					query_name(rptr->name, C_IN, T_A, rptr);
					Debug((DEBUG_DNS,"getres_err: didn't work with T_AAAA, now also trying with T_A for %s",rptr->name));
#endif
				resend_query(rptr);
			    }
			else
			    {
#ifdef INET6
/* Comment out this ifdef to get names like ::ffff:a.b.c.d */
				if(rptr->type == T_AAAA)
					query_name(rptr->name, C_IN, T_A, rptr);
					Debug((DEBUG_DNS,"getres_err: didn't work with T_AAAA, now also trying with T_A for %s",rptr->name));
#endif
				resend_query(rptr);
			    }
		    }
		else if (lp)
			bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
	    }
	return (struct hostent *)NULL;
}

static	int	hash_number(ip)
Reg	u_char	*ip;
{
	Reg	u_int	hashv = 0;

	/* could use loop but slower */
	hashv += (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
#ifdef INET6
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
#endif
	hashv += hashv + (int)*ip;
	hashv %= ARES_CACSIZE;
	return (hashv);
}

static	int	hash_name(name)
register	char	*name;
{
	Reg	u_int	hashv = 0;

	for (; *name && *name != '.'; name++)
		hashv += *name;
	hashv %= ARES_CACSIZE;
	return (hashv);
}

/*
** Add a new cache item to the queue and hash table.
*/
static	aCache	*add_to_cache(ocp)
Reg	aCache	*ocp;
{
	Reg	aCache	*cp = NULL;
	Reg	int	hashv;

#ifdef DEBUG
	Debug((DEBUG_INFO,
	      "add_to_cache:ocp %#x he %#x name %#x addrl %#x 0 %#x",
		ocp, &ocp->he, ocp->he.h_name, ocp->he.h_addr_list,
		ocp->he.h_addr_list[0]));
#endif
	ocp->list_next = cachetop;
	cachetop = ocp;

	hashv = hash_name(ocp->he.h_name);
	ocp->hname_next = hashtable[hashv].name_list;
	hashtable[hashv].name_list = ocp;

	hashv = hash_number((u_char *)ocp->he.h_addr);
	ocp->hnum_next = hashtable[hashv].num_list;
	hashtable[hashv].num_list = ocp;

#ifdef	DEBUG
#ifdef INET6
	Debug((DEBUG_INFO,"add_to_cache:added %s[%08x%08x%08x%08x] cache %#x.",
	       ocp->he.h_name,
	       ((struct in6_addr *)ocp->he.h_addr_list)->s6_laddr[0],
	       ((struct in6_addr *)ocp->he.h_addr_list)->s6_laddr[1],
	       ((struct in6_addr *)ocp->he.h_addr_list)->s6_laddr[2],
	       ((struct in6_addr *)ocp->he.h_addr_list)->s6_laddr[3], ocp));
#else
	Debug((DEBUG_INFO, "add_to_cache:added %s[%08x] cache %#x.",
		ocp->he.h_name, ocp->he.h_addr_list[0], ocp));
#endif
	Debug((DEBUG_INFO,
		"add_to_cache:h1 %d h2 %x lnext %#x namnext %#x numnext %#x",
		hash_name(ocp->he.h_name), hashv, ocp->list_next,
		ocp->hname_next, ocp->hnum_next));
#endif

	/*
	 * LRU deletion of excessive cache entries.
	 */
	if (++incache > MAXCACHED)
	    {
		for (cp = cachetop; cp->list_next; cp = cp->list_next)
			;
		rem_cache(cp);
	    }
	cainfo.ca_adds++;

	return ocp;
}

/*
** update_list does not alter the cache structure passed. It is assumed that
** it already contains the correct expire time, if it is a new entry. Old
** entries have the expirey time updated.
*/
static	void	update_list(rptr, cachep)
ResRQ	*rptr;
aCache	*cachep;
{
	Reg	aCache	**cpp, *cp = cachep;
	Reg	char	*s, *t, **base;
	Reg	int	i, j;
	int	addrcount;

	/*
	** search for the new cache item in the cache list by hostname.
	** If found, move the entry to the top of the list and return.
	*/
	cainfo.ca_updates++;

	for (cpp = &cachetop; *cpp; cpp = &((*cpp)->list_next))
		if (cp == *cpp)
			break;
	if (!*cpp)
		return;
	*cpp = cp->list_next;
	cp->list_next = cachetop;
	cachetop = cp;
	if (!rptr)
		return;

#ifdef	DEBUG
	Debug((DEBUG_DEBUG,"u_l:cp %#x na %#x al %#x ad %#x",
		cp,cp->he.h_name,cp->he.h_aliases,cp->he.h_addr));
	Debug((DEBUG_DEBUG,"u_l:rptr %#x h_n %#x", rptr, rptr->he.h_name));
#endif
	/*
	 * Compare the cache entry against the new record.  Add any
	 * previously missing names for this entry.
	 */
	for (i = 0; cp->he.h_aliases[i]; i++)
		;
	addrcount = i;
	for (i = 0, s = rptr->he.h_name; s && i < MAXALIASES;
	     s = rptr->he.h_aliases[i++])
	    {
		for (j = 0, t = cp->he.h_name; t && j < MAXALIASES;
		     t = cp->he.h_aliases[j++])
			if (!mycmp(t, s))
				break;
		if (!t && j < MAXALIASES-1)
		    {
			base = cp->he.h_aliases;

			addrcount++;
			base = (char **)MyRealloc((char *)base,
					sizeof(char *) * (addrcount + 1));
			cp->he.h_aliases = base;
#ifdef	DEBUG
			Debug((DEBUG_DNS,"u_l:add name %s hal %x ac %d",
				s, cp->he.h_aliases, addrcount));
#endif
			base[addrcount-1] = mystrdup(s);
			base[addrcount] = NULL;
		    }
	    }
#ifdef INET6
	for (i = 0; cp->he.h_addr_list[i]; i++)
#else
	for (i = 0; cp->he.h_addr_list[i]; i++)
#endif
		;
	addrcount = i;

	/*
	 * Do the same again for IP#'s.
	 */
#ifdef INET6
	for (s = (char *)rptr->he.h_addr.S_ADDR;
	     ((struct IN_ADDR *)s)->S_ADDR; s += sizeof(struct IN_ADDR))
#else
	for (s = (char *)&rptr->he.h_addr.S_ADDR;
	     ((struct IN_ADDR *)s)->S_ADDR; s += sizeof(struct IN_ADDR))
#endif
	    {
#ifdef INET6
		for (i = 0; (t = cp->he.h_addr_list[i]); i++)
#else
		for (i = 0; (t = cp->he.h_addr_list[i]); i++)
#endif
			if (!bcmp(s, t, sizeof(struct IN_ADDR)))
				break;
		if (i >= MAXADDRS || addrcount >= MAXADDRS)
			break;
		/*
		 * Oh man this is bad...I *HATE* it. -avalon
		 *
		 * Whats it do ?  Reallocate two arrays, one of pointers
		 * to "char *" and the other of IP addresses.  Contents of
		 * the IP array *MUST* be preserved and the pointers into
		 * it recalculated.
		 */
		if (!t)
		    {
			struct	IN_ADDR	**ab;

			ab = (struct IN_ADDR **)cp->he.h_addr_list;
			addrcount++;
			t = (char *)MyRealloc((char *)*ab,
					addrcount * sizeof(struct IN_ADDR));
			base = (char **)MyRealloc((char *)ab,
					(addrcount + 1) * sizeof(*ab));
			cp->he.h_addr_list = base;
#ifdef	DEBUG
			Debug((DEBUG_DNS,"u_l:add IP %x hal %x ac %d",
				ntohl(((struct IN_ADDR *)s)->S_ADDR),
				cp->he.h_addr_list,
				addrcount));
#endif
			for (; addrcount; addrcount--)
			    {
				*ab++ = (struct IN_ADDR *)t;
				t += sizeof(struct IN_ADDR);
			    }
			*ab = NULL;
			bcopy(s, (char *)*--ab, sizeof(struct IN_ADDR));
		    }
	    }
	return;
}

static	aCache	*find_cache_name(name)
char	*name;
{
	Reg	aCache	*cp;
	Reg	char	*s;
	Reg	int	hashv, i;

	hashv = hash_name(name);

	cp = hashtable[hashv].name_list;
#ifdef	DEBUG
	Debug((DEBUG_DNS,"find_cache_name:find %s : hashv = %d",name,hashv));
#endif

	for (; cp; cp = cp->hname_next)
		for (i = 0, s = cp->he.h_name; s; s = cp->he.h_aliases[i++])
			if (mycmp(s, name) == 0)
			    {
				cainfo.ca_na_hits++;
				update_list(NULL, cp);
				return cp;
			    }

	for (cp = cachetop; cp; cp = cp->list_next)
	    {
		/*
		 * if no aliases or the hash value matches, we've already
		 * done this entry and all possiblilities concerning it.
		 */
		if (!*cp->he.h_aliases)
			continue;
		if (hashv == hash_name(cp->he.h_name))
			continue;
		for (i = 0, s = cp->he.h_aliases[i]; s && i < MAXALIASES; i++)
			if (!mycmp(name, s)) {
				cainfo.ca_na_hits++;
				update_list(NULL, cp);
				return cp;
			    }
	    }
	return NULL;
}

/*
 * find a cache entry by ip# and update its expire time
 */
static	aCache	*find_cache_number(rptr, numb)
ResRQ	*rptr;
char	*numb;
{
	Reg	aCache	*cp;
	Reg	int	hashv,i;
#ifdef	DEBUG
	struct	IN_ADDR	*ip = (struct IN_ADDR *)numb;
#endif

	hashv = hash_number((u_char *)numb);

	cp = hashtable[hashv].num_list;
#ifdef DEBUG
#ifdef INET6
	Debug((DEBUG_DNS,
	       "find_cache_number:find %s[%08x%08x%08x%08x]: hashv = %d",
	       inet_ntop(AF_INET6, numb,mydummy,MYDUMMY_SIZE), ip->s6_laddr[0],
	       ip->s6_laddr[1], ip->s6_laddr[2], ip->s6_laddr[3], hashv));
#else
	Debug((DEBUG_DNS,"find_cache_number:find %s[%08x]: hashv = %d",
		inetntoa(numb), ntohl(ip->s_addr), hashv));
#endif
#endif
	for (; cp; cp = cp->hnum_next) 
	    {
#ifdef INET6
		for (i = 0; cp->he.h_addr_list[i]; i++)
#else
		for (i = 0; cp->he.h_addr_list[i]; i++)
#endif
		    {
			if (!bcmp(cp->he.h_addr_list[i], numb,
				  sizeof(struct IN_ADDR)))
			    {
				cainfo.ca_nu_hits++;
				update_list(rptr, cp);
				return cp;
			    }
		    }
	    }
	for (cp = cachetop; cp; cp = cp->list_next)
	    {
		if (!cp->he.h_addr_list && !cp->he.h_aliases)
		    {
			cp = rem_list(cp);
			continue;
		    }
		/*
		 * single address entry...would have been done by hashed
		 * search above...
		 */
#ifdef INET6
		if (!cp->he.h_addr_list[1])
#else
		if (!cp->he.h_addr_list[1])
#endif
			continue;
		/*
		 * if the first IP# has the same hashnumber as the IP# we
		 * are looking for, its been done already.
		 */
		if (hashv == hash_number((u_char *)cp->he.h_addr_list[0]))
			continue;
#ifdef INET6
		for (i = 1; cp->he.h_addr_list[i]; i++)
#else
		for (i = 1; cp->he.h_addr_list[i]; i++)
#endif
			if (!bcmp(cp->he.h_addr_list[i], numb,
				  sizeof(struct IN_ADDR)))
			    {
				cainfo.ca_nu_hits++;
				update_list(rptr, cp);
				return cp;
			    }
	    }
	return NULL;
}

static	aCache	*make_cache(rptr)
ResRQ	*rptr;
{
	Reg	aCache	*cp;
	Reg	int	i, n;
	Reg	struct	hostent	*hp;
	Reg	char	*s, **t;

	/*
	** shouldn't happen but it just might...
	*/
	if (!rptr->he.h_name || !WHOSTENTP(rptr->he.h_addr.S_ADDR))
		return NULL;
	/*
	** Make cache entry.  First check to see if the cache already exists
	** and if so, return a pointer to it.
	*/
	for (i = 0; WHOSTENTP(rptr->he.h_addr_list[i].S_ADDR); i++)
		if ((cp = find_cache_number(rptr,
#ifdef INET6
				(char *)(rptr->he.h_addr_list[i].S_ADDR))))
#else
				(char *)&(rptr->he.h_addr_list[i].S_ADDR))))
#endif
			return cp;

	/*
	** a matching entry wasnt found in the cache so go and make one up.
	*/ 
	cp = (aCache *)MyMalloc(sizeof(aCache));
	bzero((char *)cp, sizeof(aCache));
	hp = &cp->he;
	for (i = 0; i < MAXADDRS - 1; i++)
		if (!WHOSTENTP(rptr->he.h_addr_list[i].S_ADDR))
			break;

	/*
	** build two arrays, one for IP#'s, another of pointers to them.
	*/
	t = hp->h_addr_list = (char **)MyMalloc(sizeof(char *) * (i+1));
	bzero((char *)t, sizeof(char *) * (i+1));

	s = (char *)MyMalloc(sizeof(struct IN_ADDR) * i);
	bzero(s, sizeof(struct IN_ADDR) * i);

	for (n = 0; n < i; n++, s += sizeof(struct IN_ADDR))
	    {
		*t++ = s;
		bcopy((char *)&rptr->he.h_addr_list[n], s,
		      sizeof(struct IN_ADDR));
	    }
	*t = (char *)NULL;

	/*
	** an array of pointers to CNAMEs.
	*/
	for (i = 0; i < MAXALIASES - 1; i++)
		if (!rptr->he.h_aliases[i])
			break;
	i++;
	t = hp->h_aliases = (char **)MyMalloc(sizeof(char *) * i);
	for (n = 0; n < i; n++, t++)
	    {
		*t = rptr->he.h_aliases[n];
		rptr->he.h_aliases[n] = NULL;
	    }

	hp->h_addrtype = rptr->he.h_addrtype;
	hp->h_length = rptr->he.h_length;
	hp->h_name = rptr->he.h_name;
	if (rptr->ttl < 600)
	    {
		reinfo.re_shortttl++;
		cp->ttl = 600;
	    }
	else
		cp->ttl = rptr->ttl;
	cp->expireat = timeofday + cp->ttl;
	rptr->he.h_name = NULL;
#ifdef DEBUG
	Debug((DEBUG_INFO,"make_cache:made cache %#x", cp));
#endif
	return add_to_cache(cp);
}

/*
 * rem_list
 */
static	aCache	*rem_list(cp)
aCache	*cp;
{
	aCache	**cpp, *cr = cp->list_next;

	/*
	 * remove cache entry from linked list
	 */
	for (cpp = &cachetop; *cpp; cpp = &((*cpp)->list_next))
		if (*cpp == cp)
		    {
			*cpp = cp->list_next;
			MyFree((char *)cp);
			break;
		    }
	return cr;
}


/*
 * rem_cache
 *     delete a cache entry from the cache structures and lists and return
 *     all memory used for the cache back to the memory pool.
 */
static	void	rem_cache(ocp)
aCache	*ocp;
{
	Reg	aCache	**cp;
	Reg	struct	hostent *hp = &ocp->he;
	Reg	int	hashv;
	Reg	aClient	*cptr;

#ifdef	DEBUG
	Debug((DEBUG_DNS, "rem_cache: ocp %#x hp %#x l_n %#x aliases %#x",
		ocp, hp, ocp->list_next, hp->h_aliases));
#endif
	/*
	** Cleanup any references to this structure by destroying the
	** pointer.
	*/
	for (hashv = highest_fd; hashv >= 0; hashv--)
		if ((cptr = local[hashv]) && (cptr->hostp == hp))
			cptr->hostp = NULL;
	/*
	 * remove cache entry from linked list
	 */
	for (cp = &cachetop; *cp; cp = &((*cp)->list_next))
		if (*cp == ocp)
		    {
			*cp = ocp->list_next;
			break;
		    }
	/*
	 * remove cache entry from hashed name lists
	 */
	hashv = hash_name(hp->h_name);
#ifdef	DEBUG
	Debug((DEBUG_DEBUG,"rem_cache: h_name %s hashv %d next %#x first %#x",
		hp->h_name, hashv, ocp->hname_next,
		hashtable[hashv].name_list));
#endif
	for (cp = &hashtable[hashv].name_list; *cp; cp = &((*cp)->hname_next))
		if (*cp == ocp)
		    {
			*cp = ocp->hname_next;
			break;
		    }
	/*
	 * remove cache entry from hashed number list
	 */
	hashv = hash_number((u_char *)hp->h_addr);
#ifdef	DEBUG
# ifdef INET6
	Debug((DEBUG_DEBUG,"rem_cache: h_addr %s hashv %d next %#x first %#x",
	       inet_ntop(AF_INET6, hp->h_addr, mydummy, MYDUMMY_SIZE),
	       hashv, ocp->hnum_next, hashtable[hashv].num_list));
# else
	Debug((DEBUG_DEBUG,"rem_cache: h_addr %s hashv %d next %#x first %#x",
	       inetntoa(hp->h_addr),
	       hashv, ocp->hnum_next, hashtable[hashv].num_list));
# endif
#endif
	for (cp = &hashtable[hashv].num_list; *cp; cp = &((*cp)->hnum_next))
		if (*cp == ocp)
		    {
			*cp = ocp->hnum_next;
			break;
		    }

	/*
	 * free memory used to hold the various host names and the array
	 * of alias pointers.
	 */
	if (hp->h_name)
		MyFree(hp->h_name);
	if (hp->h_aliases)
	    {
		for (hashv = 0; hp->h_aliases[hashv]; hashv++)
			MyFree(hp->h_aliases[hashv]);
		MyFree((char *)hp->h_aliases);
	    }

	/*
	 * free memory used to hold ip numbers and the array of them.
	 */
	if (hp->h_addr_list)
	    {
		if (*hp->h_addr_list)
			MyFree((char *)*hp->h_addr_list);
		MyFree((char *)hp->h_addr_list);
	    }

	MyFree((char *)ocp);

	incache--;
	cainfo.ca_dels++;

	return;
}

/*
 * removes entries from the cache which are older than their expirey times.
 * returns the time at which the server should next poll the cache.
 */
time_t	expire_cache(now)
time_t	now;
{
	Reg	aCache	*cp, *cp2;
	Reg	time_t	next = 0;

	for (cp = cachetop; cp; cp = cp2)
	    {
		cp2 = cp->list_next;

		if (now >= cp->expireat)
		    {
			cainfo.ca_expires++;
			rem_cache(cp);
		    }
		else if (!next || next > cp->expireat)
			next = cp->expireat;
	    }
	return (next > now) ? next : (now + AR_TTL);
}

/*
 * remove all dns cache entries.
 */
void	flush_cache()
{
	Reg	aCache	*cp;

	while ((cp = cachetop))
		rem_cache(cp);
}

int	m_dns(cptr, sptr, parc, parv)
aClient *cptr, *sptr;
int	parc;
char	*parv[];
{
	Reg	aCache	*cp;
	Reg	int	i;

	if (parv[1] && *parv[1] == 'l') {
		for(cp = cachetop; cp; cp = cp->list_next)
		    {
			sendto_one(sptr, "NOTICE %s :Ex %d ttl %d host %s(%s)",
				   parv[0], cp->expireat - timeofday, cp->ttl,
#ifdef INET6
				   cp->he.h_name, inetntop(AF_INET6,
							    cp->he.h_addr,
							    mydummy,
							    MYDUMMY_SIZE));
#else
				   cp->he.h_name, inetntoa(cp->he.h_addr));
#endif
			for (i = 0; cp->he.h_aliases[i]; i++)
				sendto_one(sptr,"NOTICE %s : %s = %s (CN)",
					   parv[0], cp->he.h_name,
					   cp->he.h_aliases[i]);
#ifdef INET6
			for (i = 1; cp->he.h_addr_list[i]; i++) {
#else
			for (i = 1; cp->he.h_addr_list[i]; i++) {
#endif
				sendto_one(sptr,"NOTICE %s : %s = %s (IP)",
					   parv[0], cp->he.h_name,
#ifdef INET6
					   inetntop(AF_INET6, 
						     cp->he.h_addr_list[i],
						     mydummy, MYDUMMY_SIZE));
#else
					   inetntoa(cp->he.h_addr_list[i]));
#endif
			}
		    }
		return 2;
	}
	sendto_one(sptr,"NOTICE %s :Ca %d Cd %d Ce %d Cl %d Ch %d:%d Cu %d",
		   sptr->name,
		   cainfo.ca_adds, cainfo.ca_dels, cainfo.ca_expires,
		   cainfo.ca_lookups,
		   cainfo.ca_na_hits, cainfo.ca_nu_hits, cainfo.ca_updates);

	sendto_one(sptr,"NOTICE %s :Re %d Rl %d/%d Rp %d Rq %d",
		   sptr->name, reinfo.re_errors, reinfo.re_nu_look,
		   reinfo.re_na_look, reinfo.re_replies, reinfo.re_requests);
	sendto_one(sptr,"NOTICE %s :Ru %d Rsh %d Rs %d(%d) Rt %d", sptr->name,
		   reinfo.re_unkrep, reinfo.re_shortttl, reinfo.re_sent,
		   reinfo.re_resends, reinfo.re_timeouts);
	return 2;
}

u_long	cres_mem(sptr, nick)
aClient	*sptr;
char	*nick;
{
	register aCache	*c = cachetop;
	register struct	hostent	*h;
	register int	i;
	u_long	nm = 0, im = 0, sm = 0, ts = 0;

	for ( ;c ; c = c->list_next)
	    {
		sm += sizeof(*c);
		h = &c->he;
#ifdef INET6
		for (i = 0; h->h_addr_list[i]; i++)
#else
		for (i = 0; h->h_addr_list[i]; i++)
#endif
		    {
			im += sizeof(char *);
			im += sizeof(struct IN_ADDR);
		    }
		im += sizeof(char *);
		for (i = 0; h->h_aliases[i]; i++)
		    {
			nm += sizeof(char *);
			nm += strlen(h->h_aliases[i]);
		    }
		nm += i - 1;
		nm += sizeof(char *);
		if (h->h_name)
			nm += strlen(h->h_name);
	    }
	ts = ARES_CACSIZE * sizeof(CacheTable);
	sendto_one(sptr, ":%s %d %s :RES table %d",
		   me.name, RPL_STATSDEBUG, nick, ts);
	sendto_one(sptr, ":%s %d %s :Structs %d IP storage %d Name storage %d",
		   me.name, RPL_STATSDEBUG, nick, sm, im, nm);
	return ts + sm + im + nm;
}


static	int	bad_hostname(name, len)
char *name;
int len;
{
	char	*s, c;

	for (s = name; (c = *s) && len; s++, len--)
		if (isspace(c) || (c == 0x7) || (c == ':') ||
		    (c == '*') || (c == '?'))
			return -1;
	return 0;
}
