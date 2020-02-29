// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ANDROID_VR_ARCORE_DEVICE_ARCORE_PLANE_MANAGER_H_
#define CHROME_BROWSER_ANDROID_VR_ARCORE_DEVICE_ARCORE_PLANE_MANAGER_H_

#include <map>

#include "base/util/type_safety/id_type.h"
#include "base/util/type_safety/pass_key.h"
#include "chrome/browser/android/vr/arcore_device/arcore_sdk.h"
#include "chrome/browser/android/vr/arcore_device/scoped_arcore_objects.h"
#include "device/vr/public/mojom/vr_service.mojom.h"

namespace device {

class ArCoreImpl;

using PlaneId = util::IdTypeU64<class PlaneTag>;

std::pair<gfx::Quaternion, gfx::Point3F> GetPositionAndOrientationFromArPose(
    const ArSession* session,
    const ArPose* pose);

device::mojom::Pose GetMojomPoseFromArPose(const ArSession* session,
                                           const ArPose* pose);

device::internal::ScopedArCoreObject<ArPose*> GetArPoseFromMojomPose(
    const ArSession* session,
    const device::mojom::Pose& pose);

class ArCorePlaneManager {
 public:
  ArCorePlaneManager(util::PassKey<ArCoreImpl> pass_key,
                     ArSession* arcore_session);
  ~ArCorePlaneManager();

  // Updates plane manager state - it should be called in every frame if the
  // ARCore session supports plane detection. Currently, if the WebXR session
  // supports hit test feature or plane detection feature, the ARCore session
  // needs to be configured with planes enabled and this method needs to be
  // called.
  void Update(ArFrame* ar_frame);

  mojom::XRPlaneDetectionDataPtr GetDetectedPlanesData() const;

  bool PlaneExists(PlaneId id) const;

  // Returns base::nullopt if plane with the given address does not exist.
  base::Optional<PlaneId> GetPlaneId(void* plane_address) const;

  // Returns base::nullopt if plane with the given id does not exist.
  base::Optional<gfx::Transform> GetMojoFromPlane(PlaneId id) const;

  // Creates Anchor object given a plane ID. This is needed since Plane objects
  // are managed by this class in its entirety and are not accessible outside
  // it.
  device::internal::ScopedArCoreObject<ArAnchor*> CreateAnchor(
      PlaneId id,
      const device::mojom::Pose& pose) const;

 private:
  // Executes |fn| for each still tracked, non-subsumed plane present in
  // |arcore_planes|. |fn| will receive 2 parameters, an owning
  // `ScopedArCoreObject<ArAnchor*>`, and, for convenience, the non-owning
  // ArPlane* typecast from the first parameter.
  template <typename FunctionType>
  void ForEachArCorePlane(ArTrackableList* arcore_planes, FunctionType fn);

  // Owned by ArCoreImpl - non-owning pointer is fine since ArCorePlaneManager
  // is also owned by ArCoreImpl.
  ArSession* arcore_session_;

  // List of trackables - used for retrieving planes detected by ARCore.
  // Allows reuse of the list across updates; ARCore clears the list on each
  // call to the ARCore SDK.
  internal::ScopedArCoreObject<ArTrackableList*> arcore_planes_;
  // Allows reuse of the pose object; ARCore will populate it with new data on
  // each call to the ARCore SDK.
  internal::ScopedArCoreObject<ArPose*> ar_pose_;

  uint64_t next_id_ = 1;
  // Returns tuple containing plane id and a boolean signifying that the
  // plane was created. It should be called only during calls to |Update()|.
  std::pair<PlaneId, bool> CreateOrGetPlaneId(void* plane_address);
  // Mapping from plane address to plane ID. It should be modified only during
  // calls to |Update()|.
  std::map<void*, PlaneId> ar_plane_address_to_id_;
  // Mapping from plane ID to ARCore plane object. It should be modified only
  // during calls to |Update()|.
  std::map<PlaneId, device::internal::ScopedArCoreObject<ArTrackable*>>
      plane_id_to_plane_object_;
  // Set containing IDs of planes updated in the last frame. It should be
  // modified only during calls to |Update()|.
  std::set<PlaneId> updated_plane_ids_;
};

}  // namespace device

#endif  // CHROME_BROWSER_ANDROID_VR_ARCORE_DEVICE_ARCORE_PLANE_MANAGER_H_
