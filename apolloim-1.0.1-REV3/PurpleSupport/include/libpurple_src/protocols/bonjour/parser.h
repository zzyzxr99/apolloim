/**
 * @file parser.h Bonjour Jabber XML parser functions
 *
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
 */
#ifndef _PURPLE_BONJOUR_PARSER_H_
#define _PURPLE_BONJOUR_PARSER_H_

#include "buddy.h"
#include "jabber.h"

void bonjour_parser_setup(BonjourJabberConversation *bconv);
void bonjour_parser_process(PurpleBuddy *pb, const char *buf, int len);

#endif /* _PURPLE_BONJOUR_PARSER_H_ */
