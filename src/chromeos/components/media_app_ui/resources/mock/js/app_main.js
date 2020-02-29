// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * Provides a mock of http://go/media-app/index.ts which is pre-built and
 * brought in via DEPS to ../../app/js/app_main.js. Runs in an isolated guest.
 */

/**
 * Helper that returns UI that can serve as an effective mock of a fragment of
 * the real app, after loading a provided Blob URL.
 *
 * @typedef{function(string): Promise<!HTMLElement>}}
 */
var ModuleHandler;

/** @type{ModuleHandler} */
const createVideoChild = async (blobSrc) => {
  const container = /** @type{!HTMLElement} */ (
      document.createElement('backlight-video-container'));
  const video =
      /** @type{HTMLVideoElement} */ (document.createElement('video'));
  video.src = blobSrc;
  container.attachShadow({mode: 'open'});
  container.shadowRoot.appendChild(video);
  return container;
};

/** @type{ModuleHandler} */
const createImgChild = async (blobSrc) => {
  const img = /** @type{!HTMLImageElement} */ (document.createElement('img'));
  img.src = blobSrc;
  await img.decode();
  return img;
};

/**
 * A mock app used for testing when src-internal is not available.
 * @implements mediaApp.ClientApi
 */
class BacklightApp extends HTMLElement {
  constructor() {
    super();
    this.currentMedia =
        /** @type{!HTMLElement} */ (document.createElement('img'));
    this.appendChild(this.currentMedia);
  }

  /** @override  */
  async loadFiles(files) {
    const file = files.item(0);
    const factory =
        file.mimeType.match('^video/') ? createVideoChild : createImgChild;

    // Note the mock app will just leak this Blob URL.
    const child = await factory(URL.createObjectURL(file.blob));

    // Simulate an app that shows one image at a time.
    this.replaceChild(child, this.currentMedia);
    this.currentMedia = child;
  }
}
window.customElements.define('backlight-app', BacklightApp);

class VideoContainer extends HTMLElement {}
window.customElements.define('backlight-video-container', VideoContainer);

document.addEventListener('DOMContentLoaded', () => {
  // The "real" app first loads translations for populating strings in the app
  // for the initial load, then does this.
  document.body.appendChild(new BacklightApp());
});
