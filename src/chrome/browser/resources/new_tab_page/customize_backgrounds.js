// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './grid.js';

import {html, PolymerElement} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js';
import {BrowserProxy} from './browser_proxy.js';

/** Element that lets the user configure the background. */
class CustomizeBackgroundsElement extends PolymerElement {
  static get is() {
    return 'ntp-customize-backgrounds';
  }

  static get template() {
    return html`{__html_template__}`;
  }

  static get properties() {
    return {
      /** @private {!Array<!newTabPage.mojom.BackgroundCollection>} */
      collections_: Array,
    };
  }

  constructor() {
    super();
    BrowserProxy.getInstance().handler.getBackgroundCollections().then(
        ({collections}) => {
          this.collections_ = collections;
        });
  }
}

customElements.define(
    CustomizeBackgroundsElement.is, CustomizeBackgroundsElement);
