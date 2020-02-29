// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * 'crostini-subpage' is the settings subpage for managing Crostini.
 */

Polymer({
  is: 'settings-crostini-subpage',

  behaviors:
      [PrefsBehavior, WebUIListenerBehavior, settings.RouteOriginBehavior],

  properties: {
    /** Preferences state. */
    prefs: {
      type: Object,
      notify: true,
    },

    /**
     * Whether export / import UI should be displayed.
     * @private {boolean}
     */
    showCrostiniExportImport_: {
      type: Boolean,
      value() {
        return loadTimeData.getBoolean('showCrostiniExportImport');
      },
    },

    /** @private {boolean} */
    showArcAdbSideloading_: {
      type: Boolean,
      computed: 'and_(isArcAdbSideloadingSupported_, isAndroidEnabled_)',
    },

    /** @private {boolean} */
    isArcAdbSideloadingSupported_: {
      type: Boolean,
      value() {
        return loadTimeData.getBoolean('arcAdbSideloadingSupported');
      },
    },

    /** @private {boolean} */
    showCrostiniPortForwarding_: {
      type: Boolean,
      value() {
        return loadTimeData.getBoolean('showCrostiniPortForwarding');
      },
    },

    /** @private {boolean} */
    isAndroidEnabled_: {
      type: Boolean,
    },

    /**
     * Whether the uninstall options should be displayed.
     * @private {boolean}
     */
    hideCrostiniUninstall_: {
      type: Boolean,
      computed: 'or_(installerShowing_, upgraderDialogShowing_)',
    },

    /**
     * Whether the button to launch the Crostini container upgrade flow should
     * be shown.
     * @private {boolean}
     */
    showCrostiniContainerUpgrade_: {
      type: Boolean,
      value() {
        return loadTimeData.getBoolean('showCrostiniContainerUpgrade');
      },
    },

    /**
     * Whether the button to show the disk resizing view should be shown.
     * @private {boolean}
     */
    showCrostiniDiskResize_: {
      type: Boolean,
      value() {
        return loadTimeData.getBoolean('showCrostiniDiskResize');
      },
    },

    /*
     * Whether the installer is showing.
     * @private {boolean}
     */
    installerShowing_: {
      type: Boolean,
    },

    /**
     * Whether the upgrader dialog is showing.
     * @private {boolean}
     */
    upgraderDialogShowing_: {
      type: Boolean,
    },

    /**
     * Whether the button to launch the Crostini container upgrade flow should
     * be disabled.
     * @private {boolean}
     */
    disableUpgradeButton_: {
      type: Boolean,
      computed: 'or_(installerShowing_, upgraderDialogShowing_)',
    }
  },

  /** settings.RouteOriginBehavior override */
  route_: settings.routes.CROSTINI_DETAILS,

  observers: [
    'onCrostiniEnabledChanged_(prefs.crostini.enabled.value)',
    'onArcEnabledChanged_(prefs.arc.enabled.value)'
  ],

  attached() {
    this.addWebUIListener('crostini-installer-status-changed', (status) => {
      this.installerShowing_ = status;
    });
    this.addWebUIListener('crostini-upgrader-status-changed', (status) => {
      this.upgraderDialogShowing_ = status;
    });
    settings.CrostiniBrowserProxyImpl.getInstance()
        .requestCrostiniInstallerStatus();
    settings.CrostiniBrowserProxyImpl.getInstance()
        .requestCrostiniUpgraderDialogStatus();
  },

  ready() {
    const r = settings.routes;
    this.addFocusConfig_(r.CROSTINI_SHARED_PATHS, '#crostini-shared-paths');
    this.addFocusConfig_(
        r.CROSTINI_SHARED_USB_DEVICES, '#crostini-shared-usb-devices');
    this.addFocusConfig_(r.CROSTINI_EXPORT_IMPORT, '#crostini-export-import');
    this.addFocusConfig_(r.CROSTINI_ANDROID_ADB, '#crostini-enable-arc-adb');
    this.addFocusConfig_(
        r.CROSTINI_PORT_FORWARDING, '#crostini-port-forwarding');
    this.addFocusConfig_(r.CROSTINI_DISK_RESIZE, '#crostini-disk-resize');
  },

  /** @private */
  onCrostiniEnabledChanged_(enabled) {
    if (!enabled &&
        settings.Router.getInstance().getCurrentRoute() ==
            settings.routes.CROSTINI_DETAILS) {
      settings.Router.getInstance().navigateToPreviousRoute();
    }
  },

  /** @private */
  onArcEnabledChanged_(enabled) {
    this.isAndroidEnabled_ = enabled;
  },

  /** @private */
  onExportImportClick_() {
    settings.Router.getInstance().navigateTo(
        settings.routes.CROSTINI_EXPORT_IMPORT);
  },

  /** @private */
  onEnableArcAdbClick_() {
    settings.Router.getInstance().navigateTo(
        settings.routes.CROSTINI_ANDROID_ADB);
  },

  /** @private */
  onShowDiskResizeClick_() {
    settings.Router.getInstance().navigateTo(
        settings.routes.CROSTINI_DISK_RESIZE);
  },

  /**
   * Shows a confirmation dialog when removing crostini.
   * @private
   */
  onRemoveClick_() {
    settings.CrostiniBrowserProxyImpl.getInstance().requestRemoveCrostini();
    settings.recordSettingChange();
  },

  /**
   * Shows the upgrade flow dialog.
   * @private
   */
  onContainerUpgradeClick_() {
    settings.CrostiniBrowserProxyImpl.getInstance()
        .requestCrostiniContainerUpgradeView();
  },

  /** @private */
  onSharedPathsClick_() {
    settings.Router.getInstance().navigateTo(
        settings.routes.CROSTINI_SHARED_PATHS);
  },

  /** @private */
  onSharedUsbDevicesClick_() {
    settings.Router.getInstance().navigateTo(
        settings.routes.CROSTINI_SHARED_USB_DEVICES);
  },

  /** @private */
  onPortForwardingClick_: function() {
    settings.Router.getInstance().navigateTo(
        settings.routes.CROSTINI_PORT_FORWARDING);
  },

  /**
   * @private
   * @param {boolean} a
   * @param {boolean} b
   * @return {boolean}
   */
  and_: function(a, b) {
    return a && b;
  },

  /**
   * @private
   * @param {boolean} a
   * @param {boolean} b
   * @return {boolean}
   */
  or_: function(a, b) {
    return a || b;
  },
});
