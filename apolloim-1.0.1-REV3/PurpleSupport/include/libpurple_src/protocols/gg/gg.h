/**
 * @file gg.h
 *
 * purple
 *
 * Copyright (C) 2005  Bartosz Oler <bartosz@bzimage.us>
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


#ifndef _PURPLE_GG_H
#define _PURPLE_GG_H

#include <libgadu.h>
#include "internal.h"
#include "search.h"
#include "connection.h"


#define PUBDIR_RESULTS_MAX 20


typedef struct
{
	char *name;
	GList *participants;

} GGPChat;

typedef void (*GGPTokenCallback)(PurpleConnection *);

typedef struct
{
	char *id;
	char *data;
	unsigned int size;

	struct gg_http *req;
	guint inpa;

	GGPTokenCallback cb;

} GGPToken;

typedef struct {

	struct gg_session *session;
	GGPToken *token;
	GList *chats;
	GGPSearches *searches;

	uin_t tmp_buddy;
	int chats_count;

} GGPInfo;


#endif /* _PURPLE_GG_H */

/* vim: set ts=8 sts=0 sw=8 noet: */
