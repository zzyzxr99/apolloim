//
//  BBTOC-Callback.h
//  BoomBot
//
//  Created by Josh on Sun Mar 02 2003.
//

/*
 BBTOC-Callback.h: Objective-C firetalk interface callbacks.
 Part of BoomBot.
 Copyright (C) 2006 Joshua Watzman.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

void ft_callback_error(void *connection, void *clientstruct, int error, char *roomoruser);
void ft_callback_connectfailed(void *c, void *cs, int error, char *reason);
void ft_callback_doinit (void *c, void *cs, char *nickname);
void ft_callback_getmessage(void *c, void *cs, const char * const who, const int automessage, const char * const message);
// void ft_callback_setidle(void *c, long *idle);
void ft_callback_buddyonline(void *c, void *cs, char *nickname, char *group);
void ft_callback_buddyoffline(void *c, void *cs, char *nickname, char *group);
void ft_callback_listbuddy(void *c, void *cs,char *nickname, char *group, char online, char away, long idletime);
void ft_callback_disconnect(void *c, void *cs, const int error);
void ft_callback_needpass(void *c, void *cs, char *p, const int size);
void ft_callback_storepass(char* newpass);
void ft_callback_action(void *connection, void *clientstruct, char *sender, int automessage_flag, char *message);
