// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/storage_access_api/storage_access_grant_permission_context.h"

#include "base/test/scoped_feature_list.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/permissions/permission_request_id.h"
#include "components/permissions/permission_request_manager.h"
#include "components/permissions/test/mock_permission_prompt_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"

namespace {

constexpr char kInsecureURL[] = "http://www.example.com";
constexpr char kSecureURL[] = "https://www.example.com";

void SaveResult(ContentSetting* content_setting_result,
                ContentSetting content_setting) {
  DCHECK(content_setting_result);
  *content_setting_result = content_setting;
}

}  // namespace

class StorageAccessGrantPermissionContextTest
    : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();

    // Ensure we are navigated to some page so that the proper views get setup.
    NavigateAndCommit(GURL(kInsecureURL));

    // Create PermissionRequestManager.
    permissions::PermissionRequestManager::CreateForWebContents(web_contents());

    mock_permission_prompt_factory_ =
        std::make_unique<permissions::MockPermissionPromptFactory>(
            permissions::PermissionRequestManager::FromWebContents(
                web_contents()));
  }

  void TearDown() override {
    mock_permission_prompt_factory_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }

 private:
  std::unique_ptr<permissions::MockPermissionPromptFactory>
      mock_permission_prompt_factory_;
};

TEST_F(StorageAccessGrantPermissionContextTest, InsecureOriginsAreAllowed) {
  StorageAccessGrantPermissionContext permission_context(profile());
  EXPECT_TRUE(permission_context.IsPermissionAvailableToOrigins(
      GURL(kInsecureURL), GURL(kInsecureURL)));
  EXPECT_TRUE(permission_context.IsPermissionAvailableToOrigins(
      GURL(kInsecureURL), GURL(kSecureURL)));
}

// When the Storage Access API feature is disabled we should block the
// permission request.
TEST_F(StorageAccessGrantPermissionContextTest,
       PermissionBlockedWhenFeatureDisabled) {
  base::test::ScopedFeatureList scoped_disable;
  scoped_disable.InitAndDisableFeature(blink::features::kStorageAccessAPI);

  StorageAccessGrantPermissionContext permission_context(profile());
  permissions::PermissionRequestID fake_id(
      /*render_process_id=*/0, /*render_frame_id=*/0, /*request_id=*/0);

  ContentSetting result = CONTENT_SETTING_DEFAULT;
  permission_context.DecidePermission(
      web_contents(), fake_id, GURL(kSecureURL), GURL(kSecureURL),
      /*user_gesture=*/true, base::BindOnce(&SaveResult, &result));
  EXPECT_EQ(CONTENT_SETTING_BLOCK, result);
}

// When the Storage Access API feature is enabled and we have a user gesture we
// should get a decision.
TEST_F(StorageAccessGrantPermissionContextTest,
       PermissionDecidedWhenFeatureEnabled) {
  base::test::ScopedFeatureList scoped_enable;
  scoped_enable.InitAndEnableFeature(blink::features::kStorageAccessAPI);

  StorageAccessGrantPermissionContext permission_context(profile());
  permissions::PermissionRequestID fake_id(
      /*render_process_id=*/0, /*render_frame_id=*/0, /*request_id=*/0);

  ContentSetting result = CONTENT_SETTING_DEFAULT;
  permission_context.DecidePermission(
      web_contents(), fake_id, GURL(kSecureURL), GURL(kSecureURL),
      /*user_gesture=*/true, base::BindOnce(&SaveResult, &result));
  base::RunLoop().RunUntilIdle();

  // We should get a prompt showing up right now.
  permissions::PermissionRequestManager* manager =
      permissions::PermissionRequestManager::FromWebContents(web_contents());
  DCHECK(manager);
  EXPECT_TRUE(manager->IsRequestInProgress());

  // Close the prompt and validate we get the expected setting back in our
  // callback.
  manager->Closing();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(CONTENT_SETTING_ASK, result);
}

// No user gesture should force a permission rejection.
TEST_F(StorageAccessGrantPermissionContextTest,
       PermissionDeniedWithoutUserGesture) {
  base::test::ScopedFeatureList scoped_enable;
  scoped_enable.InitAndEnableFeature(blink::features::kStorageAccessAPI);

  StorageAccessGrantPermissionContext permission_context(profile());
  permissions::PermissionRequestID fake_id(
      /*render_process_id=*/0, /*render_frame_id=*/0, /*request_id=*/0);

  ContentSetting result = CONTENT_SETTING_DEFAULT;
  permission_context.DecidePermission(
      web_contents(), fake_id, GURL(kSecureURL), GURL(kSecureURL),
      /*user_gesture=*/false, base::BindOnce(&SaveResult, &result));
  EXPECT_EQ(CONTENT_SETTING_BLOCK, result);
}

TEST_F(StorageAccessGrantPermissionContextTest,
       PermissionStatusBlockedWhenFeatureDisabled) {
  base::test::ScopedFeatureList scoped_disable;
  scoped_disable.InitAndDisableFeature(blink::features::kStorageAccessAPI);

  StorageAccessGrantPermissionContext permission_context(profile());

  EXPECT_EQ(CONTENT_SETTING_BLOCK,
            permission_context
                .GetPermissionStatus(/*render_frame_host=*/nullptr,
                                     GURL(kSecureURL), GURL(kSecureURL))
                .content_setting);
}

TEST_F(StorageAccessGrantPermissionContextTest,
       PermissionStatusAsksWhenFeatureEnabled) {
  base::test::ScopedFeatureList scoped_enable;
  scoped_enable.InitAndEnableFeature(blink::features::kStorageAccessAPI);

  StorageAccessGrantPermissionContext permission_context(profile());

  EXPECT_EQ(CONTENT_SETTING_ASK,
            permission_context
                .GetPermissionStatus(/*render_frame_host=*/nullptr,
                                     GURL(kSecureURL), GURL(kSecureURL))
                .content_setting);
}
