// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.settings.website;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.NativeMethods;
import org.chromium.chrome.browser.profiles.Profile;

/**
 * Communicates between CookieControlsService (C++ backend) and observers in the Incognito NTP Java
 * UI.
 */
public class CookieControlsServiceBridge {
    /**
     * Interface for a class that wants to receive cookie controls updates from
     * CookieControlsServiceBridge.
     */
    public interface CookieControlsServiceObserver {
        /**
         * Called when there is an update in the cookie controls that should be reflected in the UI.
         * @param checked A boolean indicating whether the toggle indicating third-party cookies are
         *         currently being blocked should be checked or not.
         * @param enforced A boolean indicating if third-party cookies being blocked is currently
         *         enforced by policy/cookie settings or not.
         */
        public void sendCookieControlsUIChanges(boolean checked, boolean enforced);
    }

    private long mNativeCookieControlsServiceBridge;
    private CookieControlsServiceObserver mObserver;

    /**
     * Initializes a CookieControlsServiceBridge instance.
     * @param observer An observer to call with updates from the cookie controls service.
     * @param profile The Profile instance to observe.
     */
    public CookieControlsServiceBridge(CookieControlsServiceObserver observer, Profile profile) {
        mObserver = observer;
        mNativeCookieControlsServiceBridge = CookieControlsServiceBridgeJni.get().init(
                CookieControlsServiceBridge.this, profile);
    }

    /**
     * Destroys the native counterpart of this class.
     */
    public void destroy() {
        if (mNativeCookieControlsServiceBridge != 0) {
            CookieControlsServiceBridgeJni.get().destroy(
                    mNativeCookieControlsServiceBridge, CookieControlsServiceBridge.this);
            mNativeCookieControlsServiceBridge = 0;
        }
    }

    /**
     * Updates the CookieControlsService on the status of the toggle, and thus the state of
     * third-party cookie blocking in incognito.
     * @param enable A boolean indicating whether the toggle has been switched on or off.
     */
    public void handleCookieControlsToggleChanged(boolean enable) {
        CookieControlsServiceBridgeJni.get().handleCookieControlsToggleChanged(
                mNativeCookieControlsServiceBridge, enable);
    }

    @CalledByNative
    private void sendCookieControlsUIChanges(boolean checked, boolean enforced) {
        mObserver.sendCookieControlsUIChanges(checked, enforced);
    }

    @NativeMethods
    interface Natives {
        long init(CookieControlsServiceBridge caller, Profile profile);
        void destroy(long nativeCookieControlsServiceBridge, CookieControlsServiceBridge caller);
        void handleCookieControlsToggleChanged(
                long nativeCookieControlsServiceBridge, boolean enable);
    }
}
