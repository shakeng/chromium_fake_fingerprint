// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.payments.handler;

import androidx.annotation.VisibleForTesting;

import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeVersionInfo;
import org.chromium.chrome.browser.WebContentsFactory;
import org.chromium.chrome.browser.flags.ChromeFeatureList;
import org.chromium.chrome.browser.payments.PaymentsExperimentalFeatures;
import org.chromium.chrome.browser.payments.handler.toolbar.PaymentHandlerToolbarCoordinator;
import org.chromium.chrome.browser.widget.bottomsheet.BottomSheetController;
import org.chromium.components.embedder_support.view.ContentView;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.ViewAndroidDelegate;
import org.chromium.ui.modelutil.PropertyModel;
import org.chromium.ui.modelutil.PropertyModelChangeProcessor;
import org.chromium.url.URI;

/**
 * PaymentHandler coordinator, which owns the component overall, i.e., creates other objects in the
 * component and connects them. It decouples the implementation of this component from other
 * components and acts as the point of contact between them. Any code in this component that needs
 * to interact with another component does that through this coordinator.
 */
public class PaymentHandlerCoordinator {
    private Runnable mHider;
    private WebContents mWebContents;
    private PaymentHandlerToolbarCoordinator mToolbarCoordinator;

    /** Constructs the payment-handler component coordinator. */
    public PaymentHandlerCoordinator() {
        assert isEnabled();
    }

    /** Observes the state changes of the payment-handler UI. */
    public interface PaymentHandlerUiObserver {
        /** Called when Payment Handler UI is closed. */
        void onPaymentHandlerUiClosed();
        /** Called when Payment Handler UI is shown. */
        void onPaymentHandlerUiShown();
    }

    /** Observes the WebContents of the payment-handler UI. */
    public interface PaymentHandlerWebContentsObserver {
        /**
         * Called when the WebContents has been initialized.
         * @param webContents The WebContents of the PaymentHandler.
         */
        void onWebContentsInitialized(WebContents webContents);
    }

    /**
     * Shows the payment-handler UI.
     *
     * @param chromeActivity The activity where the UI should be shown.
     * @param url The url of the payment handler app, i.e., that of
     *         "PaymentRequestEvent.openWindow(url)".
     * @param isIncognito Whether the tab is in incognito mode.
     * @param webContentsObserver The observer of the WebContents of the
     *         PaymentHandler.
     * @param uiObserver The {@link PaymentHandlerUiObserver} that observes this Payment Handler UI.
     * @return Whether the payment-handler UI was shown. Can be false if the UI was suppressed.
     */
    public boolean show(ChromeActivity activity, URI url, boolean isIncognito,
            PaymentHandlerWebContentsObserver webContentsObserver,
            PaymentHandlerUiObserver uiObserver) {
        assert mHider == null : "Already showing payment-handler UI";

        mWebContents = WebContentsFactory.createWebContents(isIncognito, /*initiallyHidden=*/false);
        ContentView webContentView = ContentView.createContentView(activity, mWebContents);
        mWebContents.initialize(ChromeVersionInfo.getProductVersion(),
                ViewAndroidDelegate.createBasicDelegate(webContentView), webContentView,
                activity.getWindowAndroid(), WebContents.createDefaultInternalsHolder());
        webContentsObserver.onWebContentsInitialized(mWebContents);
        mWebContents.getNavigationController().loadUrl(new LoadUrlParams(url.toString()));

        mToolbarCoordinator = new PaymentHandlerToolbarCoordinator(activity, mWebContents, url);

        PropertyModel model = new PropertyModel.Builder(PaymentHandlerProperties.ALL_KEYS).build();
        PaymentHandlerMediator mediator = new PaymentHandlerMediator(model, this::hide,
                mWebContents, uiObserver, activity.getActivityTab().getView(),
                mToolbarCoordinator.getView(), mToolbarCoordinator.getShadowHeightPx());
        activity.getActivityTab().getView().addOnLayoutChangeListener(mediator);
        BottomSheetController bottomSheetController = activity.getBottomSheetController();
        bottomSheetController.addObserver(mediator);
        mWebContents.addObserver(mediator);

        // Observer is designed to set here rather than in the constructor because
        // PaymentHandlerMediator and PaymentHandlerToolbarCoordinator have mutual dependencies.
        mToolbarCoordinator.setObserver(mediator);
        PaymentHandlerView view = new PaymentHandlerView(
                activity, mWebContents, webContentView, mToolbarCoordinator.getView());
        assert mToolbarCoordinator.getToolbarHeightPx() == view.getToolbarHeightPx();
        PropertyModelChangeProcessor changeProcessor =
                PropertyModelChangeProcessor.create(model, view, PaymentHandlerViewBinder::bind);
        mHider = () -> {
            changeProcessor.destroy();
            bottomSheetController.removeObserver(mediator);
            bottomSheetController.hideContent(/*content=*/view, /*animate=*/true);
            mWebContents.destroy();
            uiObserver.onPaymentHandlerUiClosed();
            activity.getActivityTab().getView().removeOnLayoutChangeListener(mediator);
            mediator.destroy();
        };
        return bottomSheetController.requestShowContent(view, /*animate=*/true);
    }

    /**
     * Get the WebContents of the Payment Handler for testing purpose. In other situations,
     * WebContents should not be leaked outside the Payment Handler.
     *
     * @return The WebContents of the Payment Handler.
     */
    @VisibleForTesting
    public WebContents getWebContentsForTest() {
        return mWebContents;
    }

    /** Hides the payment-handler UI. */
    public void hide() {
        if (mHider == null) return;
        mHider.run();
        mHider = null;
    }

    /**
     * @return Whether this solution (as opposed to the Chrome-custom-tab based solution) of
     *     PaymentHandler is enabled. This solution is intended to replace the other
     *     solution.
     */
    public static boolean isEnabled() {
        // Enabling the flag of either ScrollToExpand or PaymentsExperimentalFeatures will enable
        // this feature.
        return PaymentsExperimentalFeatures.isEnabled(
                ChromeFeatureList.SCROLL_TO_EXPAND_PAYMENT_HANDLER);
    }

    @VisibleForTesting
    public void clickSecurityIconForTest() {
        mToolbarCoordinator.clickSecurityIconForTest();
    }
}
