// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/cloud_content_scanning/deep_scanning_browsertest_base.h"
#include "chrome/browser/safe_browsing/cloud_content_scanning/deep_scanning_dialog_views.h"
#include "chrome/browser/safe_browsing/cloud_content_scanning/fake_deep_scanning_dialog_delegate.h"
#include "chrome/browser/safe_browsing/dm_token_utils.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/testing_browser_process.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/features.h"

namespace safe_browsing {

namespace {

constexpr char kDmToken[] = "dm_token";

constexpr base::TimeDelta kInitialUIDelay =
    base::TimeDelta::FromMilliseconds(100);
constexpr base::TimeDelta kMinimumPendingDelay =
    base::TimeDelta::FromMilliseconds(400);
constexpr base::TimeDelta kSuccessTimeout =
    base::TimeDelta::FromMilliseconds(100);

}  // namespace

DeepScanningBrowserTestBase::DeepScanningBrowserTestBase() {
  // Enable every deep scanning features.
  scoped_feature_list_.InitWithFeatures(
      {kContentComplianceEnabled, kMalwareScanEnabled,
       kDeepScanningOfUploadsUI},
      {});

  // Change the time values of the upload UI to smaller ones to make tests
  // showing it run faster.
  DeepScanningDialogViews::SetInitialUIDelayForTesting(kInitialUIDelay);
  DeepScanningDialogViews::SetMinimumPendingDialogTimeForTesting(
      kMinimumPendingDelay);
  DeepScanningDialogViews::SetSuccessDialogTimeoutForTesting(kSuccessTimeout);
}

DeepScanningBrowserTestBase::~DeepScanningBrowserTestBase() = default;

void DeepScanningBrowserTestBase::TearDownOnMainThread() {
  DeepScanningDialogDelegate::ResetFactoryForTesting();

  SetDlpPolicy(CheckContentComplianceValues::CHECK_NONE);
  SetMalwarePolicy(SendFilesForMalwareCheckValues::DO_NOT_SCAN);
  SetWaitPolicy(DelayDeliveryUntilVerdictValues::DELAY_NONE);
}

void DeepScanningBrowserTestBase::SetDlpPolicy(
    CheckContentComplianceValues state) {
  g_browser_process->local_state()->SetInteger(prefs::kCheckContentCompliance,
                                               state);
}

void DeepScanningBrowserTestBase::SetMalwarePolicy(
    SendFilesForMalwareCheckValues state) {
  browser()->profile()->GetPrefs()->SetInteger(
      prefs::kSafeBrowsingSendFilesForMalwareCheck, state);
}

void DeepScanningBrowserTestBase::SetWaitPolicy(
    DelayDeliveryUntilVerdictValues state) {
  g_browser_process->local_state()->SetInteger(
      prefs::kDelayDeliveryUntilVerdict, state);
}

void DeepScanningBrowserTestBase::SetUpDelegate() {
  SetDMTokenForTesting(policy::DMToken::CreateValidTokenForTesting(kDmToken));
  DeepScanningDialogDelegate::SetFactoryForTesting(base::BindRepeating(
      &FakeDeepScanningDialogDelegate::Create, base::DoNothing(),
      base::Bind(&DeepScanningBrowserTestBase::StatusCallback,
                 base::Unretained(this)),
      base::Bind(&DeepScanningBrowserTestBase::EncryptionStatusCallback,
                 base::Unretained(this)),
      kDmToken));
}

void DeepScanningBrowserTestBase::SetQuitClosure(
    base::RepeatingClosure quit_closure) {
  quit_closure_ = quit_closure;
}

void DeepScanningBrowserTestBase::CallQuitClosure() {
  if (!quit_closure_.is_null())
    quit_closure_.Run();
}

void DeepScanningBrowserTestBase::SetStatusCallbackResponse(
    DeepScanningClientResponse response) {
  status_callback_response_ = response;
}

DeepScanningClientResponse DeepScanningBrowserTestBase::StatusCallback(
    const base::FilePath& path) {
  return status_callback_response_;
}

bool DeepScanningBrowserTestBase::EncryptionStatusCallback(
    const base::FilePath& path) {
  return false;
}

}  // namespace safe_browsing
