/*
 ApolloTOC.m: Objective-C firetalk interface.
 By Alex C. Schaefer

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
#import <UIKit/UIAnimator.h>
#import "StartView.h"

extern UIApplication *UIApp;

@interface ApolloIMApp : UIApplication 
{
    UIWindow		*_window;
    StartView		*startView;
	
}
-(void)resetIdles;
- (void)applicationDidFinishLaunching:(NSNotification *)aNotification;
- (void)applicationWillTerminate;
- (void)applicationSuspend:(struct __GSEvent *)event;
- (void)applicationResume:(struct __GSEvent *)event;
- (BOOL)isSuspendingUnderLock;
- (BOOL)applicationIsReadyToSuspend;
- (BOOL) suspendRemainInMemory;
- (void)ringerChanged:(int)fp8;
//- (void)applicationDidResumeFromUnderLock;
//- (void)applicationWillSuspendUnderLock;
@end
