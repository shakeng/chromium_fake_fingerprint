// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/permissions/permissions_client.h"

#include "base/callback.h"
#include "build/build_config.h"
#include "components/permissions/notification_permission_ui_selector.h"

#if !defined(OS_ANDROID)
#include "ui/gfx/paint_vector_icon.h"
#endif

namespace permissions {
namespace {
PermissionsClient* g_client = nullptr;
}

PermissionsClient::PermissionsClient() {
  DCHECK(!g_client);
  g_client = this;
}

PermissionsClient::~PermissionsClient() {
  g_client = nullptr;
}

// static
PermissionsClient* PermissionsClient::Get() {
  DCHECK(g_client);
  return g_client;
}

double PermissionsClient::GetSiteEngagementScore(
    content::BrowserContext* browser_context,
    const GURL& origin) {
  return 0.0;
}

void PermissionsClient::GetUkmSourceId(content::BrowserContext* browser_context,
                                       const content::WebContents* web_contents,
                                       const GURL& requesting_origin,
                                       GetUkmSourceIdCallback callback) {
  std::move(callback).Run(base::nullopt);
}

PermissionRequest::IconId PermissionsClient::GetOverrideIconId(
    ContentSettingsType type) {
#if defined(OS_ANDROID)
  return 0;
#else
  return gfx::kNoneIcon;
#endif
}

std::unique_ptr<NotificationPermissionUiSelector>
PermissionsClient::CreateNotificationPermissionUiSelector(
    content::BrowserContext* browser_context) {
  return nullptr;
}

void PermissionsClient::OnPromptResolved(
    content::BrowserContext* browser_context,
    PermissionRequestType request_type,
    PermissionAction action) {}

base::Optional<url::Origin> PermissionsClient::GetAutoApprovalOrigin() {
  return base::nullopt;
}

bool PermissionsClient::CanBypassEmbeddingOriginCheck(
    const GURL& requesting_origin,
    const GURL& embedding_origin) {
  return false;
}

}  // namespace permissions
