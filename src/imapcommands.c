/*
 Copyright (C) 1999-2004 IC & S  dbmail@ic-s.nl
 Copyright (c) 2004-2006 NFG Net Facilities Group BV support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* 
 *
 * imapcommands.c
 * 
 * IMAP server command implementations
 */

#include "dbmail.h"
#define THIS_MODULE "imap"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

extern db_param_t * _db_params;
#define DBPFX _db_params->pfx

#ifndef MAX_RETRIES
#define MAX_RETRIES 12
#endif

extern const char *imap_flag_desc[];
extern const char *imap_flag_desc_escaped[];

int list_is_lsub = 0;

extern const char AcceptedMailboxnameChars[];

int imap_before_smtp = 0;

/*
 * RETURN VALUES _ic_ functions:
 *
 * -1 Fatal error, close connection to user
 *  0 Succes
 *  1 Non-fatal error, connection stays alive
 */


/* 
 * ANY-STATE COMMANDS: capability, noop, logout
 */

/*
 * _ic_capability()
 *
 * returns a string to the client containing the server capabilities
 */
int _ic_capability(ImapSession *self)
{
	field_t val;
	gboolean override = FALSE;

	if (!check_state_and_args(self, "CAPABILITY", 0, 0, -1))
		return 1;	/* error, return */

	
	GETCONFIGVALUE("capability", "IMAP", val);
	if (strlen(val) > 0)
		override = TRUE;

	dbmail_imap_session_printf(self, "* CAPABILITY %s\r\n", override ? val : IMAP_CAPABILITY_STRING);
	dbmail_imap_session_printf(self, "%s OK CAPABILITY completed\r\n", self->tag);

	return 0;
}


/*
 * _ic_noop()
 *
 * performs No operation
 */
int _ic_noop(ImapSession *self)
{
	if (!check_state_and_args(self, "NOOP", 0, 0, -1))
		return 1;	/* error, return */

	if (self->state == IMAPCS_SELECTED)
		dbmail_imap_session_mailbox_status(self, TRUE);
	
	dbmail_imap_session_printf(self, "%s OK NOOP completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_logout()
 *
 * prepares logout from IMAP-server
 */
int _ic_logout(ImapSession *self)
{
	timestring_t timestring;

	// flush recent messages from previous select
	dbmail_imap_session_mailbox_update_recent(self);

	if (!check_state_and_args(self, "LOGOUT", 0, 0, -1))
		return 1;	/* error, return */

	create_current_timestring(&timestring);
	dbmail_imap_session_set_state(self,IMAPCS_LOGOUT);
	TRACE(TRACE_MESSAGE, "[%p] user (id:%llu) logging out @ [%s]", self, self->userid, timestring);

	dbmail_imap_session_printf(self, "* BYE dbmail imap server kisses you goodbye\r\n");

	return 0;
}

/*
 * PRE-AUTHENTICATED STATE COMMANDS
 * login, authenticate
 */
/*
 * _ic_login()
 *
 * Performs login-request handling.
 */
int _ic_login(ImapSession *self)
{
	int result;
	timestring_t timestring;
	
	if (!check_state_and_args(self, "LOGIN", 2, 2, IMAPCS_NON_AUTHENTICATED))
		return 1;
	
	create_current_timestring(&timestring);
	if ((result = dbmail_imap_session_handle_auth(self, self->args[self->args_idx], self->args[self->args_idx+1])))
		return result;
	if (imap_before_smtp)
		db_log_ip(self->ci->ip_src);

	dbmail_imap_session_printf(self, "%s OK LOGIN completed\r\n", self->tag);

	if (! self->mbxinfo)
		dbmail_imap_session_get_mbxinfo(self);

	return 0;
}


/*
 * _ic_authenticate()
 * 
 * performs authentication using LOGIN mechanism:
 *
 *
 */
int _ic_authenticate(ImapSession *self)
{
	int result;

	timestring_t timestring;
	
	if (!check_state_and_args(self, "AUTHENTICATE", 3, 3, IMAPCS_NON_AUTHENTICATED))
		return 1;

	create_current_timestring(&timestring);

	/* check authentication method */
	if (strcasecmp(self->args[self->args_idx], "login") != 0) {
		dbmail_imap_session_printf(self,
			"%s NO Invalid authentication mechanism specified\r\n",
			self->tag);
		return 1;
	}

	/* try to validate user */
	if ((result = dbmail_imap_session_handle_auth(self,self->args[self->args_idx+1],self->args[self->args_idx+2]))) {
		return result;
	}

	if (imap_before_smtp)
		db_log_ip(self->ci->ip_src);

	dbmail_imap_session_printf(self, "%s OK AUTHENTICATE completed\r\n", self->tag);
	
	if (! self->mbxinfo)
		dbmail_imap_session_get_mbxinfo(self);

	return 0;
}


/* 
 * AUTHENTICATED STATE COMMANDS 
 * select, examine, create, delete, rename, subscribe, 
 * unsubscribe, list, lsub, status, append
 */

/*
 * _ic_select()
 * 
 * select a specified mailbox
 */
#define PERMSTRING_SIZE 80
int _ic_select(ImapSession *self)
{
	u64_t key = 0;
	u64_t *msn = NULL;
	int result;
	char *mailbox;
	char permstring[PERMSTRING_SIZE];

	if (!check_state_and_args(self, "SELECT", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;	/* error, return */

	mailbox = self->args[self->args_idx];
	
	if ((result = dbmail_imap_session_mailbox_open(self, mailbox))) 
		return result;

	/* update permission: select implies read-write */
	self->mailbox->info->permission = IMAPPERM_READWRITE;

	dbmail_imap_session_set_state(self,IMAPCS_SELECTED);

	if ((result = dbmail_imap_session_mailbox_show_info(self)))
		return result;

	/* show idx of first unseen msg (if present) */
	if (self->mailbox->info->exists) {
		key = db_first_unseen(self->mailbox->info->uid);
		if ( (key > 0) && (msn = g_tree_lookup(self->mailbox->ids, &key))) {
			dbmail_imap_session_printf(self,
				"* OK [UNSEEN %llu] first unseen message\r\n", *msn);
		}
	}
	/* permission */
	switch (self->mailbox->info->permission) {
	case IMAPPERM_READ:
		g_snprintf(permstring, PERMSTRING_SIZE, "READ-ONLY");
		break;
	case IMAPPERM_READWRITE:
		g_snprintf(permstring, PERMSTRING_SIZE, "READ-WRITE");
		break;
	default:
		TRACE(TRACE_ERROR, "[%p] detected invalid permission mode for mailbox %llu ('%s')",
		      self, self->mailbox->info->uid, self->args[self->args_idx]);

		dbmail_imap_session_printf(self,
			"* BYE fatal: detected invalid mailbox settings\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "%s OK [%s] SELECT completed\r\n", self->tag,
		permstring);
	return 0;
}


/*
 * _ic_examine()
 * 
 * examines a specified mailbox 
 */
int _ic_examine(ImapSession *self)
{
	int result;
	char *mailbox;

	if (!check_state_and_args(self, "EXAMINE", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;

	mailbox = self->args[self->args_idx];

	if ((result = dbmail_imap_session_mailbox_open(self, mailbox)))
		return result;

	/* update permission: examine forces read-only */
	self->mailbox->info->permission = IMAPPERM_READ;

	dbmail_imap_session_set_state(self,IMAPCS_SELECTED);

	if ((result = dbmail_imap_session_mailbox_show_info(self)))
		return result;
	
	dbmail_imap_session_printf(self, "%s OK [READ-ONLY] EXAMINE completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_create()
 *
 * create a mailbox
 */
int _ic_create(ImapSession *self)
{
	int result;
	const char *message;
	u64_t mboxid;
	
	if (!check_state_and_args(self, "CREATE", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;

	/* Create the mailbox and its parents. */
	result = db_mailbox_create_with_parents(self->args[self->args_idx], BOX_COMMANDLINE, self->userid, &mboxid, &message);

	if (result > 0) {
		dbmail_imap_session_printf(self, "%s NO %s\r\n", self->tag, message);
		return DM_EGENERAL;
	} else if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		return DM_EQUERY;
	}

	dbmail_imap_session_printf(self, "%s OK CREATE completed\r\n", self->tag);
	return DM_SUCCESS;
}


/*
 * _ic_delete()
 *
 * deletes a specified mailbox
 */
int _ic_delete(ImapSession *self)
{
	int result;
	u64_t mboxid;
	GList *children = NULL;
	char *mailbox = self->args[0];
	unsigned nchildren = 0;

	if (!check_state_and_args(self, "DELETE", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;	/* error, return */

	if (! (mboxid = dbmail_imap_session_mailbox_get_idnr(self, mailbox)) ) {
		dbmail_imap_session_printf(self, "%s NO mailbox doesn't exists\r\n", self->tag);
		return 1;
	}

	/* Check if the user has ACL delete rights to this mailbox;
	 * this also returns true is the user owns the mailbox. */
	result = dbmail_imap_session_mailbox_check_acl(self, mboxid, ACL_RIGHT_DELETE);
	if (result != 0)
		return result;
	
	/* check if there is an attempt to delete inbox */
	if (strcasecmp(self->args[0], "inbox") == 0) {
		dbmail_imap_session_printf(self, "%s NO cannot delete special mailbox INBOX\r\n", self->tag);
		return 1;
	}

	/* check for children of this mailbox */
	result = db_listmailboxchildren(mboxid, self->userid, &children);
	if (result == -1) {
		/* error */
		TRACE(TRACE_ERROR, "[%p] cannot retrieve list of mailbox children", self);
		dbmail_imap_session_printf(self, "* BYE dbase/memory error\r\n");
		return -1;
	}

	children = g_list_first(children);
	nchildren = g_list_length(children);
	g_list_destroy(children);

	if (nchildren > 0) {
		/* mailbox has inferior names; error if \noselect specified */
		result = db_isselectable(mboxid);
		if (result == FALSE) {
			dbmail_imap_session_printf(self, "%s NO mailbox is non-selectable\r\n", self->tag);
			return 0;
		}
		if (result == DM_EQUERY) {
			dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
			return -1;	/* fatal */
		}

		/* mailbox has inferior names; remove all msgs and set noselect flag */
		if (db_removemsg(self->userid, mboxid) == DM_EQUERY) {
			dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
			return -1;	/* fatal */
		}

		if (! db_setselectable(mboxid, 0)) {
			dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
			return -1;	/* fatal */
		}

		/* check if this was the currently selected mailbox */
		if (self->mailbox->info && (mboxid == self->mailbox->info->uid)) 
			dbmail_imap_session_set_state(self,IMAPCS_AUTHENTICATED);

		/* ok done */
		dbmail_imap_session_printf(self, "%s OK DELETE completed\r\n", self->tag);
		return 0;
	}

	/* ok remove mailbox */
	if (db_delete_mailbox(mboxid, 0, 1)) {
		TRACE(TRACE_DEBUG,"[%p] db_delete_mailbox failed", self);
		dbmail_imap_session_printf(self,"%s NO DELETE failed\r\n", self->tag);
		return DM_EGENERAL;
	}

	/* check if this was the currently selected mailbox */
	if (self->mailbox && self->mailbox->info && (mboxid == self->mailbox->info->uid)) 
		dbmail_imap_session_set_state(self, IMAPCS_AUTHENTICATED);

	dbmail_imap_session_printf(self, "%s OK DELETE completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_rename()
 *
 * renames a specified mailbox
 */

static int dbmail_imap_session_mailbox_rename(ImapSession *self, u64_t mailbox_id, const char *newname)
{
	MailboxInfo *mb;
	if (! (mb = dbmail_imap_session_mbxinfo_lookup(self, mailbox_id)))
		return DM_EGENERAL;
	g_free(mb->name);
	mb->name = g_strdup(newname);
	if ( (db_setmailboxname(mailbox_id, newname)) == DM_EQUERY)
		return DM_EQUERY;
	return DM_SUCCESS;
}

int _ic_rename(ImapSession *self)
{
	GList *children = NULL;
	u64_t mboxid, newmboxid;
	u64_t parentmboxid = 0;
	size_t oldnamelen;
	int i, result;
	char newname[IMAP_MAX_MAILBOX_NAMELEN];
	MailboxInfo *mb;

	if (!check_state_and_args(self, "RENAME", 2, 2, IMAPCS_AUTHENTICATED))
		return 1;

	if ((mboxid = dbmail_imap_session_mailbox_get_idnr(self, self->args[0])) == 0) {
		dbmail_imap_session_printf(self, "%s NO mailbox does not exist\r\n", self->tag);
		return 1;
	}

	/* check if new name is valid */
        if (!checkmailboxname(self->args[1])) {
	        dbmail_imap_session_printf(self, "%s NO new mailbox name contains invalid characters\r\n", self->tag);
                return 1;
        }

	if ((newmboxid = dbmail_imap_session_mailbox_get_idnr(self, self->args[1])) != 0) {
		dbmail_imap_session_printf(self, "%s NO new mailbox already exists\r\n", self->tag);
		return 1;
	}

	oldnamelen = strlen(self->args[0]);

	/* check if new name would invade structure as in
	 * test (exists)
	 * rename test test/testing
	 * would create test/testing but delete test
	 */
	if (strncasecmp(self->args[0], self->args[1], (int) oldnamelen) == 0 &&
	    strlen(self->args[1]) > oldnamelen && self->args[1][oldnamelen] == '/') {
		dbmail_imap_session_printf(self,
			"%s NO new mailbox would invade mailbox structure\r\n",
			self->tag);
		return 1;
	}

	/* check if structure of new name is valid */
	/* i.e. only last part (after last '/' can be nonexistent) */
	for (i = strlen(self->args[1]) - 1; i >= 0 && self->args[1][i] != '/'; i--);
	if (i >= 0) {
		self->args[1][i] = '\0';	/* note: original char was '/' */

		if (! db_findmailbox(self->args[1], self->userid, &parentmboxid)) {
			/* parent mailbox does not exist */
			dbmail_imap_session_printf(self, "%s NO new mailbox would invade mailbox structure\r\n", self->tag);
			return 1;
		}

		/* ok, reset arg */
		self->args[1][i] = '/';
	}

	/* Check if the user has ACL delete rights to old name, 
	 * and create rights to the parent of the new name, or
	 * if the user just owns both mailboxes. */
	TRACE(TRACE_DEBUG, "[%p] Checking right to DELETE [%llu]", self, mboxid);
	result = dbmail_imap_session_mailbox_check_acl(self, mboxid, ACL_RIGHT_DELETE);
	if (result != 0)
		return result;
	TRACE(TRACE_DEBUG, "[%p] We have the right to DELETE [%llu]", self, mboxid);
	if (!parentmboxid) {
		TRACE(TRACE_DEBUG, "[%p] Destination is a top-level mailbox; not checking right to CREATE.", self);
	} else {
		TRACE(TRACE_DEBUG, "[%p] Checking right to CREATE under [%llu]", self, parentmboxid);
		result = dbmail_imap_session_mailbox_check_acl(self, parentmboxid, ACL_RIGHT_CREATE);
		if (result != 0)
			return result;
		TRACE(TRACE_DEBUG, "[%p] We have the right to CREATE under [%llu]", self, parentmboxid);
	}

	/* check if it is INBOX to be renamed */
	if (strcasecmp(self->args[0], "inbox") == 0) {
		/* ok, renaming inbox */
		/* this means creating a new mailbox and moving all the INBOX msgs to the new mailbox */
		/* inferior names of INBOX are left unchanged */
		result = db_createmailbox(self->args[1], self->userid, &newmboxid);
		if (result == -1) {
			dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
			return -1;
		}

		result = db_movemsg(newmboxid, mboxid);
		if (result == -1) {
			dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
			return -1;
		}

		/* ok done */
		dbmail_imap_session_printf(self, "%s OK RENAME completed\r\n", self->tag);
		return 0;
	}

	/* check for inferior names */
	result = db_listmailboxchildren(mboxid, self->userid, &children);
	if (result == -1) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		return -1;
	}

	/* replace name for each child */
	children = g_list_first(children);

	while (children) {
		u64_t childid = *(u64_t *)children->data;
		mb = dbmail_imap_session_mbxinfo_lookup(self, childid);

		g_snprintf(newname, IMAP_MAX_MAILBOX_NAMELEN, "%s%s", self->args[1], &mb->name[oldnamelen]);
		if ((dbmail_imap_session_mailbox_rename(self, childid, newname)) != DM_SUCCESS) {
			dbmail_imap_session_printf(self, "* BYE error renaming mailbox\r\n");
			g_list_destroy(children);
			return DM_EGENERAL;
		}
		if (! g_list_next(children)) break;
		children = g_list_next(children);
	}

	if (children)
		g_list_destroy(children);

	/* now replace name */
	if ((dbmail_imap_session_mailbox_rename(self, mboxid, self->args[1])) != DM_SUCCESS) {
		dbmail_imap_session_printf(self, "* BYE error renaming mailbox\r\n");
		return DM_EGENERAL;
	}

	dbmail_imap_session_printf(self, "%s OK RENAME completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_subscribe()
 *
 * subscribe to a specified mailbox
 */
int _ic_subscribe(ImapSession *self)
{
	u64_t mboxid;

	if (!check_state_and_args(self, "SUBSCRIBE", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;

	if (! (mboxid = dbmail_imap_session_mailbox_get_idnr(self, self->args[0]))) {
		dbmail_imap_session_printf(self, "%s OK UNSUBSCRIBE on mailbox that does not exist\r\n", self->tag);
		return 0;
	}

	/* check for the lookup-right. RFC is unclear about which right to
	   use, so I guessed it should be lookup */

	if (dbmail_imap_session_mailbox_check_acl(self, mboxid, ACL_RIGHT_LOOKUP))
		return 1;

	if (db_subscribe(mboxid, self->userid) == -1) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "%s OK SUBSCRIBE completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_unsubscribe()
 *
 * removes a mailbox from the users' subscription list
 */
int _ic_unsubscribe(ImapSession *self)
{
	u64_t mboxid;

	if (!check_state_and_args(self, "UNSUBSCRIBE", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;

	if (! (mboxid = dbmail_imap_session_mailbox_get_idnr(self, self->args[0]))) {
		dbmail_imap_session_printf(self, "%s OK UNSUBSCRIBE on mailbox that does not exist\r\n", self->tag);
		return 0;
	}

	/* check for the lookup-right. RFC is unclear about which right to
	   use, so I guessed it should be lookup */
	
	if (dbmail_imap_session_mailbox_check_acl(self, mboxid, ACL_RIGHT_LOOKUP))
		return 1;

	if (db_unsubscribe(mboxid, self->userid) == -1) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "%s OK UNSUBSCRIBE completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_list()
 *
 * executes a list command
 */
int _ic_list(ImapSession *self)
{
	GList *children = NULL;
	int result;
	size_t slen;
	unsigned i;
	char *pattern;
	char *thisname = list_is_lsub ? "LSUB" : "LIST";
	
	MailboxInfo *mb = NULL;
	GList * plist = NULL;
	gchar * pstring;


	if (!check_state_and_args(self, thisname, 2, 2, IMAPCS_AUTHENTICATED))
		return 1;

	/* check if self->args are both empty strings, i.e. A001 LIST "" "" 
	   this has special meaning; show root & delimiter */
	if (strlen(self->args[0]) == 0 && strlen(self->args[1]) == 0) {
		dbmail_imap_session_printf(self, "* %s (\\NoSelect) \"/\" \"\"\r\n",
			thisname);
		dbmail_imap_session_printf(self, "%s OK %s completed\r\n", self->tag, thisname);
		return 0;
	}

	/* check the reference name, should contain only accepted mailboxname chars */
	for (i = 0, slen = strlen(AcceptedMailboxnameChars); self->args[0][i]; i++) {
		if (index(AcceptedMailboxnameChars, self->args[0][i]) == NULL) {
			/* wrong char found */
			dbmail_imap_session_printf(self,
				"%s BAD reference name contains invalid characters\r\n",
				self->tag);
			return 1;
		}
	}
	pattern = g_strdup_printf("%s%s", self->args[0], self->args[1]);

	TRACE(TRACE_INFO, "[%p] search with pattern: [%s]", self, pattern);
	
	result = db_findmailbox_by_regex(self->userid, pattern, &children, list_is_lsub);
	if (result == -1) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		g_list_destroy(children);
		g_free(pattern);
		return -1;
	}

	if (result == 1) {
		dbmail_imap_session_printf(self, "%s BAD invalid pattern specified\r\n", self->tag);
		g_list_destroy(children);
		g_free(pattern);
		return 1;
	}

	children = g_list_first(children);

	while (children) {
		u64_t mailbox_id = *(u64_t *)children->data;
		mb = dbmail_imap_session_mbxinfo_lookup(self, mailbox_id);
		
		plist = NULL;
		if (mb->no_select)
			plist = g_list_append(plist, g_strdup("\\noselect"));
		if (mb->no_inferiors)
			plist = g_list_append(plist, g_strdup("\\noinferiors"));
		if (mb->no_children)
			plist = g_list_append(plist, g_strdup("\\hasnochildren"));
		else
			plist = g_list_append(plist, g_strdup("\\haschildren"));
		
		/* show */
		pstring = dbmail_imap_plist_as_string(plist);
		dbmail_imap_session_printf(self, "* %s %s \"%s\" \"%s\"\r\n", thisname, 
				pstring, MAILBOX_SEPARATOR, mb->name);
		
		g_list_destroy(plist);
		g_free(pstring);

		if (! g_list_next(children)) break;
		children = g_list_next(children);
	}


	if (children)
		g_list_destroy(children);

	g_free(pattern);

	dbmail_imap_session_printf(self, "%s OK %s completed\r\n", self->tag, thisname);

	return 0;
}


/*
 * _ic_lsub()
 *
 * list subscribed mailboxes
 */
int _ic_lsub(ImapSession *self)
{
	int result;

	list_is_lsub = 1;
	result = _ic_list(self);
	list_is_lsub = 0;
	return result;
}


/*
 * _ic_status()
 *
 * inquire the status of a mailbox
 */
int _ic_status(ImapSession *self)
{
	MailboxInfo *mb;
	u64_t id;
	int i, endfound, result;
	GString *response;
	GList *plst = NULL;
	gchar *pstring, *astring;
	
	
	if (!check_state_and_args(self, "STATUS", 3, 0, IMAPCS_AUTHENTICATED))
		return 1;

	if (strcmp(self->args[1], "(") != 0) {
		dbmail_imap_session_printf(self, "%s BAD argument list should be parenthesed\r\n", self->tag);
		return 1;
	}

	/* check final arg: should be ')' and no new '(' in between */
	for (i = 2, endfound = 0; self->args[i]; i++) {
		if (strcmp(self->args[i], ")") == 0) {
			endfound = i;
			break;
		} else if (strcmp(self->args[i], "(") == 0) {
			dbmail_imap_session_printf(self, "%s BAD too many parentheses specified\r\n", self->tag);
			return 1;
		}
	}

	if (endfound == 2) {
		dbmail_imap_session_printf(self, "%s BAD argument list empty\r\n", self->tag);
		return 1;
	}
	if (self->args[endfound + 1]) {
		dbmail_imap_session_printf(self, "%s BAD argument list too long\r\n", self->tag);
		return 1;
	}

	/* check if mailbox exists */
	if (! db_findmailbox(self->args[0], self->userid, &id)) {
		dbmail_imap_session_printf(self, "%s NO specified mailbox does not exist\r\n", self->tag);
		return 1;
	}

	mb = dbmail_imap_session_mbxinfo_lookup(self, id);

	if (mb == NULL) {
		/* mailbox does not exist */
		dbmail_imap_session_printf(self, "%s NO specified mailbox does not exist\r\n", self->tag);
		return 1;
	}

	result = acl_has_right(mb, self->userid, ACL_RIGHT_READ);
	if (result == -1) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}
	if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO no rights to get status for mailbox\r\n", self->tag);
		return 1;
	}

	for (i = 2; self->args[i]; i++) {
		if (strcasecmp(self->args[i], "messages") == 0)
			plst = g_list_append_printf(plst,"MESSAGES %u", mb->exists);
		else if (strcasecmp(self->args[i], "recent") == 0)
			plst = g_list_append_printf(plst,"RECENT %u", mb->recent);
		else if (strcasecmp(self->args[i], "unseen") == 0)
			plst = g_list_append_printf(plst,"UNSEEN %u", mb->unseen);
		else if (strcasecmp(self->args[i], "uidnext") == 0)
			plst = g_list_append_printf(plst,"UIDNEXT %llu", mb->msguidnext);
		else if (strcasecmp(self->args[i], "uidvalidity") == 0)
			plst = g_list_append_printf(plst,"UIDVALIDITY %llu", mb->uid);
		else if (strcasecmp(self->args[i], ")") == 0)
			break;
		else {
			dbmail_imap_session_printf(self,
				"\r\n%s BAD unrecognized option '%s' specified\r\n",
				self->tag, self->args[i]);
			return 1;
		}
	}
	astring = dbmail_imap_astring_as_string(self->args[0]);
	pstring = dbmail_imap_plist_as_string(plst); 

	response = g_string_new("");
	g_string_printf(response, "* STATUS %s %s", astring, pstring);	
	dbmail_imap_session_printf(self, "%s\r\n", response->str);
	dbmail_imap_session_printf(self, "%s OK STATUS completed\r\n", self->tag);

	g_list_destroy(plst);
	g_string_free(response,TRUE);
	g_free(astring);
	g_free(pstring);

	return 0;
}

/*
 * _ic_idle
 *
 * non-expunging close for select mailbox and return to AUTH state
 *
 */

int _ic_idle(ImapSession *self)
{
	int result;
	if (!check_state_and_args(self, "IDLE", 0, 0, IMAPCS_AUTHENTICATED))
		return 1;	/* error, return */

	if ((result = dbmail_imap_session_idle(self)) != 0)
		return result;

	return 0;
}



/*
 * _ic_append()
 *
 * append a message to a mailbox
 */
int _ic_append(ImapSession *self)
{
	u64_t mboxid;
	u64_t msg_idnr;
	int i, j, result;
	timestring_t sqldate;
	int flaglist[IMAP_NFLAGS];
	int flagcount = 0;
	GList *keywords = NULL;
	MailboxInfo *mbx = NULL;
	u64_t rfcsize = 0;

	memset(flaglist,0,sizeof(flaglist));

	if (!self->args[0] || !self->args[1]) {
		dbmail_imap_session_printf(self, "%s BAD invalid arguments specified to APPEND\r\n",
			self->tag);
		return 1;
	}

	/* find the mailbox to place the message */
	if (! db_findmailbox(self->args[0], self->userid, &mboxid)) {
		dbmail_imap_session_printf(self, "%s NO [TRYCREATE] could not find specified mailbox\r\n", self->tag);
		return 1;
	}

	mbx = dbmail_imap_session_mbxinfo_lookup(self, mboxid);

	/* check if user has right to append to  mailbox */
	result = acl_has_right(mbx, self->userid, ACL_RIGHT_INSERT);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}
	if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO no permission to append to mailbox\r\n",
				self->tag);
		dbmail_imap_session_set_state(self, IMAPCS_AUTHENTICATED);
		return 1;
	}

	i = 1;

	/* check if a flag list has been specified */
	if (self->args[i][0] == '(') {
		/* ok fetch the flags specified */
		TRACE(TRACE_DEBUG, "[%p] flag list found:", self);

		i++;
		while (self->args[i] && self->args[i][0] != ')') {
			TRACE(TRACE_DEBUG, "[%p] [%s]", self, self->args[i]);
			for (j = 0; j < IMAP_NFLAGS; j++) {
				if (strcasecmp (self->args[i], imap_flag_desc_escaped[j]) == 0) {
					flaglist[j] = 1;
					flagcount++;
					break;
				}
			}
			if (j == IMAP_NFLAGS) {
				TRACE(TRACE_DEBUG,"[%p] found keyword [%s]", self, self->args[i]);
				keywords = g_list_append(keywords,g_strdup(self->args[i]));
				flagcount++;
			}

			i++;
		}

		i++;
		TRACE(TRACE_DEBUG, "[%p] )", self);
	}

	if (!self->args[i]) {
		TRACE(TRACE_INFO, "[%p] unexpected end of arguments", self);
		dbmail_imap_session_printf(self, 
				"%s BAD invalid arguments specified to APPEND\r\n", 
				self->tag);
		return 1;
	}

	/** check ACL's for STORE */
	if (flaglist[IMAP_FLAG_SEEN] == 1) {
		result = acl_has_right(mbx, self->userid, ACL_RIGHT_SEEN);
		if (result < 0) {
			dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
			return -1;	/* fatal */
		}
		if (result == 0) {
			dbmail_imap_session_printf(self, "%s NO no right to store \\SEEN flag\r\n", self->tag);
			return 1;
		}
	}
	if (flaglist[IMAP_FLAG_DELETED] == 1) {
		result = acl_has_right(mbx, self->userid, ACL_RIGHT_DELETE);
		if (result < 0) {
			dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
			return -1;	/* fatal */
		}
		if (result == 0) {
			dbmail_imap_session_printf(self, "%s NO no right to store \\DELETED flag\r\n", self->tag);
			return 1;
		}
	}
	if (flaglist[IMAP_FLAG_ANSWERED] == 1 ||
	    flaglist[IMAP_FLAG_FLAGGED] == 1 ||
	    flaglist[IMAP_FLAG_DRAFT] == 1 ||
	    flaglist[IMAP_FLAG_RECENT] == 1 ||
	    g_list_length(keywords) > 0) {
		result = acl_has_right(mbx, self->userid, ACL_RIGHT_WRITE);
		if (result < 0) {
			dbmail_imap_session_printf(self, "*BYE internal database error\r\n");
			return -1;
		}
		if (result == 0) {
			dbmail_imap_session_printf(self, "%s NO no right to store flags\r\n", self->tag);
			return 1;
		}
	}


	/* there could be a literal date here, check if the next argument exists
	 * if so, assume this is the literal date.
	 */
	if (self->args[i + 1]) {
		struct tm tm;
		char *dt = self->args[i];

		memset(&tm, 0, sizeof(struct tm));

		dt = g_strstrip(dt);

		if (strptime(dt, "%d-%b-%Y %T", &tm) != NULL)
			strftime(sqldate, sizeof(sqldate), "%Y-%m-%d %H:%M:%S", &tm);
		else
			sqldate[0] = '\0';
		/* internal date specified */

		i++;
		TRACE(TRACE_DEBUG, "[%p] internal date [%s] found, next arg [%s]", self, sqldate, self->args[i]);
	} else {
		sqldate[0] = '\0';
	}

	/* ok literal msg should be in self->args[i] */
	/* insert this msg */

	rfcsize = strlen(self->args[i]);
	result = db_imap_append_msg(self->args[i], rfcsize, mboxid, self->userid, sqldate, &msg_idnr);

	switch (result) {
	case -1:
		TRACE(TRACE_ERROR, "[%p] error appending msg", self);
		dbmail_imap_session_printf(self, "* BYE internal dbase error storing message\r\n");
		break;

	case -2:
		TRACE(TRACE_INFO, "[%p] quotum would exceed", self);
		dbmail_imap_session_printf(self, "%s NO not enough quotum left\r\n", self->tag);
		break;

	case FALSE:
		TRACE(TRACE_ERROR, "[%p] faulty msg", self);
		dbmail_imap_session_printf(self, "%s NO invalid message specified\r\n", self->tag);
		break;

	case TRUE:
		dbmail_imap_session_printf(self, "%s OK APPEND completed\r\n", self->tag);
		break;
	}

	if (result == TRUE && flagcount > 0) {
		if (db_set_msgflag(msg_idnr, mboxid, flaglist, keywords, IMAPFA_ADD, NULL) < 0) {
			TRACE(TRACE_ERROR, "[%p] error setting flags for message [%llu]", self, msg_idnr);
			g_list_destroy(keywords);
			return -1;
		}
	}
	
	

	g_list_destroy(keywords);

	db_mailbox_mtime_update(mboxid);

	if (self->state == IMAPCS_SELECTED) {

		u64_t *uid;
		//insert new Messageinfo struct into self->mailbox->msginfo
		
		MessageInfo *msginfo = g_new0(MessageInfo,1);

		/* id */
		msginfo->id = msg_idnr;

		/* mailbox_id */
		msginfo->mailbox_id = mboxid;

		/* flags */
		for (j = 0; j < IMAP_NFLAGS; j++)
			msginfo->flags[j] = flaglist[j];

		/* internal date */
		strncpy(msginfo->internaldate, (sqldate) ? sqldate : "01-Jan-1970 00:00:01 +0100", IMAP_INTERNALDATE_LEN);

		/* rfcsize */
		msginfo->rfcsize = rfcsize;

		uid = g_new0(u64_t,1);
		*uid = msg_idnr;
		g_tree_insert(self->mailbox->msginfo, uid, msginfo); 

		self->mailbox->info->exists++;

		dbmail_mailbox_insert_uid(self->mailbox, msg_idnr);

		dbmail_imap_session_mailbox_status(self, FALSE);

	}

	return 0;
}



/* 
 * SELECTED-STATE COMMANDS 
 * sort, check, close, expunge, search, fetch, store, copy, uid
 */

/*
 * _ic_check()
 * 
 * request a checkpoint for the selected mailbox
 * (equivalent to NOOP)
 */
int _ic_check(ImapSession *self)
{
	int result;

	if (!check_state_and_args(self, "CHECK", 0, 0, IMAPCS_SELECTED))
		return 1;	/* error, return */

	result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_READ);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE Internal database error\r\n");
		return -1;
	}
	if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO no permission to do check on "
			"mailbox\r\n", self->tag);
		return 1;
	}

	dbmail_imap_session_mailbox_status(self, TRUE);

	dbmail_imap_session_printf(self, "%s OK CHECK completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_close()
 *
 * expunge deleted messages from selected mailbox & return to AUTH state
 * do not show expunge-output
 */
int _ic_close(ImapSession *self)
{
	int result;

	if (!check_state_and_args(self, "CLOSE", 0, 0, IMAPCS_SELECTED))
		return 1;	/* error, return */

	/* check if the user has to right to expunge all messages from the
	   mailbox. */
	result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_DELETE);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE Internal database error\r\n");
		return -1;
	}
	/* only perform the expunge if the user has the right to do it */
	if (result == 1)
		if (self->mailbox->info->permission == IMAPPERM_READWRITE)
			dbmail_imap_session_mailbox_expunge(self);


	/* ok, update state (always go to IMAPCS_AUTHENTICATED) */
	dbmail_imap_session_set_state(self, IMAPCS_AUTHENTICATED);
	dbmail_imap_session_printf(self, "%s OK CLOSE completed\r\n", self->tag);
	return 0;
}

/*
 * _ic_unselect
 *
 * non-expunging close for select mailbox and return to AUTH state
 *
 */

int _ic_unselect(ImapSession *self)
{
	if (!check_state_and_args(self, "UNSELECT", 0, 0, IMAPCS_SELECTED))
		return 1;	/* error, return */

	dbmail_imap_session_mailbox_close(self);
	dbmail_imap_session_printf(self, "%s OK UNSELECT completed\r\n", self->tag);
	return 0;
}

/*
 * _ic_expunge()
 *
 * expunge deleted messages from selected mailbox
 * show expunge output per message
 */
	
int _ic_expunge(ImapSession *self)
{
	int result;

	if (!check_state_and_args(self, "EXPUNGE", 0, 0, IMAPCS_SELECTED))
		return 1; /* error, return */

	if (self->mailbox->info->permission != IMAPPERM_READWRITE) {
		dbmail_imap_session_printf(self,
			"%s NO you do not have write permission on this folder\r\n",
			self->tag);
		return 1;
	}

	result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_DELETE);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}
	if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO you do not have delete rights on this mailbox\r\n", 
		self->tag);
		return 1;
	}
	if (dbmail_imap_session_mailbox_expunge(self) != DM_SUCCESS) {
		dbmail_imap_session_printf(self, "* BYE expunge failed\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "%s OK EXPUNGE completed\r\n", self->tag);
	return 0;
}


/*
 * _ic_search()
 *
 * search the selected mailbox for messages
 *
 */

static int sorted_search(ImapSession *self, search_order_t order)
{
	DbmailMailbox *mb;
	int result = 0;
	gchar *s = NULL;
	const gchar *cmd;
	gboolean sorted;

	if (order == SEARCH_SORTED)
		sorted = 1;
	
	if (self->state != IMAPCS_SELECTED) {
		dbmail_imap_session_printf(self,
			"%s BAD %s command received in invalid state\r\n",
			self->tag, self->command);
		return 1;
	}
	
	if (!self->args[0]) {
		dbmail_imap_session_printf(self, "%s BAD invalid arguments to %s\r\n",
			self->tag, self->command);
		return 1;
	}
	
	/* check ACL */
	if (! (result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_READ))) {
		dbmail_imap_session_printf(self, "%s NO no permission to search mailbox\r\n", self->tag);
		return 1;
	}
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}

	mb = self->mailbox;
	switch(order) {
		case SEARCH_SORTED:
			cmd = "SORT";
			break;
		case SEARCH_UNORDERED:
			cmd = "SEARCH";
			break;
		case SEARCH_THREAD_REFERENCES:
		case SEARCH_THREAD_ORDEREDSUBJECT:
			cmd = "THREAD";
			break;
		default:// shouldn't happen
			cmd = "NO";
			break;
	}
	if (g_tree_nnodes(mb->ids) > 0) {
		dbmail_mailbox_set_uid(mb,self->use_uid);

		if (dbmail_mailbox_build_imap_search(mb, self->args, &(self->args_idx), order) < 0) {
			dbmail_imap_session_printf(self, "%s BAD invalid arguments to %s\r\n",
				self->tag, cmd);
			return 1;
		}
		dbmail_mailbox_search(mb);
		/* ok, display results */
		switch(order) {
			case SEARCH_SORTED:
				dbmail_mailbox_sort(mb);
				s = dbmail_mailbox_sorted_as_string(mb);
			break;
			case SEARCH_UNORDERED:
				s = dbmail_mailbox_ids_as_string(mb);
			break;
			case SEARCH_THREAD_ORDEREDSUBJECT:
				s = dbmail_mailbox_orderedsubject(mb);
			break;
			case SEARCH_THREAD_REFERENCES:
				s = NULL; // TODO: unsupported
			break;
		}

	}

	if (s) {
		dbmail_imap_session_printf(self, "* %s %s\r\n", cmd, s);
		g_free(s);
	} else {
		dbmail_imap_session_printf(self, "* %s\r\n", cmd);
	}

	dbmail_imap_session_printf(self, "%s OK %s completed\r\n", self->tag, cmd);
	
	return 0;
}

int _ic_search(ImapSession *self)
{
	return sorted_search(self,SEARCH_UNORDERED);
}

int _ic_sort(ImapSession *self)
{
	return sorted_search(self,SEARCH_SORTED);
}

int _ic_thread(ImapSession *self)
{
	if (MATCH(self->args[0],"ORDEREDSUBJECT"))
		return sorted_search(self,SEARCH_THREAD_ORDEREDSUBJECT);
	if (MATCH(self->args[0],"REFERENCES"))
		dbmail_imap_session_printf(self, "%s BAD THREAD=REFERENCES not supported\r\n",self->tag);
		//return sorted_search(self,SEARCH_THREAD_REFERENCES);

	return 1;
}

int _dm_imapsession_get_ids(ImapSession *self, const char *set)
{
	dbmail_mailbox_set_uid(self->mailbox,self->use_uid);

	if (self->ids) {
		g_tree_destroy(self->ids);
		self->ids = NULL;
	}

	self->ids = dbmail_mailbox_get_set(self->mailbox, set, self->use_uid);

	if ( (!self->ids) || (g_tree_nnodes(self->ids)==0) ) {
		dbmail_imap_session_printf(self, "%s BAD invalid message range specified\r\n", self->tag);
		return DM_EGENERAL;
	}

	return DM_SUCCESS;
}



/*
 * _ic_fetch()
 *
 * fetch message(s) from the selected mailbox
 */
	
int _ic_fetch(ImapSession *self)
{
	int result, state, setidx;

	if (!check_state_and_args (self, "FETCH", 2, 0, IMAPCS_SELECTED))
		return 1;

	/* check if the user has the right to fetch messages in this mailbox */
	result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_READ);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}
	if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO no permission to fetch from mailbox\r\n", self->tag);
		return 1;
	}
	
	dbmail_imap_session_resetFi(self);

	self->fi->getUID = self->use_uid;

	setidx = self->args_idx;
	self->args_idx++; //skip on past this for the fetch_parse_args coming next...

	state = 1;
	do {
		if ( (state = dbmail_imap_session_fetch_parse_args(self)) == -2) {
			dbmail_imap_session_printf(self, "%s BAD invalid argument list to fetch\r\n", self->tag);
			return 1;
		}
		TRACE(TRACE_DEBUG,"[%p] dbmail_imap_session_fetch_parse_args loop idx %llu state %d ", self, self->args_idx, state);
		self->args_idx++;
	} while (state > 0);

	result = DM_SUCCESS;

  	if (g_tree_nnodes(self->mailbox->ids) > 0) {
 		if ((result = _dm_imapsession_get_ids(self, self->args[setidx])) == DM_SUCCESS) {
  			self->ids_list = g_tree_keys(self->ids);
  			result = dbmail_imap_session_fetch_get_items(self);
  		}
	}

	dbmail_imap_session_fetch_free(self);

	if (result == DM_SUCCESS)
		dbmail_imap_session_printf(self, "%s OK %sFETCH completed\r\n", self->tag, self->use_uid ? "UID " : "");

	return result;
}



/*
 * _ic_store()
 *
 * alter message-associated data in selected mailbox
 */

static gboolean _do_store(u64_t *id, gpointer UNUSED value, ImapSession *self)
{
	cmd_store_t *cmd = (cmd_store_t *)self->cmd;

	u64_t *msn;
	MessageInfo *msginfo = NULL;
	char *s;
	int i;

	if (self->mailbox && self->mailbox->msginfo)
		msginfo = g_tree_lookup(self->mailbox->msginfo, id);

	if (! msginfo) {
		TRACE(TRACE_WARNING, "[%p] unable to lookup msginfo struct for [%llu]", self, *id);
		return TRUE;
	}

	msn = g_tree_lookup(self->mailbox->ids, id);

	if (self->mailbox->info->permission == IMAPPERM_READWRITE) {
		if (db_set_msgflag(*id, self->mailbox->info->uid, cmd->flaglist, cmd->keywords, cmd->action, msginfo) < 0) {
			dbmail_imap_session_printf(self, "\r\n* BYE internal dbase error\r\n");
			return TRUE;
		}
	}

	// Set the system flags
	for (i = 0; i < IMAP_NFLAGS; i++) {
		
		if (i == IMAP_FLAG_RECENT) // Skip recent_flag because it is already part of the query.
			continue;

		switch (cmd->action) {
			case IMAPFA_ADD:
				if (cmd->flaglist[i])
					msginfo->flags[i] = 1;
			break;
			case IMAPFA_REMOVE:
				if (cmd->flaglist[i]) 
					msginfo->flags[i] = 0;
			break;
			case IMAPFA_REPLACE:
				if (cmd->flaglist[i]) 
					msginfo->flags[i] = 1;
				else
					msginfo->flags[i] = 0;
			break;
		}
	}

	// Set the user keywords as labels
	g_list_merge(&(msginfo->keywords), cmd->keywords, cmd->action, (GCompareFunc)g_ascii_strcasecmp);

	// reporting callback
	if (! cmd->silent) {
		s = imap_flags_as_string(msginfo);
		dbmail_imap_session_printf(self,"* %llu FETCH (FLAGS %s)\r\n", *msn, s);
		g_free(s);
	}

	return FALSE;
}

int _ic_store(ImapSession *self)
{
	cmd_store_t *cmd;
	int result, i, j, k;

	cmd = g_new0(cmd_store_t,1);

	if (!check_state_and_args (self, "STORE", 2, 0, IMAPCS_SELECTED))
		return 1;

	k = self->args_idx;
	/* multiple flags should be parenthesed */
	if (self->args[k+3] && strcmp(self->args[k+2], "(") != 0) {
		dbmail_imap_session_printf(self, "%s BAD invalid argument(s) to STORE\r\n",
			self->tag);
		return 1;
	}

	cmd->silent = FALSE;

	/* retrieve action type */
	if (MATCH(self->args[k+1], "flags"))
		cmd->action = IMAPFA_REPLACE;
	else if (MATCH(self->args[k+1], "flags.silent")) {
		cmd->action = IMAPFA_REPLACE;
		cmd->silent = TRUE;
	} else if (MATCH(self->args[k+1], "+flags"))
		cmd->action = IMAPFA_ADD;
	else if (MATCH(self->args[k+1], "+flags.silent")) {
		cmd->action = IMAPFA_ADD;
		cmd->silent = TRUE;
	} else if (MATCH(self->args[k+1], "-flags"))
		cmd->action = IMAPFA_REMOVE;
	else if (MATCH(self->args[k+1], "-flags.silent")) {
		cmd->action = IMAPFA_REMOVE;
		cmd->silent = TRUE;
	}

	if (cmd->action == IMAPFA_NONE) {
		dbmail_imap_session_printf(self, "%s BAD invalid STORE action specified\r\n", self->tag);
		return 1;
	}

	/* now fetch flag list */
	i = (strcmp(self->args[k+2], "(") == 0) ? 3 : 2;

	for (; self->args[k+i] && strcmp(self->args[k+i], ")") != 0; i++) {
		for (j = 0; j < IMAP_NFLAGS; j++) {
			/* storing the recent flag explicitely is not allowed */
			if (MATCH(self->args[k+i],"\\Recent")) {
				dbmail_imap_session_printf(self, "%s BAD invalid flag list to STORE command\r\n", self->tag);
				return 1;
			}
				
			if (MATCH(self->args[k+i], imap_flag_desc_escaped[j])) {
				cmd->flaglist[j] = 1;
				break;
			}
		}

		if (j == IMAP_NFLAGS)
			cmd->keywords = g_list_append(cmd->keywords,g_strdup(self->args[k+i]));
	}

	/** check ACL's for STORE */
	if (cmd->flaglist[IMAP_FLAG_SEEN] == 1) {
		result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_SEEN);
		if (result < 0) {
			dbmail_imap_session_printf(self, "* BYE internal database error");
			return -1;	/* fatal */
		}
		if (result == 0) {
			dbmail_imap_session_printf(self, "%s NO no right to store \\SEEN flag\r\n", self->tag);
			return 1;
		}
	}
	if (cmd->flaglist[IMAP_FLAG_DELETED] == 1) {
		result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_DELETE);
		if (result < 0) {
			dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
			return -1;	/* fatal */
		}
		if (result == 0) {
			dbmail_imap_session_printf(self, "%s NO no right to store \\DELETED flag\r\n", self->tag);
			return 1;
		}
	}
	if (cmd->flaglist[IMAP_FLAG_ANSWERED] == 1 ||
	    cmd->flaglist[IMAP_FLAG_FLAGGED] == 1 ||
	    cmd->flaglist[IMAP_FLAG_DRAFT] == 1 ||
	    g_list_length(cmd->keywords) > 0 ) {
		result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_WRITE);
		if (result < 0) {
			dbmail_imap_session_printf(self, "* BYE internal database error");
			return -1;
		}
		if (result == 0) {
			dbmail_imap_session_printf(self, "%s NO no right to store flags", self->tag);
			return 1;
		}
	}
	/* end of ACL checking. If we get here without returning, the user has
	   the right to store the flags */

	self->cmd = cmd;
	
	result = DM_SUCCESS;

  	if (g_tree_nnodes(self->mailbox->ids) > 0) {
 		if ((result = _dm_imapsession_get_ids(self, self->args[k])) == DM_SUCCESS)
 			g_tree_foreach(self->ids, (GTraverseFunc) _do_store, self);
  	}	

	if (result == DM_SUCCESS)
		dbmail_imap_session_printf(self, "%s OK %sSTORE completed\r\n", self->tag, self->use_uid ? "UID " : "");

	return result;
}


/*
 * _ic_copy()
 *
 * copy a message to another mailbox
 */

static gboolean _do_copy(u64_t *id, gpointer UNUSED value, ImapSession *self)
{
	cmd_copy_t *cmd = (cmd_copy_t *)self->cmd;
	u64_t newid;
	int result;

	result = db_copymsg(*id, cmd->mailbox_id, self->userid, &newid);
	if (result == -1) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		return TRUE;
	}
	if (result == -2) {
		dbmail_imap_session_printf(self, "%s NO quotum would exceed\r\n", self->tag);
		return TRUE;
	}
	return FALSE;
}


int _ic_copy(ImapSession *self)
{
	u64_t destmboxid;
	int result;
	MailboxInfo *destmbox;
	cmd_copy_t cmd;
	
	if (!check_state_and_args(self, "COPY", 2, 2, IMAPCS_SELECTED))
		return 1;	/* error, return */

	/* check if destination mailbox exists */
	if (! db_findmailbox(self->args[self->args_idx+1], self->userid, &destmboxid)) {
		dbmail_imap_session_printf(self, "%s NO [TRYCREATE] specified mailbox does not exist\r\n",
			self->tag);
		return 1;
	}
	// check if user has right to COPY from source mailbox
	result = acl_has_right(self->mailbox->info, self->userid, ACL_RIGHT_READ);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;	/* fatal */
	}
	if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO no permission to copy from mailbox\r\n",
			self->tag);
		return 1;
	}
	// check if user has right to COPY to destination mailbox
	destmbox = dbmail_imap_session_mbxinfo_lookup(self, destmboxid);

	result = acl_has_right(destmbox, self->userid, ACL_RIGHT_INSERT);
	if (result < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;	/* fatal */
	}
	if (result == 0) {
		dbmail_imap_session_printf(self,
			"%s NO no permission to copy to mailbox\r\n", self->tag);
		return 1;
	}

	cmd.mailbox_id = destmboxid;
	self->cmd = &cmd;

	if (g_tree_nnodes(self->mailbox->ids) > 0) {
 		if ((_dm_imapsession_get_ids(self, self->args[self->args_idx]) == DM_SUCCESS))
 			g_tree_foreach(self->ids, (GTraverseFunc) _do_copy, self);
 		else
  			return DM_EGENERAL;
  	}	
	db_mailbox_mtime_update(destmboxid);

	dbmail_imap_session_printf(self, "%s OK %sCOPY completed\r\n", self->tag,
		self->use_uid ? "UID " : "");
	return 0;
}


/*
 * _ic_uid()
 *
 * fetch/store/copy/search message UID's
 */
int _ic_uid(ImapSession *self)
{
	int result;

	if (self->state != IMAPCS_SELECTED) {
		dbmail_imap_session_printf(self,
			"%s BAD UID command received in invalid state\r\n",
			self->tag);
		return 1;
	}

	if (!self->args[0]) {
		dbmail_imap_session_printf(self, "%s BAD missing argument(s) to UID\r\n",
			self->tag);
		return 1;
	}

	self->use_uid = 1;	/* set global var to make clear we will be using UID's */
	
	/* ACL rights for UID are handled by the other functions called below */
	if (MATCH(self->args[self->args_idx], "fetch")) {
		self->args_idx++; 
		result = _ic_fetch(self);
	} else if (MATCH(self->args[self->args_idx], "copy")) {
		self->args_idx++;
		result = _ic_copy(self);
	} else if (MATCH(self->args[self->args_idx], "store")) {
		self->args_idx++;
		result = _ic_store(self);
	} else if (MATCH(self->args[self->args_idx], "search")) {
		self->args_idx++;
		result = _ic_search(self);
	} else if (MATCH(self->args[self->args_idx], "sort")) {
		self->args_idx++;
		result = _ic_sort(self);
	} else if (MATCH(self->args[self->args_idx], "thread")) {
		self->args_idx++;
		result = _ic_thread(self);
	} else {
		dbmail_imap_session_printf(self, "%s BAD invalid UID command\r\n", self->tag);
		result = 1;
	}

	self->use_uid = 0;

	return result;
}


/* Helper function for _ic_getquotaroot() and _ic_getquota().
 * Send all resource limits in `quota'.
 */
void send_quota(ImapSession *self, quota_t * quota)
{
	int r;
	u64_t usage, limit;
	char *name;

	for (r = 0; r < quota->n_resources; r++) {
		if (quota->resource[r].limit > 0) {
			switch (quota->resource[r].type) {
			case RT_STORAGE:
				name = "STORAGE";
				usage = quota->resource[r].usage / 1024;
				limit = quota->resource[r].limit / 1024;
				break;
			default:
				continue;
			}
			dbmail_imap_session_printf(self,
				"* QUOTA \"%s\" (%s %llu %llu)\r\n",
				quota->root, name, usage, limit);
		}
	}
}

/*
 * _ic_getquotaroot()
 *
 * get quota root and send quota
 */
int _ic_getquotaroot(ImapSession *self)
{
	quota_t *quota;
	char *root, *errormsg;

	if (!check_state_and_args(self, "GETQUOTAROOT", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;	/* error, return */

	root = quota_get_quotaroot(self->userid, self->args[self->args_idx], &errormsg);
	if (root == NULL) {
		dbmail_imap_session_printf(self, "%s NO %s\r\n", self->tag, errormsg);
		return 1;
	}

	quota = quota_get_quota(self->userid, root, &errormsg);
	if (quota == NULL) {
		dbmail_imap_session_printf(self, "%s NO %s\r\n", self->tag, errormsg);
		return 1;
	}

	dbmail_imap_session_printf(self, "* QUOTAROOT \"%s\" \"%s\"\r\n", self->args[self->args_idx],
		quota->root);
	send_quota(self, quota);
	quota_free(quota);

	dbmail_imap_session_printf(self, "%s OK GETQUOTAROOT completed\r\n", self->tag);
	return 0;
}

/*
 * _ic_getquot()
 *
 * get quota
 */
int _ic_getquota(ImapSession *self)
{
	quota_t *quota;
	char *errormsg;

	if (!check_state_and_args(self, "GETQUOTA", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;	/* error, return */

	quota = quota_get_quota(self->userid, self->args[self->args_idx], &errormsg);
	if (quota == NULL) {
		dbmail_imap_session_printf(self, "%s NO %s\r\n", self->tag, errormsg);
		return 1;
	}

	send_quota(self, quota);
	quota_free(quota);

	dbmail_imap_session_printf(self, "%s OK GETQUOTA completed\r\n", self->tag);
	return 0;
}

/* returns -1 on error, 0 if user or mailbox not found and 1 otherwise */
static int imap_acl_pre_administer(const char *mailboxname,
				   const char *username,
				   u64_t executing_userid,
				   u64_t * mboxid, u64_t * target_userid)
{
	int result;
	if (! db_findmailbox(mailboxname, executing_userid, mboxid))
		return FALSE;

	result = auth_user_exists(username, target_userid);
	if (result < 1)
		return result;

	return 1;
}

int _ic_setacl(ImapSession *self)
{
	/* SETACL mailboxname identifier mod_rights */
	int result;
	u64_t mboxid;
	u64_t targetuserid;
	MailboxInfo *mailbox;

	if (!check_state_and_args(self, "SETACL", 3, 3, IMAPCS_AUTHENTICATED))
		return 1;

	result = imap_acl_pre_administer(self->args[self->args_idx], self->args[self->args_idx+1], self->userid,
					 &mboxid, &targetuserid);
	if (result == -1) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	} else if (result == 0) {
		dbmail_imap_session_printf(self, "%s NO SETACL failure: can't set acl\r\n",
			self->tag);
		return 1;
	}
	// has the rights to 'administer' this mailbox? 
	mailbox = dbmail_imap_session_mbxinfo_lookup(self, mboxid);

	if (acl_has_right(mailbox, self->userid, ACL_RIGHT_ADMINISTER) != 1) {
		dbmail_imap_session_printf(self, "%s NO SETACL failure: can't set acl, "
			"you don't have the proper rights\r\n", self->tag);
		return 1;
	}
	// set the new acl
	if (acl_set_rights(targetuserid, mboxid, self->args[self->args_idx+2]) < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "%s OK SETACL completed\r\n", self->tag);
	return 0;
}


int _ic_deleteacl(ImapSession *self)
{
	// DELETEACL mailboxname identifier
	u64_t mboxid;
	u64_t targetuserid;
	MailboxInfo *mailbox;

	if (!check_state_and_args(self, "DELETEACL", 2, 2, IMAPCS_AUTHENTICATED))
		return 1;

	if (imap_acl_pre_administer(self->args[self->args_idx], self->args[self->args_idx+1], self->userid,
				    &mboxid, &targetuserid) == -1) {
		dbmail_imap_session_printf(self, "* BYE internal dbase error\r\n");
		return -1;
	}
	
	mailbox = dbmail_imap_session_mbxinfo_lookup(self, mboxid);

	// has the rights to 'administer' this mailbox? 
	if (acl_has_right(mailbox, self->userid, ACL_RIGHT_ADMINISTER) != 1) {
		dbmail_imap_session_printf(self, "%s NO DELETEACL failure: can't delete "
			"acl\r\n", self->tag);
		return 1;
	}
	// set the new acl
	if (acl_delete_acl(targetuserid, mboxid) < 0) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "%s OK DELETEACL completed\r\n", self->tag);
	return 0;
}

int _ic_getacl(ImapSession *self)
{
	/* GETACL mailboxname */
	u64_t mboxid;
	char *acl_string;

	if (!check_state_and_args(self, "GETACL", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;

	if (! db_findmailbox(self->args[self->args_idx], self->userid, &mboxid)) {
		dbmail_imap_session_printf(self, "%s NO GETACL failure: can't get acl\r\n",
			self->tag);
		return 1;
	}
	// get acl string (string of identifier-rights pairs)
	if (!(acl_string = acl_get_acl(mboxid))) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "* ACL \"%s\" %s\r\n", self->args[self->args_idx], acl_string);
	g_free(acl_string);
	dbmail_imap_session_printf(self, "%s OK GETACL completed\r\n", self->tag);
	return 0;
}

int _ic_listrights(ImapSession *self)
{
	/* LISTRIGHTS mailboxname identifier */
	int result;
	u64_t mboxid;
	u64_t targetuserid;
	char *listrights_string;
	MailboxInfo *mailbox;

	if (!check_state_and_args(self, "LISTRIGHTS", 2, 2, IMAPCS_AUTHENTICATED))
		return 1;

	result = imap_acl_pre_administer(self->args[self->args_idx], self->args[self->args_idx+1], self->userid,
					 &mboxid, &targetuserid);
	if (result == -1) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	} else if (result == 0) {
		dbmail_imap_session_printf(self,
			"%s, NO LISTRIGHTS failure: can't set acl\r\n",
			self->tag);
		return 1;
	}
	// has the rights to 'administer' this mailbox? 
	mailbox = dbmail_imap_session_mbxinfo_lookup(self, mboxid);

	if (acl_has_right(mailbox, self->userid, ACL_RIGHT_ADMINISTER) != 1) {
		dbmail_imap_session_printf(self,
			"%s NO LISTRIGHTS failure: can't set acl\r\n",
			self->tag);
		return 1;
	}
	// set the new acl
	if (!(listrights_string = acl_listrights(targetuserid, mboxid))) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "* LISTRIGHTS \"%s\" %s %s\r\n",
		self->args[self->args_idx], self->args[self->args_idx+1], listrights_string);
	dbmail_imap_session_printf(self, "%s OK LISTRIGHTS completed\r\n", self->tag);
	g_free(listrights_string);
	return 0;
}

int _ic_myrights(ImapSession *self)
{
	/* MYRIGHTS mailboxname */
	u64_t mboxid;
	char *myrights_string;

	if (!check_state_and_args(self, "LISTRIGHTS", 1, 1, IMAPCS_AUTHENTICATED))
		return 1;

	if (! db_findmailbox(self->args[self->args_idx], self->userid, &mboxid)) {
		dbmail_imap_session_printf(self, "%s NO MYRIGHTS failure: unknown mailbox\r\n", self->tag);
		return 1;
	}

	if (!(myrights_string = acl_myrights(self->userid, mboxid))) {
		dbmail_imap_session_printf(self, "* BYE internal database error\r\n");
		return -1;
	}

	dbmail_imap_session_printf(self, "* MYRIGHTS \"%s\" %s\r\n", self->args[self->args_idx],
		myrights_string);
	g_free(myrights_string);
	dbmail_imap_session_printf(self, "%s OK MYRIGHTS complete\r\n", self->tag);
	return 0;
}

int _ic_namespace(ImapSession *self)
{
	/* NAMESPACE command */
	if (!check_state_and_args(self, "NAMESPACE", 0, 0, IMAPCS_AUTHENTICATED))
		return 1;

	dbmail_imap_session_printf(self, "* NAMESPACE ((\"\" \"%s\")) ((\"%s\" \"%s\")) "
		"((\"%s\" \"%s\"))\r\n",
		MAILBOX_SEPARATOR, NAMESPACE_USER,
		MAILBOX_SEPARATOR, NAMESPACE_PUBLIC, MAILBOX_SEPARATOR);
	dbmail_imap_session_printf(self, "%s OK NAMESPACE complete\r\n", self->tag);
	return 0;
}