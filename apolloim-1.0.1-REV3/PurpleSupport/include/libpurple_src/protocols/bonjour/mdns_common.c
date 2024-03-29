/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <string.h>

#include "internal.h"
#include "debug.h"

#include "mdns_common.h"
#include "mdns_interface.h"
#include "bonjour.h"
#include "buddy.h"


/**
 * Allocate space for the dns-sd data.
 */
BonjourDnsSd * bonjour_dns_sd_new() {
	BonjourDnsSd *data = g_new0(BonjourDnsSd, 1);
	return data;
}

/**
 * Deallocate the space of the dns-sd data.
 */
void bonjour_dns_sd_free(BonjourDnsSd *data) {
	g_free(data->first);
	g_free(data->last);
	g_free(data->phsh);
	g_free(data->status);
	g_free(data->vc);
	g_free(data->msg);
	g_free(data);
}

static GSList *generate_presence_txt_records(BonjourDnsSd *data) {
	GSList *ret = NULL;
	PurpleKeyValuePair *kvp;
	char portstring[6];
	const char *jid, *aim, *email;

	/* Convert the port to a string */
	snprintf(portstring, sizeof(portstring), "%d", data->port_p2pj);

	jid = purple_account_get_string(data->account, "jid", NULL);
	aim = purple_account_get_string(data->account, "AIM", NULL);
	email = purple_account_get_string(data->account, "email", NULL);

#define _M_ADD_R(k, v) \
	kvp = g_new0(PurpleKeyValuePair, 1); \
	kvp->key = g_strdup(k); \
	kvp->value = g_strdup(v); \
	ret = g_slist_prepend(ret, kvp); \

	/* We should try to follow XEP-0174, but some clients have "issues", so we humor them.
	 * See http://telepathy.freedesktop.org/wiki/SalutInteroperability
	 */

	/* Needed by iChat */
	_M_ADD_R("txtvers", "1")
	/* Needed by Gaim/Pidgin <= 2.0.1 (remove at some point) */
	_M_ADD_R("1st", data->first)
	/* Needed by Gaim/Pidgin <= 2.0.1 (remove at some point) */
	_M_ADD_R("last", data->last)
	/* Needed by Adium */
	_M_ADD_R("port.p2pj", portstring)
	/* Needed by iChat, Gaim/Pidgin <= 2.0.1 */
	_M_ADD_R("status", data->status)
	_M_ADD_R("node", "libpurple")
	_M_ADD_R("ver", VERSION)
	/* Currently always set to "!" since we don't support AV and wont ever be in a conference */
	_M_ADD_R("vc", data->vc)
	if (email != NULL && *email != '\0') {
		_M_ADD_R("email", email)
	}
	if (jid != NULL && *jid != '\0') {
		_M_ADD_R("jid", jid)
	}
	/* Nonstandard, but used by iChat */
	if (aim != NULL && *aim != '\0') {
		_M_ADD_R("AIM", aim)
	}
	if (data->msg != NULL && *data->msg != '\0') {
		_M_ADD_R("msg", data->msg)
	}
	if (data->phsh != NULL && *data->phsh != '\0') {
		_M_ADD_R("phsh", data->phsh)
	}

	/* TODO: ext, nick */
	return ret;
}

static void free_presence_txt_records(GSList *lst) {
	PurpleKeyValuePair *kvp;
	while(lst) {
		kvp = lst->data;
		g_free(kvp->key);
		g_free(kvp->value);
		g_free(kvp);
		lst = g_slist_remove(lst, lst->data);
	}
}

static gboolean publish_presence(BonjourDnsSd *data, PublishType type) {
	GSList *txt_records;
	gboolean ret;

	txt_records = generate_presence_txt_records(data);
	ret = _mdns_publish(data, type, txt_records);
	free_presence_txt_records(txt_records);

	return ret;
}

/**
 * Send a new dns-sd packet updating our status.
 */
void bonjour_dns_sd_send_status(BonjourDnsSd *data, const char *status, const char *status_message) {
	g_free(data->status);
	g_free(data->msg);

	data->status = g_strdup(status);
	data->msg = g_strdup(status_message);

	/* Update our text record with the new status */
	publish_presence(data, PUBLISH_UPDATE);
}

/**
 * Retrieve the buddy icon blob
 */
void bonjour_dns_sd_retrieve_buddy_icon(BonjourBuddy* buddy) {
	_mdns_retrieve_buddy_icon(buddy);
}

void bonjour_dns_sd_update_buddy_icon(BonjourDnsSd *data) {
	PurpleStoredImage *img;

	if ((img = purple_buddy_icons_find_account_icon(data->account))) {
		gconstpointer avatar_data;
		gsize avatar_len;

		avatar_data = purple_imgstore_get_data(img);
		avatar_len = purple_imgstore_get_size(img);

		if (_mdns_set_buddy_icon_data(data, avatar_data, avatar_len)) {
			/* The filename is a SHA-1 hash of the data (conveniently what we need) */
			const char *p, *filename = purple_imgstore_get_filename(img);

			g_free(data->phsh);
			data->phsh = NULL;

			/* Get rid of the extension */
			p = strchr(filename, '.');
			if (p)
				data->phsh = g_strndup(filename, p - filename);
			else
				purple_debug_error("bonjour", "account buddy icon returned unexpected filename (%s)"
								"; unable to extract hash. Clearing buddy icon\n", filename);

			/* Update our TXT record */
			publish_presence(data, PUBLISH_UPDATE);
		}

		purple_imgstore_unref(img);
	} else {
		/* We need to do this regardless of whether data->phsh is set so that we
		 * cancel any icons that are currently in the process of being set */
		_mdns_set_buddy_icon_data(data, NULL, 0);
		if (data->phsh != NULL) {
			/* Clear the buddy icon */
			g_free(data->phsh);
			data->phsh = NULL;
			/* Update our TXT record */
			publish_presence(data, PUBLISH_UPDATE);
		}
	}
}

/**
 * Advertise our presence within the dns-sd daemon and start browsing
 * for other bonjour peers.
 */
gboolean bonjour_dns_sd_start(BonjourDnsSd *data) {

	/* Initialize the dns-sd data and session */
	if (!_mdns_init_session(data))
		return FALSE;

	/* Publish our bonjour IM client at the mDNS daemon */
	if (!publish_presence(data, PUBLISH_START))
		return FALSE;

	/* Advise the daemon that we are waiting for connections */
	if (!_mdns_browse(data)) {
		purple_debug_error("bonjour", "Unable to get service.");
		return FALSE;
	}

	return TRUE;
}

/**
 * Unregister the "_presence._tcp" service at the mDNS daemon.
 */

void bonjour_dns_sd_stop(BonjourDnsSd *data) {
	_mdns_stop(data);
}
