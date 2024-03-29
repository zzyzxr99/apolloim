/*
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
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
 *
 */


#include "internal.h"

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "debug.h"
#include "util.h"
#include "version.h"
#include "yahoo.h"
#include "yahoo_packet.h"


/**
 * The additional protocol specific info attached to each buddy.  We need
 * to store the unique numeric id number to allow us to push alias changes.
 */
struct YahooUser
{
    const char *id;             /* The yahoo accountid for this buddy (not YahooID but numeric value) */
    char *firstname;            /* Storing this information for no real reason, just because */
    char *lastname;             /* Storing this information for no real reason, just because */
    char *nickname;             /* Storing this information for no real reason, just because */
};

void yahoo_update_alias(PurpleConnection *gc, const char *who, const char *alias);
void yahoo_fetch_aliases(PurpleConnection *gc);
