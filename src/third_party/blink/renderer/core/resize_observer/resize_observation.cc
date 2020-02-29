// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/resize_observer/resize_observation.h"
#include "third_party/blink/renderer/core/display_lock/display_lock_utilities.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/resize_observer/resize_observer.h"
#include "third_party/blink/renderer/core/resize_observer/resize_observer_box_options.h"
#include "third_party/blink/renderer/core/svg/svg_element.h"
#include "third_party/blink/renderer/core/svg/svg_graphics_element.h"

namespace blink {

ResizeObservation::ResizeObservation(Element* target,
                                     ResizeObserver* observer,
                                     ResizeObserverBoxOptions observed_box)
    : target_(target),
      observer_(observer),
      observation_size_(0, 0),
      observed_box_(observed_box) {
  DCHECK(target_);
}

bool ResizeObservation::ObservationSizeOutOfSync() {
  if (observation_size_ == ComputeTargetSize())
    return false;

  // Skip resize observations on locked elements.
  if (UNLIKELY(target_ && DisplayLockUtilities::IsInLockedSubtreeCrossingFrames(
                              *target_))) {
    return false;
  }

  return true;
}

void ResizeObservation::SetObservationSize(const LayoutSize& observation_size) {
  observation_size_ = observation_size;
}

size_t ResizeObservation::TargetDepth() {
  unsigned depth = 0;
  for (Element* parent = target_; parent; parent = parent->parentElement())
    ++depth;
  return depth;
}

LayoutSize ResizeObservation::ComputeTargetSize() const {
  if (target_) {
    if (LayoutObject* layout_object = target_->GetLayoutObject()) {
      // https://drafts.csswg.org/resize-observer/#calculate-box-size states
      // that the bounding box should be used for SVGGraphicsElements regardless
      // of the observed box.
      if (auto* svg_graphics_element =
              DynamicTo<SVGGraphicsElement>(target_.Get())) {
        return LayoutSize(svg_graphics_element->GetBBox().Size());
      }
      if (!layout_object->IsBox())
        return LayoutSize();

      if (LayoutBox* layout_box = ToLayoutBox(layout_object)) {
        switch (observed_box_) {
          case ResizeObserverBoxOptions::BorderBox:
            return LayoutSize(layout_box->LogicalWidth(),
                              layout_box->LogicalHeight());
          case ResizeObserverBoxOptions::ContentBox:
            return LayoutSize(layout_box->ContentLogicalWidth(),
                              layout_box->ContentLogicalHeight());
          default:
            NOTREACHED();
        }
      }
    }
  }
  return LayoutSize();
}

void ResizeObservation::Trace(Visitor* visitor) {
  visitor->Trace(target_);
  visitor->Trace(observer_);
}

}  // namespace blink
