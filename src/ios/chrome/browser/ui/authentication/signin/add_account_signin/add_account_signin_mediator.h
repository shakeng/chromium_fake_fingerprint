// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_AUTHENTICATION_SIGNIN_ADD_ACCOUNT_SIGNIN_ADD_ACCOUNT_SIGNIN_MEDIATOR_H_
#define IOS_CHROME_BROWSER_UI_AUTHENTICATION_SIGNIN_ADD_ACCOUNT_SIGNIN_ADD_ACCOUNT_SIGNIN_MEDIATOR_H_

#import "components/signin/public/base/signin_metrics.h"
#import "ios/chrome/browser/ui/authentication/signin/signin_enums.h"

@class ChromeIdentity;
@class ChromeIdentityInteractionManager;
class PrefService;

namespace signin {
class IdentityManager;
}

// Delegate that displays screens for the add account and reauthentication
// flows.
@protocol AddAccountSigninMediatorDelegate

// Shows alert modal dialog and interrupts sign-in operation.
// |error| is the error to be displayed.
- (void)addAccountSigninMediatorFailedWith:(NSError*)error;

// Completes the sign-in operation.
// |signinResult| is the state of sign-in at add account flow completion.
// |identity| is the identity of the added account.
- (void)addAccountSigninMediatorFinishedWith:
            (SigninCoordinatorResult)signinResult
                                    identity:(ChromeIdentity*)identity;

@end

// Mediator that handles add account and reauthentication operations.
@interface AddAccountSigninMediator : NSObject

- (instancetype)init NS_UNAVAILABLE;
- (instancetype)
    initWithIdentityInteractionManager:
        (ChromeIdentityInteractionManager*)identityInteractionManager
                           prefService:(PrefService*)prefService
                       identityManager:(signin::IdentityManager*)identityManager
    NS_DESIGNATED_INITIALIZER;

// The delegate.
@property(nonatomic, weak) id<AddAccountSigninMediatorDelegate> delegate;

// Handles the sign-in operation.
// |signinIntent| specifies the sign-in intent, either adding an account or
// reauthentication. |accessPoint| is the view where the sign-in button was
// displayed. |promoAction| is promo button used to trigger the sign-in.
- (void)handleSigninIntent:(SigninIntent)signinIntent
               accessPoint:(signin_metrics::AccessPoint)accessPoint
               promoAction:(signin_metrics::PromoAction)promoAction;

@end

#endif  // IOS_CHROME_BROWSER_UI_AUTHENTICATION_SIGNIN_ADD_ACCOUNT_SIGNIN_ADD_ACCOUNT_SIGNIN_MEDIATOR_H_
