/**
 * @file gg.c Gadu-Gadu protocol plugin
 *
 * purple
 *
 * Copyright (C) 2005  Bartosz Oler <bartosz@bzimage.us>
 *
 * Some parts of the code are adapted or taken from the previous implementation
 * of this plugin written by Arkadiusz Miskiewicz <misiek@pld.org.pl>
 *
 * Thanks to Google's Summer of Code Program.
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

#include "internal.h"

#include "plugin.h"
#include "version.h"
#include "notify.h"
#include "status.h"
#include "blist.h"
#include "accountopt.h"
#include "debug.h"
#include "util.h"
#include "request.h"

#include <libgadu.h>

#include "gg.h"
#include "confer.h"
#include "search.h"
#include "buddylist.h"
#include "gg-utils.h"

static PurplePlugin *my_protocol = NULL;

/* ---------------------------------------------------------------------- */
/* ----- EXTERNAL CALLBACKS --------------------------------------------- */
/* ---------------------------------------------------------------------- */


/* ----- HELPERS -------------------------------------------------------- */

/**
 * Set up libgadu's proxy.
 *
 * @param account Account for which to set up the proxy.
 *
 * @return Zero if proxy setup is valid, otherwise -1.
 */
/* static int ggp_setup_proxy(PurpleAccount *account) {{{ */
static int ggp_setup_proxy(PurpleAccount *account)
{
	PurpleProxyInfo *gpi;

	gpi = purple_proxy_get_setup(account);

	if ((purple_proxy_info_get_type(gpi) != PURPLE_PROXY_NONE) &&
	    (purple_proxy_info_get_host(gpi) == NULL ||
	     purple_proxy_info_get_port(gpi) <= 0)) {

		gg_proxy_enabled = 0;
		purple_notify_error(NULL, NULL, _("Invalid proxy settings"),
				  _("Either the host name or port number specified for your given proxy type is invalid."));
		return -1;
	} else if (purple_proxy_info_get_type(gpi) != PURPLE_PROXY_NONE) {
		gg_proxy_enabled = 1;
		gg_proxy_host = g_strdup(purple_proxy_info_get_host(gpi));
		gg_proxy_port = purple_proxy_info_get_port(gpi);
		gg_proxy_username = g_strdup(purple_proxy_info_get_username(gpi));
		gg_proxy_password = g_strdup(purple_proxy_info_get_password(gpi));
	} else {
		gg_proxy_enabled = 0;
	}

	return 0;
}
/* }}} */

/*
 */
/* static void ggp_async_token_handler(gpointer _gc, gint fd, PurpleInputCondition cond) {{{ */
static void ggp_async_token_handler(gpointer _gc, gint fd, PurpleInputCondition cond)
{
	PurpleConnection *gc = _gc;
	GGPInfo *info = gc->proto_data;
	GGPToken *token = info->token;
	GGPTokenCallback cb;

	struct gg_token *t = NULL;

	purple_debug_info("gg", "token_handler: token->req: check = %d; state = %d;\n",
			token->req->check, token->req->state);

	if (gg_token_watch_fd(token->req) == -1 || token->req->state == GG_STATE_ERROR) {
		purple_debug_error("gg", "token error (1): %d\n", token->req->error);
		purple_input_remove(token->inpa);
		gg_token_free(token->req);
		token->req = NULL;

		purple_notify_error(purple_connection_get_account(gc),
				  _("Token Error"),
				  _("Unable to fetch the token.\n"), NULL);
		return;
	}

	if (token->req->state != GG_STATE_DONE) {
		purple_input_remove(token->inpa);
		token->inpa = purple_input_add(token->req->fd,
						   (token->req->check == 1)
						   	? PURPLE_INPUT_WRITE
							: PURPLE_INPUT_READ,
						   ggp_async_token_handler, gc);
		return;
	}

	if (!(t = token->req->data) || !token->req->body) {
		purple_debug_error("gg", "token error (2): %d\n", token->req->error);
		purple_input_remove(token->inpa);
		gg_token_free(token->req);
		token->req = NULL;

		purple_notify_error(purple_connection_get_account(gc),
				  _("Token Error"),
				  _("Unable to fetch the token.\n"), NULL);
		return;
	}

	purple_input_remove(token->inpa);

	token->id = g_strdup(t->tokenid);
	token->size = token->req->body_size;
	token->data = g_new0(char, token->size);
	memcpy(token->data, token->req->body, token->size);

	purple_debug_info("gg", "TOKEN! tokenid = %s; size = %d\n",
			token->id, token->size);

	gg_token_free(token->req);
	token->req = NULL;
	token->inpa = 0;

	cb = token->cb;
	token->cb = NULL;
	cb(gc);
}
/* }}} */

/*
 */
/* static void ggp_token_request(PurpleConnection *gc, GGPTokenCallback cb) {{{ */
static void ggp_token_request(PurpleConnection *gc, GGPTokenCallback cb)
{
	PurpleAccount *account;
	struct gg_http *req;
	GGPInfo *info;

	account = purple_connection_get_account(gc);

	if (ggp_setup_proxy(account) == -1)
		return;

	info = gc->proto_data;

	if ((req = gg_token(1)) == NULL) {
		purple_notify_error(account,
				  _("Token Error"),
				  _("Unable to fetch the token.\n"), NULL);
		return;
	}

	info->token = g_new(GGPToken, 1);
	info->token->cb = cb;

	info->token->req = req;
	info->token->inpa = purple_input_add(req->fd, PURPLE_INPUT_READ,
					   ggp_async_token_handler, gc);
}
/* }}} */

/* ---------------------------------------------------------------------- */

/**
 * Request buddylist from the server.
 * Buddylist is received in the ggp_callback_recv().
 *
 * @param Current action handler.
 */
/* static void ggp_action_buddylist_get(PurplePluginAction *action) {{{ */
static void ggp_action_buddylist_get(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	GGPInfo *info = gc->proto_data;

	purple_debug_info("gg", "Downloading...\n");

	gg_userlist_request(info->session, GG_USERLIST_GET, NULL);
}
/* }}} */

/**
 * Upload the buddylist to the server.
 *
 * @param action Current action handler.
 */
/* static void ggp_action_buddylist_put(PurplePluginAction *action) {{{ */
static void ggp_action_buddylist_put(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	GGPInfo *info = gc->proto_data;

	char *buddylist = ggp_buddylist_dump(purple_connection_get_account(gc));

	purple_debug_info("gg", "Uploading...\n");
	
	if (buddylist == NULL)
		return;

	gg_userlist_request(info->session, GG_USERLIST_PUT, buddylist);
	g_free(buddylist);
}
/* }}} */

/**
 * Delete buddylist from the server.
 *
 * @param action Current action handler.
 */
/* static void ggp_action_buddylist_delete(PurplePluginAction *action) {{{ */
static void ggp_action_buddylist_delete(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	GGPInfo *info = gc->proto_data;

	purple_debug_info("gg", "Deleting...\n");

	gg_userlist_request(info->session, GG_USERLIST_PUT, NULL);
}
/* }}} */

/*
 */
/* static void ggp_callback_buddylist_save_ok(PurpleConnection *gc, const char *file) {{{ */
static void ggp_callback_buddylist_save_ok(PurpleConnection *gc, const char *file)
{
	PurpleAccount *account = purple_connection_get_account(gc);

	FILE *fh;
	char *buddylist = ggp_buddylist_dump(account);
	gchar *msg;

	purple_debug_info("gg", "Saving...\n");
	purple_debug_info("gg", "file = %s\n", file);

	if (buddylist == NULL) {
		purple_notify_info(account, _("Save Buddylist..."),
			 _("Your buddylist is empty, nothing was written to the file."),
			 NULL);
		return;
	}

	if ((fh = g_fopen(file, "wb")) == NULL) {
		msg = g_strconcat(_("Couldn't open file"), ": ", file, "\n", NULL);
		purple_debug_error("gg", "Could not open file: %s\n", file);
		purple_notify_error(account, _("Couldn't open file"), msg, NULL);
		g_free(msg);
		g_free(buddylist);
		return;
	}

	fwrite(buddylist, sizeof(char), g_utf8_strlen(buddylist, -1), fh);
	fclose(fh);
	g_free(buddylist);

	purple_notify_info(account, _("Save Buddylist..."),
			 _("Buddylist saved successfully!"), NULL);
}
/* }}} */

/*
 */
/* static void ggp_callback_buddylist_load_ok(PurpleConnection *gc, gchar *file) {{{ */
static void ggp_callback_buddylist_load_ok(PurpleConnection *gc, gchar *file)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	GError *error = NULL;
	char *buddylist = NULL;
	gsize length;

	purple_debug_info("gg", "file_name = %s\n", file);

	if (!g_file_get_contents(file, &buddylist, &length, &error)) {
		purple_notify_error(account,
				_("Couldn't load buddylist"),
				_("Couldn't load buddylist"),
				error->message);

		purple_debug_error("gg",
			"Couldn't load buddylist. file = %s; error = %s\n",
			file, error->message);

		g_error_free(error);

		return;
	}

	ggp_buddylist_load(gc, buddylist);
	g_free(buddylist);

	purple_notify_info(account,
			 _("Load Buddylist..."),
			 _("Buddylist loaded successfully!"), NULL);
}
/* }}} */

/*
 */
/* static void ggp_action_buddylist_save(PurplePluginAction *action) {{{ */
static void ggp_action_buddylist_save(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;

	purple_request_file(action, _("Save buddylist..."), NULL, TRUE,
			G_CALLBACK(ggp_callback_buddylist_save_ok), NULL,
			purple_connection_get_account(gc), NULL, NULL,
			gc);
}
/* }}} */

/*
 */
/* static void ggp_action_buddylist_load(PurplePluginAction *action) {{{ */
static void ggp_action_buddylist_load(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;

	purple_request_file(action, "Load buddylist from file...", NULL, FALSE,
			G_CALLBACK(ggp_callback_buddylist_load_ok), NULL,
			purple_connection_get_account(gc), NULL, NULL,
			gc);
}
/* }}} */

/*
 */
/* static void ggp_callback_register_account_ok(PurpleConnection *gc, PurpleRequestFields *fields) {{{ */
static void ggp_callback_register_account_ok(PurpleConnection *gc,
					     PurpleRequestFields *fields)
{
	PurpleAccount *account;
	GGPInfo *info = gc->proto_data;
	struct gg_http *h = NULL;
	struct gg_pubdir *s;
	uin_t uin;
	gchar *email, *p1, *p2, *t;
	GGPToken *token = info->token;

	email = charset_convert(purple_request_fields_get_string(fields, "email"),
			     "UTF-8", "CP1250");
	p1  = charset_convert(purple_request_fields_get_string(fields, "password1"),
			     "UTF-8", "CP1250");
	p2  = charset_convert(purple_request_fields_get_string(fields, "password2"),
			     "UTF-8", "CP1250");
	t   = charset_convert(purple_request_fields_get_string(fields, "token"),
			     "UTF-8", "CP1250");

	account = purple_connection_get_account(gc);

	if (email == NULL || p1 == NULL || p2 == NULL || t == NULL ||
	    *email == '\0' || *p1 == '\0' || *p2 == '\0' || *t == '\0') {
		purple_connection_error(gc, _("Fill in the registration fields."));
		goto exit_err;
	}

	if (g_utf8_collate(p1, p2) != 0) {
		purple_connection_error(gc, _("Passwords do not match."));
		goto exit_err;
	}

	purple_debug_info("gg", "register_account_ok: token_id = %d; t = %s\n",
			token->id, t);
	h = gg_register3(email, p1, token->id, t, 0);
	if (h == NULL || !(s = h->data) || !s->success) {
		purple_connection_error(gc,
			_("Unable to register new account. Error occurred.\n"));
		goto exit_err;
	}

	uin = s->uin;
	purple_debug_info("gg", "registered uin: %d\n", uin);

	g_free(t);
	t = g_strdup_printf("%u", uin);
	purple_account_set_username(account, t);
	/* Save the password if remembering passwords for the account */
	purple_account_set_password(account, p1);

	purple_notify_info(NULL, _("New Gadu-Gadu Account Registered"),
			 _("Registration completed successfully!"), NULL);

	/* TODO: the currently open Accounts Window will not be updated withthe
	 * new username and etc, we need to somehow have it refresh at this
	 * point
	 */

	/* Need to disconnect or actually log in. For now, we disconnect. */
	purple_connection_destroy(gc);

exit_err:
	gg_register_free(h);
	g_free(email);
	g_free(p1);
	g_free(p2);
	g_free(t);
	g_free(token->id);
	g_free(token);
}
/* }}} */

/*
 */
/* static void ggp_callback_register_account_cancel(PurpleConnection *gc, PurpleRequestFields *fields) {{{ */
static void ggp_callback_register_account_cancel(PurpleConnection *gc,
						 PurpleRequestFields *fields)
{
	GGPInfo *info = gc->proto_data;
	GGPToken *token = info->token;

	purple_connection_destroy(gc);

	g_free(token->id);
	g_free(token->data);
	g_free(token);

}
/* }}} */

/*
 */
/* static void ggp_register_user_dialog(PurpleConnection *gc) {{{ */
static void ggp_register_user_dialog(PurpleConnection *gc)
{
	PurpleAccount *account;
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	GGPInfo *info = gc->proto_data;
	GGPToken *token = info->token;


	account = purple_connection_get_account(gc);

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("email",
			_("E-mail"), "", FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("password1",
			_("Password"), "", FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("password2",
			_("Password (retype)"), "", FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("token",
			_("Enter current token"), "", FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	/* original size: 60x24 */
	field = purple_request_field_image_new("token_img",
			_("Current token"), token->data, token->size);
	purple_request_field_group_add_field(group, field);

	purple_request_fields(account,
		_("Register New Gadu-Gadu Account"),
		_("Register New Gadu-Gadu Account"),
		_("Please, fill in the following fields"),
		fields,
		_("OK"), G_CALLBACK(ggp_callback_register_account_ok),
		_("Cancel"), G_CALLBACK(ggp_callback_register_account_cancel),
		purple_connection_get_account(gc), NULL, NULL,
		gc);
}
/* }}} */

/* ----- PUBLIC DIRECTORY SEARCH ---------------------------------------- */

/*
 */
/* static void ggp_callback_show_next(PurpleConnection *gc, GList *row, gpointer user_data) {{{ */
static void ggp_callback_show_next(PurpleConnection *gc, GList *row, gpointer user_data)
{
	GGPInfo *info = gc->proto_data;
	GGPSearchForm *form = user_data;
	guint32 seq;

	g_free(form->offset);
	form->offset = g_strdup(form->last_uin);

	ggp_search_remove(info->searches, form->seq);

	seq = ggp_search_start(gc, form);
	ggp_search_add(info->searches, seq, form);
}
/* }}} */

/*
 */
/* static void ggp_callback_add_buddy(PurpleConnection *gc, GList *row, gpointer user_data) {{{ */
static void ggp_callback_add_buddy(PurpleConnection *gc, GList *row, gpointer user_data)
{
	purple_blist_request_add_buddy(purple_connection_get_account(gc),
				     g_list_nth_data(row, 0), NULL, NULL);
}
/* }}} */

/*
 */
/* static void ggp_callback_im(PurpleConnection *gc, GList *row, gpointer user_data) {{{ */
static void ggp_callback_im(PurpleConnection *gc, GList *row, gpointer user_data)
{
	PurpleAccount *account;
	PurpleConversation *conv;
	char *name;

	account = purple_connection_get_account(gc);

	name = g_list_nth_data(row, 0);
	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, name);
	purple_conversation_present(conv);
}
/* }}} */

/*
 */
/* static void ggp_callback_find_buddies(PurpleConnection *gc, PurpleRequestFields *fields) {{{ */
static void ggp_callback_find_buddies(PurpleConnection *gc, PurpleRequestFields *fields)
{
	GGPInfo *info = gc->proto_data;
	GGPSearchForm *form;
	guint32 seq;

	form = ggp_search_form_new(GGP_SEARCH_TYPE_FULL);

	form->user_data = info;
	form->lastname  = charset_convert(
				purple_request_fields_get_string(fields, "lastname"),
				"UTF-8", "CP1250");
	form->firstname = charset_convert(
				purple_request_fields_get_string(fields, "firstname"),
				"UTF-8", "CP1250");
	form->nickname  = charset_convert(
				purple_request_fields_get_string(fields, "nickname"),
				"UTF-8", "CP1250");
	form->city      = charset_convert(
				purple_request_fields_get_string(fields, "city"),
				"UTF-8", "CP1250");
	form->birthyear = charset_convert(
				purple_request_fields_get_string(fields, "year"),
				"UTF-8", "CP1250");

	switch (purple_request_fields_get_choice(fields, "gender")) {
		case 1:
			form->gender = g_strdup(GG_PUBDIR50_GENDER_MALE);
			break;
		case 2:
			form->gender = g_strdup(GG_PUBDIR50_GENDER_FEMALE);
			break;
		default:
			form->gender = NULL;
			break;
	}

	form->active = purple_request_fields_get_bool(fields, "active")
				   ? g_strdup(GG_PUBDIR50_ACTIVE_TRUE) : NULL;

	form->offset = g_strdup("0");

	seq = ggp_search_start(gc, form);
	ggp_search_add(info->searches, seq, form);
}
/* }}} */

/*
 */
/* static void ggp_find_buddies(PurplePluginAction *action) {{{ */
static void ggp_find_buddies(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;

	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("lastname",
			_("Last name"), NULL, FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("firstname",
			_("First name"), NULL, FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("nickname",
			_("Nickname"), NULL, FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("city",
			_("City"), NULL, FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("year",
			_("Year of birth"), NULL, FALSE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_choice_new("gender", _("Gender"), 0);
	purple_request_field_choice_add(field, _("Male or female"));
	purple_request_field_choice_add(field, _("Male"));
	purple_request_field_choice_add(field, _("Female"));
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_bool_new("active",
			_("Only online"), FALSE);
	purple_request_field_group_add_field(group, field);

	purple_request_fields(gc,
		_("Find buddies"),
		_("Find buddies"),
		_("Please, enter your search criteria below"),
		fields,
		_("OK"), G_CALLBACK(ggp_callback_find_buddies),
		_("Cancel"), NULL,
		purple_connection_get_account(gc), NULL, NULL,
		gc);
}
/* }}} */

/* ----- CHANGE PASSWORD ------------------------------------------------ */

/*
 */
/* static void ggp_callback_change_passwd_ok(PurpleConnection *gc, PurpleRequestFields *fields) {{{ */
static void ggp_callback_change_passwd_ok(PurpleConnection *gc, PurpleRequestFields *fields)
{
	PurpleAccount *account;
	GGPInfo *info = gc->proto_data;
	struct gg_http *h;
	gchar *cur, *p1, *p2, *t;

	cur = charset_convert(
			purple_request_fields_get_string(fields, "password_cur"),
			"UTF-8", "CP1250");
	p1  = charset_convert(
			purple_request_fields_get_string(fields, "password1"),
			"UTF-8", "CP1250");
	p2  = charset_convert(
			purple_request_fields_get_string(fields, "password2"),
			"UTF-8", "CP1250");
	t   = charset_convert(
			purple_request_fields_get_string(fields, "token"),
			"UTF-8", "CP1250");

	account = purple_connection_get_account(gc);

	if (cur == NULL || p1 == NULL || p2 == NULL || t == NULL ||
	    *cur == '\0' || *p1 == '\0' || *p2 == '\0' || *t == '\0') {
		purple_notify_error(account, NULL, _("Fill in the fields."), NULL);
		goto exit_err;
	}

	if (g_utf8_collate(p1, p2) != 0) {
		purple_notify_error(account, NULL,
				  _("New passwords do not match."), NULL);
		goto exit_err;
	}

	if (g_utf8_collate(cur, purple_account_get_password(account)) != 0) {
		purple_notify_error(account, NULL,
			_("Your current password is different from the one that you specified."),
			NULL);
		goto exit_err;
	}

	purple_debug_info("gg", "Changing password\n");

	/* XXX: this e-mail should be a pref... */
	h = gg_change_passwd4(ggp_get_uin(account),
			      "user@example.net", purple_account_get_password(account),
			      p1, info->token->id, t, 0);

	if (h == NULL) {
		purple_notify_error(account, NULL,
			_("Unable to change password. Error occurred.\n"),
			NULL);
		goto exit_err;
	}

	purple_account_set_password(account, p1);

	gg_change_passwd_free(h);

	purple_notify_info(account, _("Change password for the Gadu-Gadu account"),
			 _("Password was changed successfully!"), NULL);

exit_err:
	g_free(cur);
	g_free(p1);
	g_free(p2);
	g_free(t);
	g_free(info->token->id);
	g_free(info->token->data);
	g_free(info->token);
}
/* }}} */

/*
 */
/* static void ggp_change_passwd_dialog(PurpleConnection *gc) {{{ */
static void ggp_change_passwd_dialog(PurpleConnection *gc)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	GGPInfo *info = gc->proto_data;
	GGPToken *token = info->token;

	char *msg;


	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("password_cur",
			_("Current password"), "", FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("password1",
			_("Password"), "", FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("password2",
			_("Password (retype)"), "", FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("token",
			_("Enter current token"), "", FALSE);
	purple_request_field_string_set_masked(field, FALSE);
	purple_request_field_group_add_field(group, field);

	/* original size: 60x24 */
	field = purple_request_field_image_new("token_img",
			_("Current token"), token->data, token->size);
	purple_request_field_group_add_field(group, field);

	msg = g_strdup_printf("%s %d",
		_("Please, enter your current password and your new password for UIN: "),
		ggp_get_uin(purple_connection_get_account(gc)));

	purple_request_fields(gc,
		_("Change Gadu-Gadu Password"),
		_("Change Gadu-Gadu Password"),
		msg,
		fields, _("OK"), G_CALLBACK(ggp_callback_change_passwd_ok),
		_("Cancel"), NULL,
		purple_connection_get_account(gc), NULL, NULL,
		gc);

	g_free(msg);
}
/* }}} */

/*
 */
/* static void ggp_change_passwd(PurplePluginAction *action) {{{ */
static void ggp_change_passwd(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;

	ggp_token_request(gc, ggp_change_passwd_dialog);
}
/* }}} */

/* ----- CONFERENCES ---------------------------------------------------- */

/*
 */
/* static void ggp_callback_add_to_chat_ok(PurpleConnection *gc, PurpleRequestFields *fields) {{{ */
static void ggp_callback_add_to_chat_ok(PurpleConnection *gc, PurpleRequestFields *fields)
{
	GGPInfo *info = gc->proto_data;
	PurpleRequestField *field;
	GList *sel;

	field = purple_request_fields_get_field(fields, "name");
	sel = purple_request_field_list_get_selected(field);

	ggp_confer_participants_add_uin(gc, sel->data, info->tmp_buddy);
	info->tmp_buddy = 0;
}
/* }}} */

/*
 */
/* static void ggp_bmenu_add_to_chat(PurpleBlistNode *node, gpointer ignored) {{{ */
static void ggp_bmenu_add_to_chat(PurpleBlistNode *node, gpointer ignored)
{
	PurpleBuddy *buddy;
	PurpleConnection *gc;
	GGPInfo *info;

	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	GList *l;
	gchar *msg;

	buddy = (PurpleBuddy *)node;
	gc = purple_account_get_connection(purple_buddy_get_account(buddy));
	info = gc->proto_data;

	/* TODO: It tmp_buddy != 0 then stop! */
	info->tmp_buddy = ggp_str_to_uin(purple_buddy_get_name(buddy));

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_list_new("name", "Chat name");
	for (l = info->chats; l != NULL; l = l->next) {
		GGPChat *chat = l->data;
		purple_request_field_list_add(field, g_strdup(chat->name),
					    g_strdup(chat->name));
	}
	purple_request_field_group_add_field(group, field);

	msg = g_strdup_printf(_("Select a chat for buddy: %s"),
			      purple_buddy_get_alias(buddy));
	purple_request_fields(gc,
			_("Add to chat..."),
			_("Add to chat..."),
			msg,
			fields,
			_("Add"), G_CALLBACK(ggp_callback_add_to_chat_ok),
			_("Cancel"), NULL,
			purple_connection_get_account(gc), NULL, NULL,			  
			gc);
	g_free(msg);
}
/* }}} */

/* ----- BLOCK BUDDIES -------------------------------------------------- */

/*
 */
/* static void ggp_bmenu_block(PurpleBlistNode *node, gpointer ignored) {{{ */
static void ggp_bmenu_block(PurpleBlistNode *node, gpointer ignored)
{
	PurpleConnection *gc;
	PurpleBuddy *buddy;
	GGPInfo *info;
	uin_t uin;

	buddy = (PurpleBuddy *)node;
	gc = purple_account_get_connection(purple_buddy_get_account(buddy));
	info = gc->proto_data;

	uin = ggp_str_to_uin(purple_buddy_get_name(buddy));

	if (purple_blist_node_get_bool(node, "blocked")) {
		purple_blist_node_set_bool(node, "blocked", FALSE);
		gg_remove_notify_ex(info->session, uin, GG_USER_BLOCKED);
		gg_add_notify_ex(info->session, uin, GG_USER_NORMAL);
		purple_debug_info("gg", "send: uin=%d; mode=NORMAL\n", uin);
	} else {
		purple_blist_node_set_bool(node, "blocked", TRUE);
		gg_remove_notify_ex(info->session, uin, GG_USER_NORMAL);
		gg_add_notify_ex(info->session, uin, GG_USER_BLOCKED);
		purple_debug_info("gg", "send: uin=%d; mode=BLOCKED\n", uin);
	}
}
/* }}} */

/* ---------------------------------------------------------------------- */
/* ----- INTERNAL CALLBACKS --------------------------------------------- */
/* ---------------------------------------------------------------------- */

/* just a prototype */
static void ggp_set_status(PurpleAccount *account, PurpleStatus *status);

/**
 * Handle change of the status of the buddy.
 *
 * @param gc     PurpleConnection
 * @param uin    UIN of the buddy.
 * @param status ID of the status.
 * @param descr  Description.
 */
/* static void ggp_generic_status_handler(PurpleConnection *gc, uin_t uin, int status, const char *descr) {{{ */
static void ggp_generic_status_handler(PurpleConnection *gc, uin_t uin,
				       int status, const char *descr)
{
	gchar *from;
	const char *st;
	gchar *msg;

	from = g_strdup_printf("%ld", (unsigned long int)uin);
	switch (status) {
		case GG_STATUS_NOT_AVAIL:
		case GG_STATUS_NOT_AVAIL_DESCR:
			st = "offline";
			break;
		case GG_STATUS_AVAIL:
		case GG_STATUS_AVAIL_DESCR:
			st = "available";
			break;
		case GG_STATUS_BUSY:
		case GG_STATUS_BUSY_DESCR:
			st = "away";
			break;
		case GG_STATUS_BLOCKED:
			/* user is blocking us.... */
			st = "blocked";
			break;
		default:
			st = "available";
			purple_debug_info("gg",
				"GG_EVENT_NOTIFY: Unknown status: %d\n", status);
			break;
	}

	purple_debug_info("gg", "st = %s\n", st);
	msg = charset_convert(descr, "CP1250", "UTF-8");
	purple_prpl_got_user_status(purple_connection_get_account(gc),
				  from, st, "message", msg, NULL);
	g_free(from);
	g_free(msg);
}
/* }}} */

/*
 */
/* static void ggp_sr_close_cb(gpointer user_data) {{{ */
static void ggp_sr_close_cb(gpointer user_data)
{
	GGPSearchForm *form = user_data;
	GGPInfo *info = form->user_data;

	ggp_search_remove(info->searches, form->seq);
	ggp_search_form_destroy(form);
}
/* }}} */

/**
 * Translate a status' ID to a more user-friendly name.
 *
 * @param id The ID of the status.
 *
 * @return The user-friendly name of the status.
 */
/* static const char *ggp_status_by_id(unsigned int id) {{{ */
static const char *ggp_status_by_id(unsigned int id)
{
	const char *st;

	purple_debug_info("gg", "ggp_status_by_id: %d\n", id);
	switch (id) {
		case GG_STATUS_NOT_AVAIL:
			st = _("Offline");
			break;
		case GG_STATUS_AVAIL:
			st = _("Available");
			break;
		case GG_STATUS_BUSY:
			st = _("Away");
			break;
		default:
			st = _("Unknown");
			break;
	}

	return st;
}
/* }}} */

/*
 */
/* static void ggp_pubdir_handle_info(PurpleConnection *gc, gg_pubdir50_t req, GGPSearchForm *form) {{{ */
static void ggp_pubdir_handle_info(PurpleConnection *gc, gg_pubdir50_t req,
				   GGPSearchForm *form)
{
	PurpleNotifyUserInfo *user_info;
	PurpleBuddy *buddy;
	char *val, *who;

	user_info = purple_notify_user_info_new();

	val = ggp_search_get_result(req, 0, GG_PUBDIR50_STATUS);
	/* XXX: Use of ggp_str_to_uin() is an ugly hack! */
	purple_notify_user_info_add_pair(user_info, _("Status"), ggp_status_by_id(ggp_str_to_uin(val)));
	g_free(val);

	who = ggp_search_get_result(req, 0, GG_PUBDIR50_UIN);
	purple_notify_user_info_add_pair(user_info, _("UIN"), who);

	val = ggp_search_get_result(req, 0, GG_PUBDIR50_FIRSTNAME);
	purple_notify_user_info_add_pair(user_info, _("First Name"), val);
	g_free(val);

	val = ggp_search_get_result(req, 0, GG_PUBDIR50_NICKNAME);
	purple_notify_user_info_add_pair(user_info, _("Nickname"), val);
	g_free(val);

	val = ggp_search_get_result(req, 0, GG_PUBDIR50_CITY);
	purple_notify_user_info_add_pair(user_info, _("City"), val);
	g_free(val);

	val = ggp_search_get_result(req, 0, GG_PUBDIR50_BIRTHYEAR);
	if (strncmp(val, "0", 1)) {
		purple_notify_user_info_add_pair(user_info, _("Birth Year"), val);
	}
	g_free(val);

	/*
	 * Include a status message, if exists and buddy is in the blist.
	 */
	buddy = purple_find_buddy(purple_connection_get_account(gc), who);
	if (NULL != buddy) {
		PurpleStatus *status;
		const char *msg;
		char *text;

		status = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
		msg = purple_status_get_attr_string(status, "message");

		if (msg != NULL) {
			text = g_markup_escape_text(msg, -1);
			purple_notify_user_info_add_pair(user_info, _("Message"), text);
			g_free(text);
		}
	}

	purple_notify_userinfo(gc, who, user_info, ggp_sr_close_cb, form);
	g_free(who);
	purple_notify_user_info_destroy(user_info);
}
/* }}} */

/*
 */
/* static void ggp_pubdir_handle_full(PurpleConnection *gc, gg_pubdir50_t req, GGPSearchForm *form) {{{ */
static void ggp_pubdir_handle_full(PurpleConnection *gc, gg_pubdir50_t req,
				   GGPSearchForm *form)
{
	PurpleNotifySearchResults *results;
	PurpleNotifySearchColumn *column;
	int res_count;
	int start;
	int i;

	g_return_if_fail(form != NULL);

	res_count = gg_pubdir50_count(req);
	res_count = (res_count > PUBDIR_RESULTS_MAX) ? PUBDIR_RESULTS_MAX : res_count;

	results = purple_notify_searchresults_new();

	if (results == NULL) {
		purple_debug_error("gg", "ggp_pubdir_reply_handler: "
				 "Unable to display the search results.\n");
		purple_notify_error(gc, NULL,
				  _("Unable to display the search results."),
				  NULL);
		ggp_sr_close_cb(form);
		return;
	}

	column = purple_notify_searchresults_column_new(_("UIN"));
	purple_notify_searchresults_column_add(results, column);

	column = purple_notify_searchresults_column_new(_("First Name"));
	purple_notify_searchresults_column_add(results, column);

	column = purple_notify_searchresults_column_new(_("Nickname"));
	purple_notify_searchresults_column_add(results, column);

	column = purple_notify_searchresults_column_new(_("City"));
	purple_notify_searchresults_column_add(results, column);

	column = purple_notify_searchresults_column_new(_("Birth Year"));
	purple_notify_searchresults_column_add(results, column);

	purple_debug_info("gg", "Going with %d entries\n", res_count);

	start = (int)ggp_str_to_uin(gg_pubdir50_get(req, 0, GG_PUBDIR50_START));
	purple_debug_info("gg", "start = %d\n", start);

	for (i = 0; i < res_count; i++) {
		GList *row = NULL;
		char *birth = ggp_search_get_result(req, i, GG_PUBDIR50_BIRTHYEAR);

		/* TODO: Status will be displayed as an icon. */
		/* row = g_list_append(row, ggp_search_get_result(req, i, GG_PUBDIR50_STATUS)); */
		row = g_list_append(row, ggp_search_get_result(req, i,
							GG_PUBDIR50_UIN));
		row = g_list_append(row, ggp_search_get_result(req, i,
							GG_PUBDIR50_FIRSTNAME));
		row = g_list_append(row, ggp_search_get_result(req, i,
							GG_PUBDIR50_NICKNAME));
		row = g_list_append(row, ggp_search_get_result(req, i,
							GG_PUBDIR50_CITY));
		row = g_list_append(row,
			(birth && strncmp(birth, "0", 1)) ? birth : g_strdup("-"));

		purple_notify_searchresults_row_add(results, row);

		if (i == res_count - 1) {
			g_free(form->last_uin);
			form->last_uin = ggp_search_get_result(req, i, GG_PUBDIR50_UIN);
		}
	}

	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_CONTINUE,
					     ggp_callback_show_next);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_ADD,
					     ggp_callback_add_buddy);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_IM,
					     ggp_callback_im);

	if (form->window == NULL) {
		void *h = purple_notify_searchresults(gc,
				_("Gadu-Gadu Public Directory"),
				_("Search results"), NULL, results,
				(PurpleNotifyCloseCallback)ggp_sr_close_cb,
				form);

		if (h == NULL) {
			purple_debug_error("gg", "ggp_pubdir_reply_handler: "
					 "Unable to display the search results.\n");
			purple_notify_error(gc, NULL,
					  _("Unable to display the search results."),
					  NULL);
			return;
		}

		form->window = h;
	} else {
		purple_notify_searchresults_new_rows(gc, results, form->window);
	}
}
/* }}} */

/*
 */
/* static void ggp_pubdir_reply_handler(PurpleConnection *gc, gg_pubdir50_t req) {{{ */
static void ggp_pubdir_reply_handler(PurpleConnection *gc, gg_pubdir50_t req)
{
	GGPInfo *info = gc->proto_data;
	GGPSearchForm *form;
	int res_count;
	guint32 seq;

	seq = gg_pubdir50_seq(req);
	form = ggp_search_get(info->searches, seq);

	/*
	 * this can happen when user will request more results
	 * and close the results window before they arrive.
	 */
	g_return_if_fail(form != NULL);

	res_count = gg_pubdir50_count(req);
	if (res_count < 1) {
		purple_debug_info("gg", "GG_EVENT_PUBDIR50_SEARCH_REPLY: Nothing found\n");
		purple_notify_error(gc, NULL,
			_("No matching users found"),
			_("There are no users matching your search criteria."));
		ggp_sr_close_cb(form);
		return;
	}

	switch (form->search_type) {
		case GGP_SEARCH_TYPE_INFO:
			ggp_pubdir_handle_info(gc, req, form);
			break;
		case GGP_SEARCH_TYPE_FULL:
			ggp_pubdir_handle_full(gc, req, form);
			break;
		default:
			purple_debug_warning("gg", "Unknown search_type!\n");
			break;
	}
}
/* }}} */

/**
 * Dispatch a message received from a buddy.
 *
 * @param gc PurpleConnection.
 * @param ev Gadu-Gadu event structure.
 */
/* static void ggp_recv_message_handler(PurpleConnection *gc, const struct gg_event *ev) {{{ */
static void ggp_recv_message_handler(PurpleConnection *gc, const struct gg_event *ev)
{
	GGPInfo *info = gc->proto_data;
	PurpleConversation *conv;
	gchar *from;
	gchar *msg;
	gchar *tmp;

	from = g_strdup_printf("%lu", (unsigned long int)ev->event.msg.sender);

	tmp = charset_convert((const char *)ev->event.msg.message,
			      "CP1250", "UTF-8");
	purple_str_strip_char(tmp, '\r');
	msg = g_markup_escape_text(tmp, -1);
	g_free(tmp);

	purple_debug_info("gg", "msg form (%s): %s (class = %d; rcpt_count = %d)\n",
			from, msg, ev->event.msg.msgclass,
			ev->event.msg.recipients_count);

	if (ev->event.msg.recipients_count == 0) {
		serv_got_im(gc, from, msg, 0, ev->event.msg.time);
	} else {
		const char *chat_name;
		int chat_id;
		char *buddy_name;

		chat_name = ggp_confer_find_by_participants(gc,
				ev->event.msg.recipients,
				ev->event.msg.recipients_count);

		if (chat_name == NULL) {
			chat_name = ggp_confer_add_new(gc, NULL);
			serv_got_joined_chat(gc, info->chats_count, chat_name);

			ggp_confer_participants_add_uin(gc, chat_name,
							ev->event.msg.sender);

			ggp_confer_participants_add(gc, chat_name,
						    ev->event.msg.recipients,
						    ev->event.msg.recipients_count);
		}
		conv = ggp_confer_find_by_name(gc, chat_name);
		chat_id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv));

		buddy_name = ggp_buddy_get_name(gc, ev->event.msg.sender);
		serv_got_chat_in(gc, chat_id, buddy_name,
				 PURPLE_MESSAGE_RECV, msg, ev->event.msg.time);
		g_free(buddy_name);
	}
	g_free(msg);
	g_free(from);
}
/* }}} */

/*
 */
/* static void ggp_callback_recv(gpointer _gc, gint fd, PurpleInputCondition cond) {{{ */
static void ggp_callback_recv(gpointer _gc, gint fd, PurpleInputCondition cond)
{
	PurpleConnection *gc = _gc;
	GGPInfo *info = gc->proto_data;
	struct gg_event *ev;
	int i;

	if (!(ev = gg_watch_fd(info->session))) {
		purple_debug_error("gg",
			"ggp_callback_recv: gg_watch_fd failed -- CRITICAL!\n");
		purple_connection_error(gc, _("Unable to read socket"));
		return;
	}

	switch (ev->type) {
		case GG_EVENT_NONE:
			/* Nothing happened. */
			break;
		case GG_EVENT_MSG:
			ggp_recv_message_handler(gc, ev);
			break;
		case GG_EVENT_ACK:
			purple_debug_info("gg",
				"message sent to: %ld, delivery status=%d, seq=%d\n",
				ev->event.ack.recipient, ev->event.ack.status,
				ev->event.ack.seq);
			break;
		case GG_EVENT_NOTIFY:
		case GG_EVENT_NOTIFY_DESCR:
			{
				struct gg_notify_reply *n;
				char *descr;

				purple_debug_info("gg", "notify_pre: (%d) status: %d\n",
						ev->event.notify->uin,
						ev->event.notify->status);

				n = (ev->type == GG_EVENT_NOTIFY) ? ev->event.notify
								  : ev->event.notify_descr.notify;

				for (; n->uin; n++) {
					descr = (ev->type == GG_EVENT_NOTIFY) ? NULL
							: ev->event.notify_descr.descr;

					purple_debug_info("gg",
						"notify: (%d) status: %d; descr: %s\n",
						n->uin, n->status, descr ? descr : "(null)");

					ggp_generic_status_handler(gc,
						n->uin, n->status, descr);
				}
			}
			break;
		case GG_EVENT_NOTIFY60:
			purple_debug_info("gg",
				"notify60_pre: (%d) status=%d; version=%d; descr=%s\n",
				ev->event.notify60->uin, ev->event.notify60->status,
				ev->event.notify60->version,
				ev->event.notify60->descr ? ev->event.notify60->descr : "(null)");

			for (i = 0; ev->event.notify60[i].uin; i++) {
				purple_debug_info("gg",
					"notify60: (%d) status=%d; version=%d; descr=%s\n",
					ev->event.notify60[i].uin,
					ev->event.notify60[i].status,
					ev->event.notify60[i].version,
					ev->event.notify60[i].descr ? ev->event.notify60[i].descr : "(null)");

				ggp_generic_status_handler(gc, ev->event.notify60[i].uin,
					ev->event.notify60[i].status,
					ev->event.notify60[i].descr);
			}
			break;
		case GG_EVENT_STATUS:
			purple_debug_info("gg", "status: (%d) status=%d; descr=%s\n",
					ev->event.status.uin, ev->event.status.status,
					ev->event.status.descr ? ev->event.status.descr : "(null)");

			ggp_generic_status_handler(gc, ev->event.status.uin,
				ev->event.status.status, ev->event.status.descr);
			break;
		case GG_EVENT_STATUS60:
			purple_debug_info("gg",
				"status60: (%d) status=%d; version=%d; descr=%s\n",
				ev->event.status60.uin, ev->event.status60.status,
				ev->event.status60.version,
				ev->event.status60.descr ? ev->event.status60.descr : "(null)");

			ggp_generic_status_handler(gc, ev->event.status60.uin,
				ev->event.status60.status, ev->event.status60.descr);
			break;
		case GG_EVENT_USERLIST:
	    		if (ev->event.userlist.type == GG_USERLIST_GET_REPLY) {
				purple_debug_info("gg", "GG_USERLIST_GET_REPLY\n");
				purple_notify_info(gc, NULL,
					_("Buddy list downloaded"),
					_("Your buddy list was downloaded from the server."));
				if (ev->event.userlist.reply != NULL) {
					ggp_buddylist_load(gc, ev->event.userlist.reply);
				}
			} else {
				purple_debug_info("gg", "GG_USERLIST_PUT_REPLY\n");
				purple_notify_info(gc, NULL,
					_("Buddy list uploaded"),
					_("Your buddy list was stored on the server."));
			}
			break;
		case GG_EVENT_PUBDIR50_SEARCH_REPLY:
			ggp_pubdir_reply_handler(gc, ev->event.pubdir50);
			break;
		default:
			purple_debug_error("gg",
				"unsupported event type=%d\n", ev->type);
			break;
	}

	gg_free_event(ev);
}
/* }}} */

/*
 */
/* static void ggp_async_login_handler(gpointer _gc, gint fd, PurpleInputCondition cond) {{{ */
static void ggp_async_login_handler(gpointer _gc, gint fd, PurpleInputCondition cond)
{
	PurpleConnection *gc = _gc;
	GGPInfo *info;
	struct gg_event *ev;

	g_return_if_fail(PURPLE_CONNECTION_IS_VALID(gc));

	info = gc->proto_data;

	purple_debug_info("gg", "login_handler: session: check = %d; state = %d;\n",
			info->session->check, info->session->state);

	switch (info->session->state) {
		case GG_STATE_RESOLVING:
			purple_debug_info("gg", "GG_STATE_RESOLVING\n");
			break;
		case GG_STATE_CONNECTING_HUB:
			purple_debug_info("gg", "GG_STATE_CONNECTING_HUB\n");
			break;
		case GG_STATE_READING_DATA:
			purple_debug_info("gg", "GG_STATE_READING_DATA\n");
			break;
		case GG_STATE_CONNECTING_GG:
			purple_debug_info("gg", "GG_STATE_CONNECTING_GG\n");
			break;
		case GG_STATE_READING_KEY:
			purple_debug_info("gg", "GG_STATE_READING_KEY\n");
			break;
		case GG_STATE_READING_REPLY:
			purple_debug_info("gg", "GG_STATE_READING_REPLY\n");
			break;
		default:
			purple_debug_error("gg", "unknown state = %d\n",
					 info->session->state);
		break;
	}

	if (!(ev = gg_watch_fd(info->session))) {
		purple_debug_error("gg", "login_handler: gg_watch_fd failed!\n");
		purple_connection_error(gc, _("Unable to read socket"));
		return;
	}
	purple_debug_info("gg", "login_handler: session->fd = %d\n", info->session->fd);
	purple_debug_info("gg", "login_handler: session: check = %d; state = %d;\n",
			info->session->check, info->session->state);

	purple_input_remove(gc->inpa);

	/** XXX I think that this shouldn't be done if ev->type is GG_EVENT_CONN_FAILED or GG_EVENT_CONN_SUCCESS -datallah */
	gc->inpa = purple_input_add(info->session->fd,
				  (info->session->check == 1) ? PURPLE_INPUT_WRITE
							      : PURPLE_INPUT_READ,
				  ggp_async_login_handler, gc);

	switch (ev->type) {
		case GG_EVENT_NONE:
			/* Nothing happened. */
			purple_debug_info("gg", "GG_EVENT_NONE\n");
			break;
		case GG_EVENT_CONN_SUCCESS:
			{
				PurpleAccount *account;
				PurplePresence *presence;
				PurpleStatus *status;

				purple_debug_info("gg", "GG_EVENT_CONN_SUCCESS\n");
				purple_input_remove(gc->inpa);
				gc->inpa = purple_input_add(info->session->fd,
							  PURPLE_INPUT_READ,
							  ggp_callback_recv, gc);

				/* gg_change_status(info->session, GG_STATUS_AVAIL); */

				account = purple_connection_get_account(gc);
				presence = purple_account_get_presence(account);
				status = purple_presence_get_active_status(presence);

				ggp_set_status(account, status);
				purple_connection_set_state(gc, PURPLE_CONNECTED);
				ggp_buddylist_send(gc);
			}
			break;
		case GG_EVENT_CONN_FAILED:
			purple_input_remove(gc->inpa);
			gc->inpa = 0;
			purple_connection_error(gc, _("Connection failed."));
			break;
		default:
			purple_debug_error("gg", "strange event: %d\n", ev->type);
			break;
	}

	gg_free_event(ev);
}
/* }}} */

/* ---------------------------------------------------------------------- */
/* ----- PurplePluginProtocolInfo ----------------------------------------- */
/* ---------------------------------------------------------------------- */

/* static const char *ggp_list_icon(PurpleAccount *account, PurpleBuddy *buddy) {{{ */
static const char *ggp_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
	return "gadu-gadu";
}
/* }}} */

/* static char *ggp_status_text(PurpleBuddy *b) {{{ */
static char *ggp_status_text(PurpleBuddy *b)
{
	PurpleStatus *status;
	const char *msg;
	char *text;
	char *tmp;

	status = purple_presence_get_active_status(purple_buddy_get_presence(b));

	msg = purple_status_get_attr_string(status, "message");

	if (msg != NULL) {
		tmp = purple_markup_strip_html(msg);
		text = g_markup_escape_text(tmp, -1);
		g_free(tmp);

		return text;
	} else {
		tmp = purple_utf8_salvage(purple_status_get_name(status));
		text = g_markup_escape_text(tmp, -1);
		g_free(tmp);

		return text;
	}
}
/* }}} */

/* static void ggp_tooltip_text(PurpleBuddy *b, PurpleNotifyUserInfo *user_info, gboolean full) {{{ */
static void ggp_tooltip_text(PurpleBuddy *b, PurpleNotifyUserInfo *user_info, gboolean full)
{
	PurpleStatus *status;
	char *text, *tmp;
	const char *msg, *name;

	g_return_if_fail(b != NULL);

	status = purple_presence_get_active_status(purple_buddy_get_presence(b));
	msg = purple_status_get_attr_string(status, "message");
	name = purple_status_get_name(status);

	if (msg != NULL) {
		text = g_markup_escape_text(msg, -1);
		if (PURPLE_BUDDY_IS_ONLINE(b)) {
			tmp = g_strdup_printf("%s: %s", name, text);
			purple_notify_user_info_add_pair(user_info, _("Status"), tmp);
			g_free(tmp);
		} else {
			purple_notify_user_info_add_pair(user_info, _("Message"), text);
		}
		g_free(text);
	/* We don't want to duplicate 'Status: Offline'. */
	} else if (PURPLE_BUDDY_IS_ONLINE(b)) {
		purple_notify_user_info_add_pair(user_info, _("Status"), name);
	}
}
/* }}} */

/* static GList *ggp_status_types(PurpleAccount *account) {{{ */
static GList *ggp_status_types(PurpleAccount *account)
{
	PurpleStatusType *type;
	GList *types = NULL;

	type = purple_status_type_new_with_attrs(
			PURPLE_STATUS_AVAILABLE, NULL, NULL, TRUE, TRUE, FALSE,
			"message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
			NULL);
	types = g_list_append(types, type);

	/*
	 * Without this selecting Invisible as own status doesn't
	 * work. It's not used and not needed to show status of buddies.
	 */
	type = purple_status_type_new_with_attrs(
			PURPLE_STATUS_INVISIBLE, NULL, NULL, TRUE, TRUE, FALSE,
			"message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
			NULL);
	types = g_list_append(types, type);

	type = purple_status_type_new_with_attrs(
			PURPLE_STATUS_AWAY, NULL, NULL, TRUE, TRUE, FALSE,
			"message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
			NULL);
	types = g_list_append(types, type);

	/*
	 * This status is necessary to display guys who are blocking *us*.
	 */
	type = purple_status_type_new_with_attrs(
			PURPLE_STATUS_INVISIBLE, "blocked", _("Blocked"), TRUE, FALSE, FALSE,
			"message", _("Message"), purple_value_new(PURPLE_TYPE_STRING), NULL);
	types = g_list_append(types, type);

	type = purple_status_type_new_with_attrs(
			PURPLE_STATUS_OFFLINE, NULL, NULL, TRUE, TRUE, FALSE,
			"message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
			NULL);
	types = g_list_append(types, type);

	return types;
}
/* }}} */

/* static GList *ggp_blist_node_menu(PurpleBlistNode *node) {{{ */
static GList *ggp_blist_node_menu(PurpleBlistNode *node)
{
	PurpleMenuAction *act;
	GList *m = NULL;

	if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
		return NULL;

	act = purple_menu_action_new(_("Add to chat"),
	                           PURPLE_CALLBACK(ggp_bmenu_add_to_chat),
	                           NULL, NULL);
	m = g_list_append(m, act);

	/* Using a blist node boolean here is also wrong.
	 * Once the Block and Unblock actions are added to the core,
	 * this will have to go. -- rlaager */
	if (purple_blist_node_get_bool(node, "blocked")) {
		act = purple_menu_action_new(_("Unblock"),
		                           PURPLE_CALLBACK(ggp_bmenu_block),
		                           NULL, NULL);
	} else {
		act = purple_menu_action_new(_("Block"),
		                           PURPLE_CALLBACK(ggp_bmenu_block),
		                           NULL, NULL);
	}
	m = g_list_append(m, act);

	return m;
}
/* }}} */

/* static GList *ggp_chat_info(PurpleConnection *gc) {{{ */
static GList *ggp_chat_info(PurpleConnection *gc)
{
	GList *m = NULL;
	struct proto_chat_entry *pce;

	pce = g_new0(struct proto_chat_entry, 1);
	pce->label = _("Chat _name:");
	pce->identifier = "name";
	pce->required = TRUE;
	m = g_list_append(m, pce);

	return m;
}
/* }}} */

/* static void ggp_login(PurpleAccount *account) {{{ */
static void ggp_login(PurpleAccount *account)
{
	PurpleConnection *gc;
	struct gg_login_params *glp;
	GGPInfo *info;

	if (ggp_setup_proxy(account) == -1)
		return;

	gc = purple_account_get_connection(account);
	glp = g_new0(struct gg_login_params, 1);
	info = g_new0(GGPInfo, 1);

	/* Probably this should be moved to *_new() function. */
	info->session = NULL;
	info->chats = NULL;
	info->chats_count = 0;
	info->token = NULL;
	info->searches = ggp_search_new();

	gc->proto_data = info;

	glp->uin = ggp_get_uin(account);
	glp->password = (char *)purple_account_get_password(account);

	glp->async = 1;
	glp->status = GG_STATUS_AVAIL;
	glp->tls = 0;

	info->session = gg_login(glp);
	if (info->session == NULL) {
		purple_connection_error(gc, _("Connection failed."));
		g_free(glp);
		return;
	}
	gc->inpa = purple_input_add(info->session->fd, PURPLE_INPUT_READ,
				  ggp_async_login_handler, gc);
}
/* }}} */

/* static void ggp_close(PurpleConnection *gc) {{{ */
static void ggp_close(PurpleConnection *gc)
{

	if (gc == NULL) {
		purple_debug_info("gg", "gc == NULL\n");
		return;
	}

	if (gc->proto_data) {
		PurpleAccount *account = purple_connection_get_account(gc);
		PurpleStatus *status;
		GGPInfo *info = gc->proto_data;

		status = purple_account_get_active_status(account);

		if (info->session != NULL) {
			ggp_set_status(account, status);
			gg_logoff(info->session);
			gg_free_session(info->session);
		}

		/* Immediately close any notifications on this handle since that process depends
		 * upon the contents of info->searches, which we are about to destroy.
		 */
		purple_notify_close_with_handle(gc);

		ggp_search_destroy(info->searches);
		g_free(info);
		gc->proto_data = NULL;
	}

	if (gc->inpa > 0)
		purple_input_remove(gc->inpa);

	ggp_buddylist_offline(gc);

	purple_debug_info("gg", "Connection closed.\n");
}
/* }}} */

/* static int ggp_send_im(PurpleConnection *gc, const char *who, const char *msg, PurpleMessageFlags flags) {{{ */
static int ggp_send_im(PurpleConnection *gc, const char *who, const char *msg,
		       PurpleMessageFlags flags)
{
	GGPInfo *info = gc->proto_data;
	char *tmp, *plain;
	int ret = 0;

	if (strlen(msg) == 0) {
		return 0;
	}

	purple_debug_info("gg", "ggp_send_im: msg = %s\n", msg);
	plain = purple_unescape_html(msg);
	tmp = charset_convert(plain, "UTF-8", "CP1250");

	if (NULL == tmp || strlen(tmp) == 0) {
		ret = 0;
	} else if (strlen(tmp) > GG_MSG_MAXSIZE) {
		ret = -E2BIG;
	} else if (gg_send_message(info->session, GG_CLASS_CHAT,
				ggp_str_to_uin(who), (unsigned char *)tmp) < 0) {
		ret = -1;
	} else {
		ret = 1;
	}

	g_free(plain);
	g_free(tmp);

	return ret;
}
/* }}} */

/* static void ggp_get_info(PurpleConnection *gc, const char *name) { {{{ */
static void ggp_get_info(PurpleConnection *gc, const char *name)
{
	GGPInfo *info = gc->proto_data;
	GGPSearchForm *form;
	guint32 seq;

	form = ggp_search_form_new(GGP_SEARCH_TYPE_INFO);

	form->user_data = info;
	form->uin = g_strdup(name);
	form->offset = g_strdup("0");
	form->last_uin = g_strdup("0");

	seq = ggp_search_start(gc, form);
	ggp_search_add(info->searches, seq, form);
}
/* }}} */

/* static void ggp_set_status(PurpleAccount *account, PurpleStatus *status) {{{ */
static void ggp_set_status(PurpleAccount *account, PurpleStatus *status)
{
	PurpleConnection *gc;
	GGPInfo *info;
	const char *status_id, *msg;
	int new_status, new_status_descr;

	if (!purple_status_is_active(status))
		return;

	gc = purple_account_get_connection(account);
	info = gc->proto_data;

	status_id = purple_status_get_id(status);

	purple_debug_info("gg", "ggp_set_status: Requested status = %s\n",
			status_id);

	if (strcmp(status_id, "available") == 0) {
		new_status = GG_STATUS_AVAIL;
		new_status_descr = GG_STATUS_AVAIL_DESCR;
	} else if (strcmp(status_id, "away") == 0) {
		new_status = GG_STATUS_BUSY;
		new_status_descr = GG_STATUS_BUSY_DESCR;
	} else if (strcmp(status_id, "invisible") == 0) {
		new_status = GG_STATUS_INVISIBLE;
		new_status_descr = GG_STATUS_INVISIBLE_DESCR;
	} else if (strcmp(status_id, "offline") == 0) {
		new_status = GG_STATUS_NOT_AVAIL;
		new_status_descr = GG_STATUS_NOT_AVAIL_DESCR;
	} else {
		new_status = GG_STATUS_AVAIL;
		new_status_descr = GG_STATUS_AVAIL_DESCR;
		purple_debug_info("gg",
			"ggp_set_status: uknown status requested (status_id=%s)\n",
			status_id);
	}

	msg = purple_status_get_attr_string(status, "message");

	if (msg == NULL) {
		gg_change_status(info->session, new_status);
	} else {
		gchar *tmp, *new_msg;

		tmp = charset_convert(msg, "UTF-8", "CP1250");
		new_msg = purple_markup_strip_html(tmp);
		g_free(tmp);

		gg_change_status_descr(info->session, new_status_descr, new_msg);
		g_free(new_msg);
	}
}
/* }}} */

/* static void ggp_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group) {{{ */
static void ggp_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group)
{
	GGPInfo *info = gc->proto_data;

	gg_add_notify(info->session, ggp_str_to_uin(buddy->name));
}
/* }}} */

/* static void ggp_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group) {{{ */
static void ggp_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
						 PurpleGroup *group)
{
	GGPInfo *info = gc->proto_data;

	gg_remove_notify(info->session, ggp_str_to_uin(buddy->name));
}
/* }}} */

/* static void ggp_join_chat(PurpleConnection *gc, GHashTable *data) {{{ */
static void ggp_join_chat(PurpleConnection *gc, GHashTable *data)
{
	GGPInfo *info = gc->proto_data;
	GGPChat *chat;
	char *chat_name;
	GList *l;
	PurpleConversation *conv;
	PurpleAccount *account = purple_connection_get_account(gc);

	chat_name = g_hash_table_lookup(data, "name");

	if (chat_name == NULL)
		return;

	purple_debug_info("gg", "joined %s chat\n", chat_name);

	for (l = info->chats; l != NULL; l = l->next) {
		 chat = l->data;

		 if (chat != NULL && g_utf8_collate(chat->name, chat_name) == 0) {
			 purple_notify_error(gc, _("Chat error"),
				 _("This chat name is already in use"), NULL);
			 return;
		 }
	}

	ggp_confer_add_new(gc, chat_name);
	conv = serv_got_joined_chat(gc, info->chats_count, chat_name);
	purple_conv_chat_add_user(PURPLE_CONV_CHAT(conv),
				purple_account_get_username(account), NULL,
				PURPLE_CBFLAGS_NONE, TRUE);
}
/* }}} */

/* static char *ggp_get_chat_name(GHashTable *data) { {{{ */
static char *ggp_get_chat_name(GHashTable *data) {
	return g_strdup(g_hash_table_lookup(data, "name"));
}
/* }}} */

/* static int ggp_chat_send(PurpleConnection *gc, int id, const char *message, PurpleMessageFlags flags) {{{ */
static int ggp_chat_send(PurpleConnection *gc, int id, const char *message, PurpleMessageFlags flags)
{
	PurpleConversation *conv;
	GGPInfo *info = gc->proto_data;
	GGPChat *chat = NULL;
	GList *l;
	char *msg, *plain;
	uin_t *uins;
	int count = 0;

	if ((conv = purple_find_chat(gc, id)) == NULL)
		return -EINVAL;

	for (l = info->chats; l != NULL; l = l->next) {
		chat = l->data;

		if (g_utf8_collate(chat->name, conv->name) == 0) {
			break;
		}

		chat = NULL;
	}

	if (chat == NULL) {
		purple_debug_error("gg",
			"ggp_chat_send: Hm... that's strange. No such chat?\n");
		return -EINVAL;
	}

	uins = g_new0(uin_t, g_list_length(chat->participants));

	for (l = chat->participants; l != NULL; l = l->next) {
		uin_t uin = GPOINTER_TO_INT(l->data);

		uins[count++] = uin;
	}

	plain = purple_unescape_html(message);
	msg = charset_convert(plain, "UTF-8", "CP1250");
	g_free(plain);
	gg_send_message_confer(info->session, GG_CLASS_CHAT, count, uins,
				(unsigned char *)msg);
	g_free(msg);
	g_free(uins);

	serv_got_chat_in(gc, id,
			 purple_account_get_username(purple_connection_get_account(gc)),
			 0, message, time(NULL));

	return 0;
}
/* }}} */

/* static void ggp_keepalive(PurpleConnection *gc) {{{ */
static void ggp_keepalive(PurpleConnection *gc)
{
	GGPInfo *info = gc->proto_data;

	/* purple_debug_info("gg", "Keeping connection alive....\n"); */

	if (gg_ping(info->session) < 0) {
		purple_debug_info("gg", "Not connected to the server "
				"or gg_session is not correct\n");
		purple_connection_error(gc, _("Not connected to the server."));
	}
}
/* }}} */

/* static void ggp_register_user(PurpleAccount *account) {{{ */
static void ggp_register_user(PurpleAccount *account)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	GGPInfo *info;

	info = gc->proto_data = g_new0(GGPInfo, 1);

	ggp_token_request(gc, ggp_register_user_dialog);
}
/* }}} */

/* static GList *ggp_actions(PurplePlugin *plugin, gpointer context) {{{ */
static GList *ggp_actions(PurplePlugin *plugin, gpointer context)
{
	GList *m = NULL;
	PurplePluginAction *act;

	act = purple_plugin_action_new(_("Find buddies..."),
				     ggp_find_buddies);
	m = g_list_append(m, act);

	m = g_list_append(m, NULL);

	act = purple_plugin_action_new(_("Change password..."),
				     ggp_change_passwd);
	m = g_list_append(m, act);

	m = g_list_append(m, NULL);

	act = purple_plugin_action_new(_("Upload buddylist to Server"),
				     ggp_action_buddylist_put);
	m = g_list_append(m, act);

	act = purple_plugin_action_new(_("Download buddylist from Server"),
				     ggp_action_buddylist_get);
	m = g_list_append(m, act);

	act = purple_plugin_action_new(_("Delete buddylist from Server"),
				     ggp_action_buddylist_delete);
	m = g_list_append(m, act);

	act = purple_plugin_action_new(_("Save buddylist to file..."),
				     ggp_action_buddylist_save);
	m = g_list_append(m, act);

	act = purple_plugin_action_new(_("Load buddylist from file..."),
				     ggp_action_buddylist_load);
	m = g_list_append(m, act);

	return m;
}
/* }}} */

/* static gboolean ggp_offline_message(const PurpleBuddy *buddy) {{{ */
static gboolean ggp_offline_message(const PurpleBuddy *buddy)
{
	return TRUE;
}
/* }}} */

/* prpl_info setup {{{ */
static PurplePluginProtocolInfo prpl_info =
{
	OPT_PROTO_REGISTER_NOSCREENNAME,
	NULL,				/* user_splits */
	NULL,				/* protocol_options */
	NO_BUDDY_ICONS,			/* icon_spec */
	ggp_list_icon,			/* list_icon */
	NULL,				/* list_emblem */
	ggp_status_text,		/* status_text */
	ggp_tooltip_text,		/* tooltip_text */
	ggp_status_types,		/* status_types */
	ggp_blist_node_menu,		/* blist_node_menu */
	ggp_chat_info,			/* chat_info */
	NULL,				/* chat_info_defaults */
	ggp_login,			/* login */
	ggp_close,			/* close */
	ggp_send_im,			/* send_im */
	NULL,				/* set_info */
	NULL,				/* send_typing */
	ggp_get_info,			/* get_info */
	ggp_set_status,			/* set_away */
	NULL,				/* set_idle */
	NULL,				/* change_passwd */
	ggp_add_buddy,			/* add_buddy */
	NULL,				/* add_buddies */
	ggp_remove_buddy,		/* remove_buddy */
	NULL,				/* remove_buddies */
	NULL,				/* add_permit */
	NULL,				/* add_deny */
	NULL,				/* rem_permit */
	NULL,				/* rem_deny */
	NULL,				/* set_permit_deny */
	ggp_join_chat,			/* join_chat */
	NULL,				/* reject_chat */
	ggp_get_chat_name,		/* get_chat_name */
	NULL,				/* chat_invite */
	NULL,				/* chat_leave */
	NULL,				/* chat_whisper */
	ggp_chat_send,			/* chat_send */
	ggp_keepalive,			/* keepalive */
	ggp_register_user,		/* register_user */
	NULL,				/* get_cb_info */
	NULL,				/* get_cb_away */
	NULL,				/* alias_buddy */
	NULL,				/* group_buddy */
	NULL,				/* rename_group */
	NULL,				/* buddy_free */
	NULL,				/* convo_closed */
	NULL,				/* normalize */
	NULL,				/* set_buddy_icon */
	NULL,				/* remove_group */
	NULL,				/* get_cb_real_name */
	NULL,				/* set_chat_topic */
	NULL,				/* find_blist_chat */
	NULL,				/* roomlist_get_list */
	NULL,				/* roomlist_cancel */
	NULL,				/* roomlist_expand_category */
	NULL,				/* can_receive_file */
	NULL,				/* send_file */
	NULL,				/* new_xfer */
	ggp_offline_message,		/* offline_message */
	NULL,				/* whiteboard_prpl_ops */
	NULL,				/* send_raw */
	NULL,				/* roomlist_room_serialize */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};
/* }}} */

/* PurplePluginInfo setup {{{ */
static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,		/* magic */
	PURPLE_MAJOR_VERSION,		/* major_version */
	PURPLE_MINOR_VERSION,		/* minor_version */
	PURPLE_PLUGIN_PROTOCOL,		/* plugin type */
	NULL,				/* ui_requirement */
	0,				/* flags */
	NULL,				/* dependencies */
	PURPLE_PRIORITY_DEFAULT,		/* priority */

	"prpl-gg",			/* id */
	"Gadu-Gadu",			/* name */
	VERSION,			/* version */

	N_("Gadu-Gadu Protocol Plugin"),	/* summary */
	N_("Polish popular IM"),		/* description */
	"boler@sourceforge.net",	/* author */
	PURPLE_WEBSITE,			/* homepage */

	NULL,				/* load */
	NULL,				/* unload */
	NULL,				/* destroy */

	NULL,				/* ui_info */
	&prpl_info,			/* extra_info */
	NULL,				/* prefs_info */
	ggp_actions,			/* actions */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};
/* }}} */

/* static void purple_gg_debug_handler(int level, const char * format, va_list args) {{{ */
static void purple_gg_debug_handler(int level, const char * format, va_list args) {
	PurpleDebugLevel purple_level;
	char *msg = g_strdup_vprintf(format, args);

	/* This is pretty pointless since the GG_DEBUG levels don't correspond to
	 * the purple ones */
	switch (level) {
		case GG_DEBUG_FUNCTION:
			purple_level = PURPLE_DEBUG_INFO;
			break;
		case GG_DEBUG_MISC:
		case GG_DEBUG_NET:
		case GG_DEBUG_DUMP:
		case GG_DEBUG_TRAFFIC:
		default:
			purple_level = PURPLE_DEBUG_MISC;
			break;
	}

	purple_debug(purple_level, "gg", msg);
	g_free(msg);
}
/* }}} */

/*
 */
/* static void init_plugin(PurplePlugin *plugin) {{{ */
static void init_plugin(PurplePlugin *plugin)
{
	PurpleAccountOption *option;

	option = purple_account_option_string_new(_("Nickname"),
			"nick", _("Gadu-Gadu User"));
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
						   option);

	my_protocol = plugin;

	gg_debug_handler = purple_gg_debug_handler;
}
/* }}} */

PURPLE_INIT_PLUGIN(gg, init_plugin, info);

/* vim: set ts=8 sts=0 sw=8 noet: */
