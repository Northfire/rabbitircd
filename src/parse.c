/************************************************************************
 *   Unreal Internet Relay Chat Daemon, src/parse.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *                      University of Oulu, Computing Center
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

/* -- Jto -- 03 Jun 1990
 * Changed the order of defines...
 */

#ifndef CLEAN_COMPILE
static char sccsid[] =
    "@(#)parse.c	2.33 1/30/94 (C) 1988 University of Oulu, \
Computing Center and Jarkko Oikarinen";
#endif
#include <string.h>
#include "struct.h"
#include "common.h"

ID_Copyright
    ("(C) 1988 University of Oulu, Computing Center and Jarkko Oikarinen");
ID_Notes("2.33 1/30/94");
#undef RAWDEBUG

char backupbuf[8192];

#define MSGTAB
#include "msg.h"
#undef MSGTAB
#include "sys.h"
#include "numeric.h"
#include "h.h"
#include "proto.h"

/*
 * NOTE: parse() should not be called recursively by other functions!
 */
static char *para[MAXPARA + 2];

static char sender[HOSTLEN + 1];
static int cancel_clients(aClient *, aClient *, char *);
static void remove_unknown(aClient *, char *);
/*
**  Find a client (server or user) by name.
**
**  *Note*
**	Semantics of this function has been changed from
**	the old. 'name' is now assumed to be a null terminated
**	string and the search is the for server and user.
*/
aClient *find_client(char *name, aClient *cptr)
{
	if (cptr == NULL || IsServer(cptr))
	{
		aClient *acptr;

		if ((acptr = hash_find_id(name, NULL)) != NULL)
			return acptr;
	}

	return hash_find_client(name, NULL);
}

aClient inline *find_nickserv(char *name, aClient *cptr)
{
	if (name)
		cptr = hash_find_nickserver(name, cptr);

	return cptr;
}


/*
**  Find server by name.
**
**	This implementation assumes that server and user names
**	are unique, no user can have a server name and vice versa.
**	One should maintain separate lists for users and servers,
**	if this restriction is removed.
**
**  *Note*
**	Semantics of this function has been changed from
**	the old. 'name' is now assumed to be a null terminated
**	string.
*/
aClient inline *find_server(char *name, aClient *cptr)
{
	if (name)
	{
		cptr = hash_find_server(name, cptr);
	}
	return cptr;
}


aClient inline *find_name(char *name, aClient *cptr)
{
	aClient *c2ptr = cptr;

	if (!collapse(name))
		return c2ptr;

	if ((c2ptr = hash_find_server(name, cptr)))
		return (c2ptr);
	if (!index(name, '*'))
		return c2ptr;
	list_for_each_entry(c2ptr, &client_list, client_node)
	{
		if (!IsServer(c2ptr) && !IsMe(c2ptr))
			continue;
		if (match(name, c2ptr->name) == 0)
			break;
		if (index(c2ptr->name, '*'))
			if (match(c2ptr->name, name) == 0)
				break;
	}
	return (c2ptr ? c2ptr : cptr);
}

/*
**  Find person by (nick)name.
*/
aClient *find_person(char *name, aClient *cptr)
{
	aClient *c2ptr;

	c2ptr = find_client(name, cptr);

	if (c2ptr && IsClient(c2ptr) && c2ptr->user)
		return c2ptr;

	return NULL;
}


void ban_flooder(aClient *cptr)
{
	/* place_host_ban also takes care of removing any other clients with same host/ip */
	place_host_ban(cptr, BAN_ACT_ZLINE, "Flood from unknown connection", UNKNOWN_FLOOD_BANTIME);
	return;
}

/*
 * This routine adds fake lag if needed.
 */
inline void parse_addlag(aClient *cptr, int cmdbytes)
{
	if (!IsServer(cptr) && !IsNoFakeLag(cptr) &&
#ifdef FAKELAG_CONFIGURABLE
		!(cptr->class && (cptr->class->options & CLASS_OPT_NOFAKELAG)) && 
#endif
#ifdef NO_FAKE_LAG_FOR_LOCOPS	
	!IsAnOper(cptr))
#else
	!IsOper(cptr))
#endif		
	{
		cptr->since += (1 + cmdbytes/90);
	}		
}

/*
 * parse a buffer.
 *
 * NOTE: parse() should not be called recusively by any other fucntions!
 */
int  parse(aClient *cptr, char *buffer, char *bufend)
{
	Hook *h;
	int buf_len = 0;
	aClient *from = cptr;
	char *ch, *s;
	int  len, i, numeric = 0, paramcount, noprefix = 0;
#ifdef DEBUGMODE
	time_t then, ticks;
	int  retval;
#endif
	aCommand *cmptr = NULL;

	for(h = Hooks[HOOKTYPE_PACKET]; h; h = h->next) {
		buf_len = (int)(bufend - buffer);
		(*(h->func.intfunc))(from, &me, &buffer, &buf_len);
		if(!buffer) return 0;
		bufend = buffer + buf_len;
	}

	Debug((DEBUG_ERROR, "Parsing: %s (from %s)", buffer,
	    (*cptr->name ? cptr->name : "*")));
	if (IsDead(cptr))
		return 0;

	if ((cptr->receiveK >= UNKNOWN_FLOOD_AMOUNT) && IsUnknown(cptr))
	{
		sendto_snomask(SNO_FLOOD, "Flood from unknown connection %s detected",
			cptr->sockhost);
		ban_flooder(cptr);
		return FLUSH_BUFFER;
	}

	/* this call is a bit obsolete? - takes up CPU */
	backupbuf[0] = '\0';
	strcpy(backupbuf, buffer);
	s = sender;
	*s = '\0';
	for (ch = buffer; *ch == ' '; ch++)
		;
	para[0] = from->name;
	if (*ch == ':' || *ch == '@')
	{
		/*
		   ** Copy the prefix to 'sender' assuming it terminates
		   ** with SPACE (or NULL, which is an error, though).
		 */
		for (++ch, i = 0; *ch && *ch != ' '; ++ch)
			if (s < (sender + sizeof(sender) - 1))
				*s++ = *ch;	/* leave room for NULL */
		*s = '\0';
		/*
		   ** Actually, only messages coming from servers can have
		   ** the prefix--prefix silently ignored, if coming from
		   ** a user client...
		   **
		   ** ...sigh, the current release "v2.2PL1" generates also
		   ** null prefixes, at least to NOTIFY messages (e.g. it
		   ** puts "sptr->nickname" as prefix from server structures
		   ** where it's null--the following will handle this case
		   ** as "no prefix" at all --msa  (": NOTICE nick ...")
		 */
		if (*sender && IsServer(cptr))
		{
			from = find_client(sender, (aClient *)NULL);
			if (!from || match(from->name, sender))
				from = find_server_quick(sender);
			else if (!from && index(sender, '@'))
				from = find_nickserv(sender, (aClient *)NULL);
			para[0] = sender;

			/* Hmm! If the client corresponding to the
			 * prefix is not found--what is the correct
			 * action??? Now, I will ignore the message
			 * (old IRC just let it through as if the
			 * prefix just wasn't there...) --msa
			 */

			if (!from)
			{
				Debug((DEBUG_ERROR,
				    "Unknown prefix (%s)(%s) from (%s)",
				    sender, buffer, cptr->name));
				ircstp->is_unpf++;
				remove_unknown(cptr, sender);
				return -1;
			}
			if (from->from != cptr)
			{
				ircstp->is_wrdi++;
				Debug((DEBUG_ERROR,
				    "Message (%s) coming from (%s)",
				    buffer, cptr->name));
				return cancel_clients(cptr, from, ch);
			}
		}
		while (*ch == ' ')
			ch++;
	}
	else
		noprefix = 1;
	if (*ch == '\0')
	{
		ircstp->is_empt++;
		Debug((DEBUG_NOTICE, "Empty message from host %s:%s",
		    cptr->name, from->name));
		if (!IsServer(cptr))
			cptr->since++; /* 1s fake lag */
		return (-1);
	}
	/*
	   ** Extract the command code from the packet.  Point s to the end
	   ** of the command code and calculate the length using pointer
	   ** arithmetic.  Note: only need length for numerics and *all*
	   ** numerics must have paramters and thus a space after the command
	   ** code. -avalon
	 */
	s = (char *)index(ch, ' ');	/* s -> End of the command code */
	len = (s) ? (s - ch) : 0;
	if (len == 3 &&
	    isdigit(*ch) && isdigit(*(ch + 1)) && isdigit(*(ch + 2)))
	{
		cmptr = NULL;
		numeric = (*ch - '0') * 100 + (*(ch + 1) - '0') * 10
		    + (*(ch + 2) - '0');
		paramcount = MAXPARA;
		ircstp->is_num++;
	}
	else
	{
		int flags = 0;
		int bytes = bufend - ch;
		if (s)
			*s++ = '\0';
		if (!IsRegistered(from))
			flags |= M_UNREGISTERED;
		if (IsPerson(from))
			flags |= M_USER;
		if (IsServer(from))
			flags |= M_SERVER;
		if (IsShunned(from))
			flags |= M_SHUN;
		if (IsVirus(from))
			flags |= M_VIRUS;
		if (IsAnOper(from))
			flags |= M_OPER;
		cmptr = find_Command(ch, IsServer(cptr) ? 1 : 0, flags);
		if (!cmptr)
		{
			/*
			   ** Note: Give error message *only* to recognized
			   ** persons. It's a nightmare situation to have
			   ** two programs sending "Unknown command"'s or
			   ** equivalent to each other at full blast....
			   ** If it has got to person state, it at least
			   ** seems to be well behaving. Perhaps this message
			   ** should never be generated, though...  --msa
			   ** Hm, when is the buffer empty -- if a command
			   ** code has been found ?? -Armin
			   ** This error should indeed not be sent in case
			   ** of notices -- Syzop.
			 */
			if (!IsRegistered(cptr) && stricmp(ch, "NOTICE")) {
				sendto_one(from, ":%s %d %s :You have not registered",
				    me.name, ERR_NOTREGISTERED, ch);
				parse_addlag(cptr, bytes);
				return -1;
			}
			if (IsShunned(cptr))
				return -1;
				
			if (buffer[0] != '\0')
			{
				if (IsPerson(from))
					sendto_one(from,
					    ":%s %d %s %s :Unknown command",
					    me.name, ERR_UNKNOWNCOMMAND,
					    from->name, ch);
				Debug((DEBUG_ERROR, "Unknown (%s) from %s",
				    ch, get_client_name(cptr, TRUE)));
				parse_addlag(cptr, bytes);
			}
			ircstp->is_unco++;
			return (-1);
		}
		if (cmptr->flags != 0) { /* temporary until all commands are updated */
		if ((flags & M_USER) && !(cmptr->flags & M_USER))
		{
			sendto_one(cptr, rpl_str(ERR_NOTFORUSERS), me.name,
					from->name, cmptr->cmd);
			return -1;
		}
		if ((flags & M_SERVER) && !(cmptr->flags & M_SERVER))
			return -1;
		}
		if ((cmptr->flags & M_OPER) && !(flags & M_OPER))
		{
			sendto_one(cptr, rpl_str(ERR_NOPRIVILEGES), 
					me.name, from->name);
			return -1;
		}
		paramcount = cmptr->parameters;
		cmptr->bytes += bytes;
		if (!(cmptr->flags & M_NOLAG))
			parse_addlag(cptr, bytes);
	}
	/*
	   ** Must the following loop really be so devious? On
	   ** surface it splits the message to parameters from
	   ** blank spaces. But, if paramcount has been reached,
	   ** the rest of the message goes into this last parameter
	   ** (about same effect as ":" has...) --msa
	 */

	/* Note initially true: s==NULL || *(s-1) == '\0' !! */

	i = 0;
	if (s)
	{
		/*
		if (paramcount > MAXPARA)
			paramcount = MAXPARA;
		We now use functions to create commands, so we can just check this 
		once when the command is created rather than each time the command
		is used -- codemastr
		*/
		for (;;)
		{
			/*
			   ** Never "FRANCE " again!! ;-) Clean
			   ** out *all* blanks.. --msa
			 */
			while (*s == ' ')
				*s++ = '\0';

			if (*s == '\0')
				break;
			if (*s == ':')
			{
				/*
				   ** The rest is single parameter--can
				   ** include blanks also.
				 */
				para[++i] = s + 1;
				break;
			}
			para[++i] = s;
			if (i >= paramcount)
				break;
			for (; *s != ' ' && *s; s++)
				;
		}
	}
	para[++i] = NULL;
	if (cmptr == NULL)
		return (do_numeric(numeric, cptr, from, i, para));
	cmptr->count++;
	if (IsRegisteredUser(cptr) && (cmptr->flags & M_RESETIDLE))
		cptr->last = TStime();

#ifndef DEBUGMODE
	if (cmptr->flags & M_ALIAS)
		return (*cmptr->func) (cptr, from, i, para, cmptr->cmd);
	else
	{
		if (!cmptr->overriders)
			return (*cmptr->func) (cptr, from, i, para);
		return (*cmptr->overridetail->func) (cmptr->overridetail, cptr, from, i, para);
	}
#else
	then = clock();
	if (cmptr->flags & M_ALIAS)
		retval = (*cmptr->func) (cptr, from, i, para, cmptr->cmd);
	else 
	{
		if (!cmptr->overriders)
			retval = (*cmptr->func) (cptr, from, i, para);
		else
			retval = (*cmptr->overridetail->func) (cmptr->overridetail, cptr, from, i, para);
	}
	if (retval != FLUSH_BUFFER)
	{
		ticks = (clock() - then);
		if (IsServer(cptr))
			cmptr->rticks += ticks;
		else
			cmptr->lticks += ticks;
		cptr->cputime += ticks;
	}

	return retval;
#endif
}

static int cancel_clients(aClient *cptr, aClient *sptr, char *cmd)
{
	if (IsServer(cptr) || IsServer(sptr) || IsMe(sptr)) return 0;
	return exit_client(cptr, cptr, &me, "Fake prefix");
}

static void remove_unknown(aClient *cptr, char *sender)
{
	if (!IsRegistered(cptr) || IsClient(cptr))
		return;
	/*
	 * Not from a server so don't need to worry about it.
	 */
	if (!IsServer(cptr))
		return;

#ifdef DEVELOP
	sendto_ops("Killing %s (%s)", sender, backupbuf);
	return;
#endif
	/*
	 * Do kill if it came from a server because it means there is a ghost
	 * user on the other server which needs to be removed. -avalon
	 */
	if (!index(sender, '.') && !isdigit(*sender))
		sendto_one(cptr, ":%s KILL %s :%s (%s(?) <- %s)",
		    me.name, sender, me.name, sender, cptr->name);
	else
		sendto_one(cptr, ":%s SQUIT %s :(Unknown from %s)",
		    me.name, sender, get_client_name(cptr, FALSE));
}
