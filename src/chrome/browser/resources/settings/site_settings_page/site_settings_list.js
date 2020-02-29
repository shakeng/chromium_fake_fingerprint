// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('settings', function() {
  /**
   * @param {string} setting Value from settings.ContentSetting.
   * @param {string} enabled Non-block label ('feature X not allowed').
   * @param {string} disabled Block label (likely just, 'Blocked').
   * @param {?string} other Tristate value (maybe, 'session only').
   */
  function defaultSettingLabel(setting, enabled, disabled, other) {
    if (setting == settings.ContentSetting.BLOCK) {
      return disabled;
    }
    if (setting == settings.ContentSetting.ALLOW) {
      return enabled;
    }

    return other || enabled;
  }

  Polymer({
    is: 'settings-site-settings-list',

    behaviors: [WebUIListenerBehavior, I18nBehavior],

    properties: {
      /** @type {!Array<!settings.CategoryListItem>} */
      categoryList: Array,

      /** @type {!Map<string, (string|Function)>} */
      focusConfig: {
        type: Object,
        observer: 'focusConfigChanged_',
      },
    },

    /** @type {?settings.SiteSettingsPrefsBrowserProxy} */
    browserProxy: null,

    /**
     * @param {!Map<string, string>} newConfig
     * @param {?Map<string, string>} oldConfig
     * @private
     */
    focusConfigChanged_(newConfig, oldConfig) {
      // focusConfig is set only once on the parent, so this observer should
      // only fire once.
      assert(!oldConfig);

      // Populate the |focusConfig| map of the parent <settings-animated-pages>
      // element, with additional entries that correspond to subpage trigger
      // elements residing in this element's Shadow DOM.
      for (const item of this.categoryList) {
        const route = settings.routes[item.route];
        this.focusConfig.set(route.path, () => this.async(() => {
          cr.ui.focusWithoutInk(assert(this.$$(`#${item.id}`)));
        }));
      }
    },

    /** @override */
    ready() {
      this.browserProxy_ =
          settings.SiteSettingsPrefsBrowserProxyImpl.getInstance();

      for (const item of this.categoryList) {
        // Default labels are not applicable to ZOOM_LEVELS or PDF.
        if (item.id === settings.ContentSettingsTypes.ZOOM_LEVELS ||
            item.id === 'pdfDocuments') {
          continue;
        }

        this.refreshDefaultValueLabel_(item.id);
      }

      this.addWebUIListener(
          'contentSettingCategoryChanged',
          this.refreshDefaultValueLabel_.bind(this));

      const hasProtocolHandlers = this.categoryList.some(item => {
        return item.id === settings.ContentSettingsTypes.PROTOCOL_HANDLERS;
      });

      if (hasProtocolHandlers) {
        // The protocol handlers have a separate enabled/disabled notifier.
        this.addWebUIListener('setHandlersEnabled', enabled => {
          this.updateDefaultValueLabel_(
              settings.ContentSettingsTypes.PROTOCOL_HANDLERS,
              enabled ? settings.ContentSetting.ALLOW :
                        settings.ContentSetting.BLOCK);
        });
        this.browserProxy_.observeProtocolHandlersEnabledState();
      }
    },

    /**
     * @param {!settings.ContentSettingsTypes} category The category to refresh
     *     (fetch current value + update UI)
     * @private
     */
    refreshDefaultValueLabel_(category) {
      this.browserProxy_.getDefaultValueForContentType(category).then(
          defaultValue => {
            this.updateDefaultValueLabel_(category, defaultValue.setting);
          });
    },

    /**
     * Updates the DOM for the given |category| to display a label that
     * corresponds to the given |setting|.
     * @param {!settings.ContentSettingsTypes} category
     * @param {!settings.ContentSetting} setting
     * @private
     */
    updateDefaultValueLabel_(category, setting) {
      const element = this.$$(`#${category}`);
      if (!element) {
        // |category| is not part of this list.
        return;
      }

      const index = this.$$('dom-repeat').indexForElement(element);
      const dataItem = this.categoryList[index];
      this.set(
          `categoryList.${index}.subLabel`,
          defaultSettingLabel(
              setting,
              dataItem.enabledLabel ? this.i18n(dataItem.enabledLabel) : '',
              dataItem.disabledLabel ? this.i18n(dataItem.disabledLabel) : '',
              dataItem.otherLabel ? this.i18n(dataItem.otherLabel) : null));
    },

    /**
     * @param {!Event} event
     * @private
     */
    onClick_(event) {
      const dataSet =
          /** @type {{route: string}} */ (event.currentTarget.dataset);
      this.fire('site-settings-item-click', dataSet.route);
    },
  });

  // #cr_define_end
  return {defaultSettingLabel};
});
