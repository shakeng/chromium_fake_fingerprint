// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_DISK_H_
#define CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_DISK_H_

#include <memory>
#include <string>
#include <vector>

#include "base/optional.h"
#include "chrome/browser/chromeos/crostini/crostini_simple_types.h"
#include "chrome/browser/chromeos/crostini/crostini_types.mojom-forward.h"
#include "chrome/browser/profiles/profile.h"
#include "chromeos/dbus/concierge/concierge_service.pb.h"

namespace crostini {

struct CrostiniDiskInfo {
  CrostiniDiskInfo();
  CrostiniDiskInfo(CrostiniDiskInfo&&);
  CrostiniDiskInfo(const CrostiniDiskInfo&) = delete;
  CrostiniDiskInfo& operator=(CrostiniDiskInfo&&);
  CrostiniDiskInfo& operator=(const CrostiniDiskInfo&) = delete;
  ~CrostiniDiskInfo();
  bool can_resize{};
  bool is_user_chosen_size{};
  int default_index{};
  std::vector<crostini::mojom::DiskSliderTickPtr> ticks;
};

namespace disk {
using OnceDiskInfoCallback =
    base::OnceCallback<void(std::unique_ptr<CrostiniDiskInfo> info)>;

// Constructs a CrostiniDiskInfo for the requested vm under the given profile
// then calls callback with it once done.
void GetDiskInfo(OnceDiskInfoCallback callback,
                 Profile* profile,
                 std::string vm_name);

// Callback for OnAmountOfFreeDiskSpace which passes off to the next step in the
// chain. Not intended to be called directly unless you're crostini_disk or
// tests.
void OnAmountOfFreeDiskSpace(OnceDiskInfoCallback callback,
                             Profile* profile,
                             std::string vm_name,
                             int64_t free_space);

// Callback for EnsureVmRunning which passes off to the next step in the chain.
// Not intended to be called directly unless you're crostini_disk or tests.
void OnVMRunning(OnceDiskInfoCallback callback,
                 Profile* profile,
                 std::string vm_name,
                 int64_t free_space,
                 CrostiniResult result);

// Callback for OnListVmDisks which passes off to the next step in the chain.
// Not intended to be called directly unless you're crostini_disk or tests.
void OnListVmDisks(
    OnceDiskInfoCallback callback,
    std::string vm_name,
    int64_t free_space,
    base::Optional<vm_tools::concierge::ListVmDisksResponse> response);

// Given a minimum, currently selected and maximum value, constructs a range of
// DiskSliderTicks spanning from min to max. Ensures that one of the ticks
// matches the current value and will write the index of that value to
// out_default_index.
std::vector<crostini::mojom::DiskSliderTickPtr> GetTicks(
    const vm_tools::concierge::VmDiskInfo& info,
    int64_t min,
    int64_t current,
    int64_t max,
    int* out_default_index);

}  // namespace disk
}  // namespace crostini
#endif  // CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_DISK_H_
