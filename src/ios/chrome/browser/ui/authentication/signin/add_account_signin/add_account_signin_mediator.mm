// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/authentication/signin/add_account_signin/add_account_signin_mediator.h"

#import "base/strings/sys_string_conversions.h"
#import "components/prefs/pref_service.h"
#import "components/signin/public/base/signin_pref_names.h"
#import "components/signin/public/identity_manager/identity_manager.h"
#import "ios/chrome/browser/signin/signin_util.h"
#import "ios/public/provider/chrome/browser/signin/chrome_identity_interaction_manager.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using signin_metrics::AccessPoint;
using signin_metrics::PromoAction;

@interface AddAccountSigninMediator ()

// The coordinator's manager that handles interactions to add identities.
@property(nonatomic, weak)
    ChromeIdentityInteractionManager* identityInteractionManager;

// The Browser state's user-selected preferences.
@property(nonatomic, assign) PrefService* prefService;

// The Browser state's identity manager.
@property(nonatomic, assign) signin::IdentityManager* identityManager;

@end

@implementation AddAccountSigninMediator

#pragma mark - Public

- (instancetype)
    initWithIdentityInteractionManager:
        (ChromeIdentityInteractionManager*)identityInteractionManager
                           prefService:(PrefService*)prefService
                       identityManager:
                           (signin::IdentityManager*)identityManager {
  self = [super init];
  if (self) {
    _identityInteractionManager = identityInteractionManager;
    _prefService = prefService;
    _identityManager = identityManager;
  }
  return self;
}

- (void)handleSigninIntent:(SigninIntent)signinIntent
               accessPoint:(AccessPoint)accessPoint
               promoAction:(PromoAction)promoAction {
  switch (signinIntent) {
    case SigninIntentAddAccount: {
      [self addAccount];
      break;
    }
    case SigninIntentReauth: {
      signin_metrics::LogSigninAccessPointStarted(accessPoint, promoAction);
      [self reauthenticate];
      break;
    }
    default: {
      NOTREACHED();
    }
  }
}

#pragma mark - Private

// Completes the add account flow including handling any errors that have not
// been handled internally by ChromeIdentity. Will return the sign-in status of
// the user.
// |identity| is the identity of the added account.
// |error| is an error reported by the SSOAuth following adding an account.
- (SigninCoordinatorResult)signinStateWithIdentity:(ChromeIdentity*)identity
                                             error:(NSError*)error {
  DCHECK(self.identityInteractionManager);
  if (error) {
    DCHECK(!identity);
    // Filter out errors handled internally by ChromeIdentity.
    if (ShouldHandleSigninError(error)) {
      return SigninCoordinatorResultInterrupted;
    }
    return SigninCoordinatorResultCanceledByUser;
  }
  DCHECK(identity);
  return SigninCoordinatorResultSuccess;
}

// Handles the reauthentication operation for a user from the screen to enter
// credentials with the primary account email pre-entered to the user consent
// flow. In the case of a sign-in error will display a modal alert and abort the
// reauth flow.
- (void)reauthenticate {
  CoreAccountInfo accountInfo = self.identityManager->GetPrimaryAccountInfo();

  std::string email = accountInfo.email;
  std::string identity = accountInfo.gaia;
  if (email.empty() || identity.empty()) {
    // This corresponds to a re-authenticate request after the user was signed
    // out. This corresponds to the case where the identity was removed as a
    // result of the permissions being removed on the server or the identity
    // being removed from another app.
    //
    // Simply use the the last signed-in user email in this case and go though
    // the entire sign-in flow as sync needs to be configured.
    email = self.prefService->GetString(prefs::kGoogleServicesLastUsername);
    identity = self.prefService->GetString(prefs::kGoogleServicesLastAccountId);
  }
  DCHECK(!email.empty());
  DCHECK(!identity.empty());
  __weak AddAccountSigninMediator* weakSelf = self;
  [self.identityInteractionManager
      reauthenticateUserWithID:base::SysUTF8ToNSString(identity)
                         email:base::SysUTF8ToNSString(email)
                    completion:^(ChromeIdentity* chromeIdentity,
                                 NSError* error) {
                      [weakSelf operationCompletedWithIdentity:chromeIdentity
                                                         error:error];
                    }];
}

// Handles the reauthentication or add account operation if the flow has not
// been interrupted by a sign-in error.
- (void)operationCompletedWithIdentity:(ChromeIdentity*)identity
                                 error:(NSError*)error {
  SigninCoordinatorResult signinResult = [self signinStateWithIdentity:identity
                                                                 error:error];
  switch (signinResult) {
    case SigninCoordinatorResultSuccess:
    case SigninCoordinatorResultCanceledByUser: {
      [self.delegate addAccountSigninMediatorFinishedWith:signinResult
                                                 identity:identity];
      break;
    }
    case SigninCoordinatorResultInterrupted: {
      [self.delegate addAccountSigninMediatorFailedWith:error];
      break;
    }
  }
}

// Handles the add account operation for a user. Presents the screen to enter
// credentials for the added account. In the case of a sign-in error will
// display a modal alert and abort the add account flow.
- (void)addAccount {
  __weak AddAccountSigninMediator* weakSelf = self;
  [self.identityInteractionManager
      addAccountWithCompletion:^(ChromeIdentity* identity, NSError* error) {
        [weakSelf operationCompletedWithIdentity:identity error:error];
      }];
}

@end
