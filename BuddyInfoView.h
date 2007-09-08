/*
 ApolloTOC.m: Objective-C firetalk interface.
 By Adam Bellmore

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

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UIKit/UITransitionView.h>
#import <UIKit/UIScroller.h>
#import <UIKit/UITextView.h>
#import "Buddy.h"

@interface BuddyInfoView : UIScroller 
{
	Buddy * buddy;
	CGRect _rect;

	UITextLabel * buddy_name_label;
	UITextLabel * idle_time_label;
	UITextLabel * status_label;
	UITextLabel * info_label;

	UITextView * info_text;
}

- (id)initWithFrame:(struct CGRect)frame withBuddy:(Buddy*)aBuddy andDelegate:(id)delegate;
- (void)dealloc;
- (void) reloadData;
- (Buddy *) buddy;

@end
