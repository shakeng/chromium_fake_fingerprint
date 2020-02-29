// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/vr/arcore_device/arcore_plane_manager.h"

#include "base/stl_util.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/android/vr/arcore_device/type_converters.h"

namespace device {

std::pair<gfx::Quaternion, gfx::Point3F> GetPositionAndOrientationFromArPose(
    const ArSession* session,
    const ArPose* pose) {
  std::array<float, 7> pose_raw;  // 7 = orientation(4) + position(3).
  ArPose_getPoseRaw(session, pose, pose_raw.data());

  return {gfx::Quaternion(pose_raw[0], pose_raw[1], pose_raw[2], pose_raw[3]),
          gfx::Point3F(pose_raw[4], pose_raw[5], pose_raw[6])};
}

device::mojom::Pose GetMojomPoseFromArPose(const ArSession* session,
                                           const ArPose* pose) {
  device::mojom::Pose result;
  std::tie(result.orientation, result.position) =
      GetPositionAndOrientationFromArPose(session, pose);

  return result;
}

device::internal::ScopedArCoreObject<ArPose*> GetArPoseFromMojomPose(
    const ArSession* session,
    const device::mojom::Pose& pose) {
  float pose_raw[7] = {};  // 7 = orientation(4) + position(3).

  pose_raw[0] = pose.orientation.x();
  pose_raw[1] = pose.orientation.y();
  pose_raw[2] = pose.orientation.z();
  pose_raw[3] = pose.orientation.w();

  pose_raw[4] = pose.position.x();
  pose_raw[5] = pose.position.y();
  pose_raw[6] = pose.position.z();

  device::internal::ScopedArCoreObject<ArPose*> result;

  ArPose_create(
      session, pose_raw,
      device::internal::ScopedArCoreObject<ArPose*>::Receiver(result).get());

  return result;
}

ArCorePlaneManager::ArCorePlaneManager(util::PassKey<ArCoreImpl> pass_key,
                                       ArSession* arcore_session)
    : arcore_session_(arcore_session) {
  DCHECK(arcore_session_);

  ArTrackableList_create(
      arcore_session_,
      internal::ScopedArCoreObject<ArTrackableList*>::Receiver(arcore_planes_)
          .get());
  DCHECK(arcore_planes_.is_valid());

  ArPose_create(
      arcore_session_, nullptr,
      internal::ScopedArCoreObject<ArPose*>::Receiver(ar_pose_).get());
  DCHECK(ar_pose_.is_valid());
}

ArCorePlaneManager::~ArCorePlaneManager() = default;

template <typename FunctionType>
void ArCorePlaneManager::ForEachArCorePlane(ArTrackableList* arcore_planes,
                                            FunctionType fn) {
  DCHECK(arcore_planes);

  int32_t trackable_list_size;
  ArTrackableList_getSize(arcore_session_, arcore_planes, &trackable_list_size);

  DVLOG(2) << __func__ << ": arcore_planes size=" << trackable_list_size;

  for (int i = 0; i < trackable_list_size; i++) {
    internal::ScopedArCoreObject<ArTrackable*> trackable;
    ArTrackableList_acquireItem(
        arcore_session_, arcore_planes, i,
        internal::ScopedArCoreObject<ArTrackable*>::Receiver(trackable).get());

    ArTrackingState tracking_state;
    ArTrackable_getTrackingState(arcore_session_, trackable.get(),
                                 &tracking_state);

    if (tracking_state != ArTrackingState::AR_TRACKING_STATE_TRACKING) {
      // Skip all planes that are not currently tracked.
      continue;
    }

#if DCHECK_IS_ON()
    {
      ArTrackableType type;
      ArTrackable_getType(arcore_session_, trackable.get(), &type);
      DCHECK(type == ArTrackableType::AR_TRACKABLE_PLANE)
          << "arcore_planes contains a trackable that is not an ArPlane!";
    }
#endif

    ArPlane* ar_plane =
        ArAsPlane(trackable.get());  // Naked pointer is fine here, ArAsPlane
                                     // does not increase ref count and the
                                     // object is owned by `trackable`.

    internal::ScopedArCoreObject<ArPlane*> subsuming_plane;
    ArPlane_acquireSubsumedBy(
        arcore_session_, ar_plane,
        internal::ScopedArCoreObject<ArPlane*>::Receiver(subsuming_plane)
            .get());

    if (subsuming_plane.is_valid()) {
      // Current plane was subsumed by other plane, skip this loop iteration.
      // Subsuming plane will be handled when its turn comes.
      continue;
    }
    // Pass the ownership of the |trackable| to the |fn|, along with the
    // |ar_plane| that points to the |trackable| but with appropriate type.
    fn(std::move(trackable), ar_plane);
  }
}

void ArCorePlaneManager::Update(ArFrame* ar_frame) {
  ArTrackableType plane_tracked_type = AR_TRACKABLE_PLANE;

  // First, ask ARCore about all Plane trackables updated in the current frame.
  ArFrame_getUpdatedTrackables(arcore_session_, ar_frame, plane_tracked_type,
                               arcore_planes_.get());

  // Collect the IDs of the updated planes. |ar_plane_address_to_id_| might
  // grow.
  std::set<PlaneId> updated_plane_ids;
  ForEachArCorePlane(
      arcore_planes_.get(),
      [this, &updated_plane_ids](
          internal::ScopedArCoreObject<ArTrackable*> trackable,
          ArPlane* ar_plane) {
        // ID
        PlaneId plane_id;
        bool created;
        std::tie(plane_id, created) = CreateOrGetPlaneId(ar_plane);

        DVLOG(3) << "Previously detected plane found, id=" << plane_id
                 << ", created?=" << created;

        updated_plane_ids.insert(plane_id);
      });

  DVLOG(3) << __func__
           << ": updated_plane_ids.size()=" << updated_plane_ids.size();

  // Then, ask about all Plane trackables that are still tracked and
  // non-subsumed.
  ArSession_getAllTrackables(arcore_session_, plane_tracked_type,
                             arcore_planes_.get());

  // Collect the objects of all currently tracked planes.
  // |ar_plane_address_to_id_| should *not* grow.
  std::map<PlaneId, device::internal::ScopedArCoreObject<ArTrackable*>>
      plane_id_to_plane_object;
  ForEachArCorePlane(
      arcore_planes_.get(),
      [this, &plane_id_to_plane_object](
          internal::ScopedArCoreObject<ArTrackable*> trackable,
          ArPlane* ar_plane) {
        // ID
        PlaneId plane_id;
        bool created;
        std::tie(plane_id, created) = CreateOrGetPlaneId(ar_plane);

        DCHECK(!created)
            << "Newly detected planes should already be handled - new plane id="
            << plane_id;

        plane_id_to_plane_object[plane_id] = std::move(trackable);
      });

  DVLOG(3) << __func__ << ": plane_id_to_plane_object.size()="
           << plane_id_to_plane_object.size();

  // Shrink |ar_plane_address_to_id_|, removing all planes that are no longer
  // tracked or were subsumed - if they do not show up in
  // |plane_id_to_plane_object| map, they are no longer tracked.
  base::EraseIf(ar_plane_address_to_id_,
                [&plane_id_to_plane_object](const auto& plane_address_and_id) {
                  return !base::Contains(plane_id_to_plane_object,
                                         plane_address_and_id.second);
                });
  plane_id_to_plane_object_.swap(plane_id_to_plane_object);
  updated_plane_ids_.swap(updated_plane_ids);
}

mojom::XRPlaneDetectionDataPtr ArCorePlaneManager::GetDetectedPlanesData()
    const {
  TRACE_EVENT0("gpu", __FUNCTION__);

  std::vector<uint64_t> all_plane_ids;
  all_plane_ids.reserve(plane_id_to_plane_object_.size());
  for (const auto& plane_id_and_object : plane_id_to_plane_object_) {
    all_plane_ids.push_back(plane_id_and_object.first.GetUnsafeValue());
  }

  std::vector<mojom::XRPlaneDataPtr> updated_planes;
  updated_planes.reserve(updated_plane_ids_.size());
  for (const auto& plane_id : updated_plane_ids_) {
    const device::internal::ScopedArCoreObject<ArTrackable*>& trackable =
        plane_id_to_plane_object_.at(plane_id);

    const ArPlane* ar_plane = ArAsPlane(trackable.get());

    // orientation
    ArPlaneType plane_type;
    ArPlane_getType(arcore_session_, ar_plane, &plane_type);

    // pose
    internal::ScopedArCoreObject<ArPose*> plane_pose;
    ArPose_create(
        arcore_session_, nullptr,
        internal::ScopedArCoreObject<ArPose*>::Receiver(plane_pose).get());
    ArPlane_getCenterPose(arcore_session_, ar_plane, plane_pose.get());
    mojom::Pose pose =
        GetMojomPoseFromArPose(arcore_session_, plane_pose.get());

    // polygon
    int32_t polygon_size;
    ArPlane_getPolygonSize(arcore_session_, ar_plane, &polygon_size);
    // We are supposed to get 2*N floats describing (x, z) cooridinates of N
    // points.
    DCHECK(polygon_size % 2 == 0);

    std::unique_ptr<float[]> vertices_raw =
        std::make_unique<float[]>(polygon_size);
    ArPlane_getPolygon(arcore_session_, ar_plane, vertices_raw.get());

    std::vector<mojom::XRPlanePointDataPtr> vertices;
    for (int i = 0; i < polygon_size; i += 2) {
      vertices.push_back(
          mojom::XRPlanePointData::New(vertices_raw[i], vertices_raw[i + 1]));
    }

    // ID
    updated_planes.push_back(mojom::XRPlaneData::New(
        plane_id.GetUnsafeValue(),
        mojo::ConvertTo<device::mojom::XRPlaneOrientation>(plane_type),
        device::mojom::Pose::New(pose), std::move(vertices)));
  }

  return mojom::XRPlaneDetectionData::New(std::move(all_plane_ids),
                                          std::move(updated_planes));
}

std::pair<PlaneId, bool> ArCorePlaneManager::CreateOrGetPlaneId(
    void* plane_address) {
  auto it = ar_plane_address_to_id_.find(plane_address);
  if (it != ar_plane_address_to_id_.end()) {
    return std::make_pair(it->second, false);
  }

  CHECK(next_id_ != std::numeric_limits<uint64_t>::max())
      << "preventing ID overflow";

  uint64_t current_id = next_id_++;
  ar_plane_address_to_id_.emplace(plane_address, current_id);

  return std::make_pair(PlaneId(current_id), true);
}

base::Optional<PlaneId> ArCorePlaneManager::GetPlaneId(
    void* plane_address) const {
  auto it = ar_plane_address_to_id_.find(plane_address);
  if (it == ar_plane_address_to_id_.end()) {
    return base::nullopt;
  }

  return it->second;
}

bool ArCorePlaneManager::PlaneExists(PlaneId id) const {
  return base::Contains(plane_id_to_plane_object_, id);
}

base::Optional<gfx::Transform> ArCorePlaneManager::GetMojoFromPlane(
    PlaneId id) const {
  auto it = plane_id_to_plane_object_.find(id);
  if (it == plane_id_to_plane_object_.end()) {
    return base::nullopt;
  }

  const ArPlane* plane =
      ArAsPlane(it->second.get());  // Naked pointer is fine here, ArAsPlane
                                    // does not increase the internal refcount.

  ArPlane_getCenterPose(arcore_session_, plane, ar_pose_.get());
  mojom::Pose mojo_pose =
      GetMojomPoseFromArPose(arcore_session_, ar_pose_.get());

  return mojo::ConvertTo<gfx::Transform>(mojo_pose);
}

device::internal::ScopedArCoreObject<ArAnchor*>
ArCorePlaneManager::CreateAnchor(PlaneId id,
                                 const device::mojom::Pose& pose) const {
  auto it = plane_id_to_plane_object_.find(id);
  if (it == plane_id_to_plane_object_.end()) {
    return {};
  }

  auto ar_pose = GetArPoseFromMojomPose(arcore_session_, pose);

  device::internal::ScopedArCoreObject<ArAnchor*> ar_anchor;
  ArStatus status = ArTrackable_acquireNewAnchor(
      arcore_session_, it->second.get(), ar_pose.get(),
      device::internal::ScopedArCoreObject<ArAnchor*>::Receiver(ar_anchor)
          .get());

  if (status != AR_SUCCESS) {
    return {};
  }

  return ar_anchor;
}

}  // namespace device
