// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/crostini/crostini_disk.h"

#include <memory>
#include <utility>

#include "base/test/bind_test_util.h"
#include "chrome/browser/chromeos/crostini/crostini_types.mojom.h"
#include "chromeos/dbus/concierge/concierge_service.pb.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace crostini {
namespace disk {

class CrostiniDiskTest : public testing::Test {
 public:
  CrostiniDiskTest() = default;
  ~CrostiniDiskTest() override = default;

 protected:
  // A wrapper for OnListVmDisks which returns the result.
  std::unique_ptr<CrostiniDiskInfo> OnListVmDisksWithResult(
      const char* vm_name,
      int64_t free_space,
      base::Optional<vm_tools::concierge::ListVmDisksResponse>
          list_disks_response) {
    std::unique_ptr<CrostiniDiskInfo> result;
    auto store =
        base::BindLambdaForTesting([&](std::unique_ptr<CrostiniDiskInfo> info) {
          result = std::move(info);
        });

    OnListVmDisks(store, vm_name, free_space, list_disks_response);
    // OnListVmDisks is synchronous and runs the callback upon finishing, so by
    // the time it returns we know that result has been stored.
    return result;
  }
};

TEST_F(CrostiniDiskTest, NonResizeableDiskReturnsEarly) {
  vm_tools::concierge::ListVmDisksResponse response;
  response.set_success(true);
  auto* image = response.add_images();
  image->set_image_type(vm_tools::concierge::DiskImageType::DISK_IMAGE_QCOW2);
  image->set_name("vm_name");

  auto disk_info = OnListVmDisksWithResult("vm_name", 0, response);
  ASSERT_TRUE(disk_info);
  EXPECT_FALSE(disk_info->can_resize);
}

TEST_F(CrostiniDiskTest, CallbackGetsEmptyInfoOnError) {
  auto disk_info_nullopt = OnListVmDisksWithResult("vm_name", 0, base::nullopt);
  EXPECT_FALSE(disk_info_nullopt);

  vm_tools::concierge::ListVmDisksResponse failure_response;
  failure_response.set_success(false);
  auto disk_info_failure =
      OnListVmDisksWithResult("vm_name", 0, failure_response);
  EXPECT_FALSE(disk_info_failure);

  vm_tools::concierge::ListVmDisksResponse no_matching_disks_response;
  auto* image = no_matching_disks_response.add_images();
  no_matching_disks_response.set_success(true);
  image->set_image_type(vm_tools::concierge::DiskImageType::DISK_IMAGE_QCOW2);
  image->set_name("wrong_name");

  auto disk_info_no_disks =
      OnListVmDisksWithResult("vm_name", 0, no_matching_disks_response);
  EXPECT_FALSE(disk_info_no_disks);
}

TEST_F(CrostiniDiskTest, IsUserChosenSizeIsReportedCorrectly) {
  vm_tools::concierge::ListVmDisksResponse response;
  auto* image = response.add_images();
  response.set_success(true);
  image->set_name("vm_name");
  image->set_image_type(vm_tools::concierge::DiskImageType::DISK_IMAGE_RAW);
  image->set_user_chosen_size(true);

  auto disk_info_user_size = OnListVmDisksWithResult("vm_name", 0, response);

  ASSERT_TRUE(disk_info_user_size);
  EXPECT_TRUE(disk_info_user_size->can_resize);
  EXPECT_TRUE(disk_info_user_size->is_user_chosen_size);

  image->set_user_chosen_size(false);

  auto disk_info_not_user_size =
      OnListVmDisksWithResult("vm_name", 0, response);

  ASSERT_TRUE(disk_info_not_user_size);
  EXPECT_TRUE(disk_info_not_user_size->can_resize);
  EXPECT_FALSE(disk_info_not_user_size->is_user_chosen_size);
}

TEST_F(CrostiniDiskTest, AreTicksCalculated) {
  // The actual tick calculation has its own unit tests, we just check that we
  // get something that looks sane for given values.
  vm_tools::concierge::ListVmDisksResponse response;
  auto* image = response.add_images();
  response.set_success(true);
  image->set_name("vm_name");
  image->set_image_type(vm_tools::concierge::DiskImageType::DISK_IMAGE_RAW);
  image->set_min_size(1000);
  image->set_size(1000);

  auto disk_info = OnListVmDisksWithResult("vm_name", 100, response);

  ASSERT_TRUE(disk_info);
  EXPECT_EQ(disk_info->ticks.front()->value, 1000);

  // Available space is current + free.
  EXPECT_EQ(disk_info->ticks.back()->value, 1100);
}

TEST_F(CrostiniDiskTest, DefaultIsCurrentValue) {
  vm_tools::concierge::ListVmDisksResponse response;
  auto* image = response.add_images();
  response.set_success(true);
  image->set_name("vm_name");
  image->set_image_type(vm_tools::concierge::DiskImageType::DISK_IMAGE_RAW);
  image->set_min_size(1000);
  image->set_size(9033);
  auto disk_info = OnListVmDisksWithResult("vm_name", 11100, response);
  ASSERT_TRUE(disk_info);

  ASSERT_TRUE(disk_info->ticks.size() > 3);
  EXPECT_EQ(disk_info->ticks.at(disk_info->default_index)->value, 9033);
  EXPECT_LT(disk_info->ticks.at(disk_info->default_index - 1)->value, 9033);
  EXPECT_GT(disk_info->ticks.at(disk_info->default_index + 1)->value, 9033);
}

TEST_F(CrostiniDiskTest, AmountOfFreeDiskSpaceFailureIsHandled) {
  std::unique_ptr<CrostiniDiskInfo> disk_info;
  auto store_info =
      base::BindLambdaForTesting([&](std::unique_ptr<CrostiniDiskInfo> info) {
        disk_info = std::move(info);
      });

  OnAmountOfFreeDiskSpace(store_info, nullptr, "vm_name", 0);
  EXPECT_FALSE(disk_info);
}

TEST_F(CrostiniDiskTest, VMRunningFailureIsHandled) {
  std::unique_ptr<CrostiniDiskInfo> disk_info;
  auto store_info =
      base::BindLambdaForTesting([&](std::unique_ptr<CrostiniDiskInfo> info) {
        disk_info = std::move(info);
      });

  OnVMRunning(store_info, nullptr, "vm_name", 0,
              CrostiniResult::VM_START_FAILED);
  EXPECT_FALSE(disk_info);
}
}  // namespace disk
}  // namespace crostini
