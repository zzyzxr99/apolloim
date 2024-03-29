/**
 * @file buddy.h Buddy handlers
 *
 * purple
 *
 * Copyright (C) 2003 Nathan Walp <faceprint@faceprint.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifndef _PURPLE_JABBER_BUDDY_H_
#define _PURPLE_JABBER_BUDDY_H_

#include "jabber.h"

typedef enum {
	JABBER_BUDDY_STATE_UNKNOWN = -2,
	JABBER_BUDDY_STATE_ERROR = -1,
	JABBER_BUDDY_STATE_UNAVAILABLE = 0,
	JABBER_BUDDY_STATE_ONLINE,
	JABBER_BUDDY_STATE_CHAT,
	JABBER_BUDDY_STATE_AWAY,
	JABBER_BUDDY_STATE_XA,
	JABBER_BUDDY_STATE_DND
} JabberBuddyState;

typedef struct _JabberBuddy {
	GList *resources;
	char *error_msg;
	enum {
		JABBER_INVISIBLE_NONE   = 0,
		JABBER_INVISIBLE_SERVER = 1 << 1,
		JABBER_INVIS_BUDDY      = 1 << 2
	} invisible;
	enum {
		JABBER_SUB_NONE    = 0,
		JABBER_SUB_PENDING = 1 << 1,
		JABBER_SUB_TO      = 1 << 2,
		JABBER_SUB_FROM    = 1 << 3,
		JABBER_SUB_BOTH    = (JABBER_SUB_TO | JABBER_SUB_FROM),
		JABBER_SUB_REMOVE  = 1 << 4
	} subscription;
} JabberBuddy;

typedef struct _JabberBuddyResource {
	JabberBuddy *jb;
	char *name;
	int priority;
	JabberBuddyState state;
	char *status;
	JabberCapabilities capabilities;
	char *thread_id;
	enum {
		JABBER_CHAT_STATES_UNKNOWN,
		JABBER_CHAT_STATES_UNSUPPORTED,
		JABBER_CHAT_STATES_SUPPORTED
	} chat_states;
	struct {
		char *version;
		char *name;
		char *os;
	} client;
} JabberBuddyResource;

void jabber_buddy_free(JabberBuddy *jb);
JabberBuddy *jabber_buddy_find(JabberStream *js, const char *name,
		gboolean create);
JabberBuddyResource *jabber_buddy_find_resource(JabberBuddy *jb,
		const char *resource);
JabberBuddyResource *jabber_buddy_track_resource(JabberBuddy *jb, const char *resource,
		int priority, JabberBuddyState state, const char *status);
void jabber_buddy_resource_free(JabberBuddyResource *jbr);
void jabber_buddy_remove_resource(JabberBuddy *jb, const char *resource);
const char *jabber_buddy_get_status_msg(JabberBuddy *jb);
void jabber_buddy_get_info(PurpleConnection *gc, const char *who);
void jabber_buddy_get_info_chat(PurpleConnection *gc, int id,
		const char *resource);

GList *jabber_blist_node_menu(PurpleBlistNode *node);

void jabber_set_info(PurpleConnection *gc, const char *info);
void jabber_setup_set_info(PurplePluginAction *action);
void jabber_set_buddy_icon(PurpleConnection *gc, PurpleStoredImage *img);

const char *jabber_buddy_state_get_name(JabberBuddyState state);
const char *jabber_buddy_state_get_status_id(JabberBuddyState state);
const char *jabber_buddy_state_get_show(JabberBuddyState state);
JabberBuddyState jabber_buddy_status_id_get_state(const char *id);
JabberBuddyState jabber_buddy_show_get_state(const char *id);

void jabber_user_search_begin(PurplePluginAction *);

void jabber_buddy_remove_all_pending_buddy_info_requests(JabberStream *js);

void jabber_vcard_fetch_mine(JabberStream *js);

#endif /* _PURPLE_JABBER_BUDDY_H_ */
