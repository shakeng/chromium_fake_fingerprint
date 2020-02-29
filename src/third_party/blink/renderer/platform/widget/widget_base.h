// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_WIDGET_WIDGET_BASE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_WIDGET_WIDGET_BASE_H_

#include "third_party/blink/renderer/platform/platform_export.h"

namespace cc {
class AnimationHost;
class LayerTreeHost;
}  // namespace cc

namespace blink {

// This class is the foundational class for all widgets that blink creates.
// (WebPagePopupImpl, WebFrameWidgetBase) will contain an instance of this
// class. For simplicity purposes this class will be a member of those classes.
// It will eventually host compositing, input and emulation. See design doc:
// https://docs.google.com/document/d/10uBnSWBaitGsaROOYO155Wb83rjOPtrgrGTrQ_pcssY/edit?ts=5e3b26f7
class PLATFORM_EXPORT WidgetBase {
 public:
  WidgetBase();
  virtual ~WidgetBase();

  // Set the current compositor hosts.
  void SetCompositorHosts(cc::LayerTreeHost*, cc::AnimationHost*);

  cc::AnimationHost* AnimationHost() const;
  cc::LayerTreeHost* LayerTreeHost() const;

 private:
  // Not owned, they are owned by the RenderWidget.
  cc::LayerTreeHost* layer_tree_host_ = nullptr;
  cc::AnimationHost* animation_host_ = nullptr;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_WIDGET_WIDGET_BASE_H_
