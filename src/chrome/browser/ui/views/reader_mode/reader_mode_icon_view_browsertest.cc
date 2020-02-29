// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/macros.h"
#include "chrome/browser/ui/views/reader_mode/reader_mode_icon_view.h"

#include "base/test/scoped_feature_list.h"
#include "chrome/browser/ui/page_action/page_action_icon_type.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/dom_distiller/content/browser/distillable_page_utils.h"
#include "components/dom_distiller/core/distilled_page_prefs.h"
#include "components/dom_distiller/core/dom_distiller_features.h"
#include "components/dom_distiller/core/dom_distiller_switches.h"
#include "components/dom_distiller/core/pref_names.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/views/test/button_test_api.h"

namespace {

const char* kSimpleArticlePath = "/dom_distiller/simple_article.html";
const char* kNonArticlePath = "/dom_distiller/non_og_article.html";

class TestDistillabilityObserver
    : public dom_distiller::DistillabilityObserver {
 public:
  explicit TestDistillabilityObserver(content::WebContents* web_contents)
      : web_contents_(web_contents),
        run_loop_(std::make_unique<base::RunLoop>()) {
    dom_distiller::AddObserver(web_contents_, this);
  }

  ~TestDistillabilityObserver() override {
    dom_distiller::RemoveObserver(web_contents_, this);
  }

  TestDistillabilityObserver(const TestDistillabilityObserver&) = delete;
  TestDistillabilityObserver& operator=(const TestDistillabilityObserver&) =
      delete;

  // Returns immediately if the result has already happened, otherwise
  // waits until that result is observed.
  void WaitForResult(const dom_distiller::DistillabilityResult& result) {
    if (WasResultFound(result)) {
      results_.clear();
      return;
    }

    result_to_wait_for_ = result;
    run_loop_->Run();
    run_loop_ = std::make_unique<base::RunLoop>();
    results_.clear();
  }

 private:
  void OnResult(const dom_distiller::DistillabilityResult& result) override {
    results_.push_back(result);
    // If we aren't waiting for anything yet, return early.
    if (!result_to_wait_for_.has_value())
      return;
    // Check if this is the one we were waiting for, and if so, stop the loop.
    if (DistillabilityResultsEqual(result, result_to_wait_for_.value()))
      run_loop_->Quit();
  }

  bool DistillabilityResultsEqual(
      const dom_distiller::DistillabilityResult& first,
      const dom_distiller::DistillabilityResult& second) {
    return first.is_distillable == second.is_distillable &&
           first.is_last == second.is_last &&
           first.is_mobile_friendly == second.is_mobile_friendly;
  }

  bool WasResultFound(const dom_distiller::DistillabilityResult& result) {
    for (auto& elem : results_) {
      if (DistillabilityResultsEqual(result, elem))
        return true;
    }
    return false;
  }

  content::WebContents* web_contents_;
  std::unique_ptr<base::RunLoop> run_loop_;
  base::Optional<dom_distiller::DistillabilityResult> result_to_wait_for_;
  std::vector<dom_distiller::DistillabilityResult> results_;
};

class ReaderModeIconViewBrowserTest : public InProcessBrowserTest {
 protected:
  ReaderModeIconViewBrowserTest() {
    feature_list_.InitAndEnableFeature(dom_distiller::kReaderMode);
  }

  void SetUpOnMainThread() override {
    ASSERT_TRUE(embedded_test_server()->Start());
    reader_mode_icon_ =
        BrowserView::GetBrowserViewForBrowser(browser())
            ->toolbar_button_provider()
            ->GetPageActionIconView(PageActionIconType::kReaderMode);
    ASSERT_NE(nullptr, reader_mode_icon_);
  }

  PageActionIconView* reader_mode_icon_;

 private:
  base::test::ScopedFeatureList feature_list_;

  DISALLOW_COPY_AND_ASSIGN(ReaderModeIconViewBrowserTest);
};

// TODO(gilmanmh): Add tests for the following cases:
//  * Icon is visible on the distilled page.
//  * Icon is not visible on about://blank, both initially and after navigating
//    to a distillable page.
IN_PROC_BROWSER_TEST_F(ReaderModeIconViewBrowserTest,
                       IconVisibilityAdaptsToPageContents) {
  TestDistillabilityObserver observer(
      browser()->tab_strip_model()->GetActiveWebContents());
  dom_distiller::DistillabilityResult expected_result;
  expected_result.is_distillable = false;
  expected_result.is_last = false;
  expected_result.is_mobile_friendly = false;

  // The icon should not be visible by default, before navigation to any page
  // has occurred.
  const bool is_visible_before_navigation = reader_mode_icon_->GetVisible();
  EXPECT_FALSE(is_visible_before_navigation);

  // The icon should be hidden on pages that aren't distillable
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(kNonArticlePath));
  observer.WaitForResult(expected_result);
  const bool is_visible_on_non_distillable_page =
      reader_mode_icon_->GetVisible();
  EXPECT_FALSE(is_visible_on_non_distillable_page);

  // The icon should appear after navigating to a distillable article.
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(kSimpleArticlePath));
  expected_result.is_distillable = true;
  observer.WaitForResult(expected_result);
  const bool is_visible_on_article = reader_mode_icon_->GetVisible();
  EXPECT_TRUE(is_visible_on_article);

  // Navigating back to a non-distillable page hides the icon again.
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(kNonArticlePath));
  expected_result.is_distillable = false;
  observer.WaitForResult(expected_result);
  const bool is_visible_after_navigation_back_to_non_distillable_page =
      reader_mode_icon_->GetVisible();
  EXPECT_FALSE(is_visible_after_navigation_back_to_non_distillable_page);
}

class ReaderModeIconViewBrowserTestWithSettings
    : public ReaderModeIconViewBrowserTest {
 protected:
  ReaderModeIconViewBrowserTestWithSettings() {
    feature_list_.InitAndEnableFeatureWithParameters(
        dom_distiller::kReaderMode,
        {{switches::kReaderModeDiscoverabilityParamName,
          switches::kReaderModeOfferInSettings}});
  }

  void SetOfferReaderModeSetting(bool value) {
    browser()->profile()->GetPrefs()->SetBoolean(
        dom_distiller::prefs::kOfferReaderMode, value);
  }

 private:
  base::test::ScopedFeatureList feature_list_;

  DISALLOW_COPY_AND_ASSIGN(ReaderModeIconViewBrowserTestWithSettings);
};

IN_PROC_BROWSER_TEST_F(ReaderModeIconViewBrowserTestWithSettings,
                       IconVisibilityDependsOnSettingIfExperimentEnabled) {
  SetOfferReaderModeSetting(false);

  TestDistillabilityObserver observer(
      browser()->tab_strip_model()->GetActiveWebContents());
  dom_distiller::DistillabilityResult expected_result;
  expected_result.is_distillable = true;
  expected_result.is_last = false;
  expected_result.is_mobile_friendly = false;

  // The icon should not appear after navigating to a distillable article,
  // because the setting to offer reader mode is disabled.
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(kSimpleArticlePath));
  observer.WaitForResult(expected_result);
  bool is_visible_on_article = reader_mode_icon_->GetVisible();
  EXPECT_FALSE(is_visible_on_article);

  // It continues to not show up when navigating to a non-distillable page.
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(kNonArticlePath));
  expected_result.is_distillable = false;
  observer.WaitForResult(expected_result);
  bool is_visible_after_navigation_back_to_non_distillable_page =
      reader_mode_icon_->GetVisible();
  EXPECT_FALSE(is_visible_after_navigation_back_to_non_distillable_page);

  // If we turn on the setting, the icon should start to show up on a
  // distillable page.
  SetOfferReaderModeSetting(true);
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(kSimpleArticlePath));
  expected_result.is_distillable = true;
  observer.WaitForResult(expected_result);
  is_visible_on_article = reader_mode_icon_->GetVisible();
  EXPECT_TRUE(is_visible_on_article);

  // But it still turns off when navigating to a non-distillable page.
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(kNonArticlePath));
  expected_result.is_distillable = false;
  observer.WaitForResult(expected_result);
  is_visible_after_navigation_back_to_non_distillable_page =
      reader_mode_icon_->GetVisible();
  EXPECT_FALSE(is_visible_after_navigation_back_to_non_distillable_page);
}

}  // namespace
