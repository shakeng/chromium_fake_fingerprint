// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.webapps;

import android.content.Intent;
import android.os.Bundle;

import org.chromium.chrome.browser.flags.ActivityType;
import org.chromium.chrome.browser.util.IntentUtils;
import org.chromium.content_public.browser.WebContents;
import org.chromium.webapk.lib.common.WebApkConstants;

/**
 * An Activity is designed for WebAPKs (native Android apps) and displays a webapp in a nearly
 * UI-less Chrome.
 */
public class WebApkActivity extends WebappActivity {
    private static final String TAG = "WebApkActivity";

    @Override
    public @WebappScopePolicy.Type int scopePolicy() {
        return WebappScopePolicy.Type.STRICT;
    }

    @Override
    protected WebappInfo createWebappInfo(Intent intent) {
        return (intent == null) ? WebApkInfo.createEmpty() : WebApkInfo.create(intent);
    }

    @Override
    protected void initializeUI(Bundle savedInstance) {
        super.initializeUI(savedInstance);
        WebContents webContents = getActivityTab().getWebContents();
        if (webContents != null) webContents.notifyRendererPreferenceUpdate();
    }

    @Override
    public boolean shouldPreferLightweightFre(Intent intent) {
        // We cannot use getWebApkPackageName() because
        // {@link WebappActivity#performPreInflationStartup()} may not have been called yet.
        String webApkPackageName =
                IntentUtils.safeGetStringExtra(intent, WebApkConstants.EXTRA_WEBAPK_PACKAGE_NAME);

        // Use the lightweight FRE for unbound WebAPKs.
        return webApkPackageName != null
                && !webApkPackageName.startsWith(WebApkConstants.WEBAPK_PACKAGE_PREFIX);
    }

    @Override
    protected void onDestroyInternal() {
        // The common case is to be connected to just one WebAPK's services. For the sake of
        // simplicity disconnect from the services of all WebAPKs.
        ChromeWebApkHost.disconnectFromAllServices(true /* waitForPendingWork */);

        super.onDestroyInternal();
    }

    @Override
    protected boolean loadUrlIfPostShareTarget(WebappInfo webappInfo) {
        WebApkInfo webApkInfo = (WebApkInfo) webappInfo;
        WebApkInfo.ShareData shareData = webApkInfo.shareData();
        if (shareData == null) {
            return false;
        }
        return new WebApkPostShareTargetNavigator().navigateIfPostShareTarget(
                webApkInfo.url(), webApkInfo.shareTarget(), shareData,
                getActivityTab().getWebContents());
    }

    @Override
    @ActivityType
    public int getActivityType() {
        return ActivityType.WEB_APK;
    }
}
