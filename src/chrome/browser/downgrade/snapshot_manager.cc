// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/downgrade/snapshot_manager.h"

#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/metrics/histogram_functions.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/downgrade/downgrade_utils.h"
#include "chrome/browser/downgrade/snapshot_file_collector.h"
#include "chrome/browser/downgrade/user_data_downgrade.h"
#include "chrome/common/chrome_constants.h"

namespace downgrade {

namespace {

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class SnapshotOperationResult {
  kSuccess = 0,
  kPartialSuccess = 1,
  kFailure = 2,
  kFailedToCreateSnapshotDirectory = 3,
  kMaxValue = kFailedToCreateSnapshotDirectory
};

// Copies the item at |user_data_dir|/|relative_path| to
// |snapshot_dir|/|relative_path| if the item exists.
// Returns |true| if the item was found at the source and successfully copied.
// Returns |false| if the item was found at the source but not successfully
// copied. Returns no value if the file was not at the source.
base::Optional<bool> CopyItemToSnapshotDirectory(
    const base::FilePath& relative_path,
    const base::FilePath& user_data_dir,
    const base::FilePath& snapshot_dir,
    bool is_directory) {
  auto source = user_data_dir.Append(relative_path);
  auto destination = snapshot_dir.Append(relative_path);

  // If nothing exists to be moved, do not consider it a success or a failure.
  if (!base::PathExists(source))
    return base::nullopt;

  return is_directory
             ? base::CopyDirectory(source, destination, /*recursive=*/true)
             : base::CopyFile(source, destination);
}

// Returns true if |base_name| matches a user profile directory's format. This
// function will ignore the "System Profile" directory.
bool IsProfileDir(const base::FilePath& base_name) {
  // The initial profile ("Default") is an easy one.
  if (base_name == base::FilePath().AppendASCII(chrome::kInitialProfile))
    return true;
  // Other profile dirs begin with "Profile " and end with a number.
  const base::FilePath prefix(
      base::FilePath().AppendASCII(chrome::kMultiProfileDirPrefix));
  int number;
  return base::StartsWith(base_name.value(), prefix.value(),
                          base::CompareCase::SENSITIVE) &&
         base::StringToInt(base::FilePath::StringPieceType(base_name.value())
                               .substr(prefix.value().length()),
                           &number);
}

// Returns a list of profile directory base names under |user_data_dir|.
std::vector<base::FilePath> GetUserProfileDirectories(
    const base::FilePath& user_data_dir) {
  std::vector<base::FilePath> profile_dirs;
  base::FileEnumerator enumerator(user_data_dir, /*recursive=*/false,
                                  base::FileEnumerator::DIRECTORIES);

  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    const auto base_name = path.BaseName();
    if (IsProfileDir(base_name))
      profile_dirs.push_back(std::move(base_name));
  }
  return profile_dirs;
}

// Moves the |source| directory to |target| to be deleted later. If the move
// initially fails, move the contents of the directory.
void MoveFolderForLaterDeletion(const base::FilePath& source,
                                const base::FilePath& target,
                                const char* move_result_histogram,
                                const char* failure_count_histogram) {
  const bool move_result = MoveWithoutFallback(source, target);
  base::UmaHistogramBoolean(move_result_histogram, move_result);
  if (move_result)
    return;
  auto failure_count =
      MoveContents(source, base::GetUniquePath(target), ExclusionPredicate());
  if (failure_count.has_value() &&
      !base::DeleteFile(source, /*recursive=*/false)) {
    failure_count = failure_count.value() + 1;
    // Report precise values rather than an exponentially bucketed
    // histogram. Bucket 0 means that the target directory could not be
    // created. All other buckets are a count of files/directories left
    // behind.
    base::UmaHistogramExactLinear(failure_count_histogram,
                                  failure_count.value_or(0), 50);
  }
}

}  // namespace

SnapshotManager::SnapshotManager(const base::FilePath& user_data_dir)
    : user_data_dir_(user_data_dir) {}

SnapshotManager::~SnapshotManager() = default;

void SnapshotManager::TakeSnapshot(const base::Version& version) {
  DCHECK(version.IsValid());
  base::FilePath snapshot_dir =
      user_data_dir_.Append(kSnapshotsDir).AppendASCII(version.GetString());

  // If the target snapshot directory already exists, try marking it for
  // deletion. In case of failure, try moving the contents then keep going.
  if (base::PathExists(snapshot_dir)) {
    auto move_target_dir = user_data_dir_.Append(kSnapshotsDir)
                               .AddExtension(kDowngradeDeleteSuffix);
    base::CreateDirectory(move_target_dir);
    MoveFolderForLaterDeletion(
        snapshot_dir, move_target_dir.AppendASCII(version.GetString()),
        "Downgrade.TakeSnapshot.MoveExistingSnapshot.Result",
        "Downgrade.TakeSnapshot.MoveExistingSnapshot.FailureCount");
  }

  size_t success_count = 0;
  size_t error_count = 0;
  auto record_success_error = [&success_count,
                               &error_count](base::Optional<bool> success) {
    if (!success.has_value())
      return;
    if (success.value())
      ++success_count;
    else
      ++error_count;
  };

  // Abort the snapshot if the snapshot directory could not be created.
  if (!base::CreateDirectory(snapshot_dir)) {
    base::UmaHistogramEnumeration(
        "Downgrade.TakeSnapshot.Result",
        SnapshotOperationResult::kFailedToCreateSnapshotDirectory);
    return;
  }
  const uint16_t milestone = version.components()[0];

  // Copy items to be preserved at the top-level of User Data.
  for (const auto& file : GetUserSnapshotItemDetails(milestone)) {
    record_success_error(
        CopyItemToSnapshotDirectory(base::FilePath(file.path), user_data_dir_,
                                    snapshot_dir, file.is_directory));
  }

  auto profile_snapshot_item_details = GetProfileSnapshotItemDetails(milestone);

  // Copy items to be preserved in each Profile directory.
  for (const auto& profile_dir : GetUserProfileDirectories(user_data_dir_)) {
    bool profile_dir_created =
        base::CreateDirectory(snapshot_dir.Append(profile_dir));
    base::UmaHistogramBoolean(
        "Downgrade.TakeSnapshot.ProfileDirectoryCreation.Result",
        profile_dir_created);
    // Abort the current profile snapshot if the profile directory could not be
    // created.
    if (!profile_dir_created) {
      ++error_count;
      continue;
    }
    for (const auto& file : profile_snapshot_item_details) {
      record_success_error(CopyItemToSnapshotDirectory(
          profile_dir.Append(file.path), user_data_dir_, snapshot_dir,
          file.is_directory));
    }
  }

  // Copy the "Last Version" file to the snapshot directory last since it is the
  // file that determines, by its presence in the snapshot directory, if the
  // snapshot is complete.
  record_success_error(CopyItemToSnapshotDirectory(
      base::FilePath(kDowngradeLastVersionFile), user_data_dir_, snapshot_dir,
      /*is_directory=*/false));

  auto snapshot_result = SnapshotOperationResult::kFailure;
  if (error_count == 0)
    snapshot_result = SnapshotOperationResult::kSuccess;
  else if (success_count > 0)
    snapshot_result = SnapshotOperationResult::kPartialSuccess;

  if (error_count > 0) {
    base::UmaHistogramExactLinear("Downgrade.TakeSnapshot.FailureCount",
                                  error_count, 100);
  }

  base::UmaHistogramEnumeration("Downgrade.TakeSnapshot.Result",
                                snapshot_result);
}

void SnapshotManager::RestoreSnapshot(const base::Version& version) {
  DCHECK(version.IsValid());
  auto snapshot_version = GetSnapshotToRestore(version, user_data_dir_);
  if (!snapshot_version)
    return;

  // The snapshot folder needs to be moved if it matches the current chrome
  // milestone. However, if it comes from an earlier version, it should be
  // copied so that a future downgrade to that version stays possible.
  const bool move_snapshot = snapshot_version == version;

  auto snapshot_dir = user_data_dir_.Append(kSnapshotsDir)
                          .AppendASCII(snapshot_version->GetString());

  size_t success_count = 0;
  size_t error_count = 0;
  auto record_success_error = [&success_count, &error_count](bool success) {
    if (success)
      ++success_count;
    else
      ++error_count;
  };
  base::FileEnumerator enumerator(
      snapshot_dir, /*recursive=*/false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);

  // Move or copy the contents of the selected snapshot directory into User
  // Data.
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    const auto item_info = enumerator.GetInfo();
    const auto target_path = user_data_dir_.Append(path.BaseName());
    if (move_snapshot)
      record_success_error(base::Move(path, target_path));
    else if (enumerator.GetInfo().IsDirectory())
      record_success_error(
          base::CopyDirectory(path, target_path, /*recursive=*/true));
    else
      record_success_error(base::CopyFile(path, target_path));
  }
  auto snapshot_result = SnapshotOperationResult::kFailure;
  if (error_count == 0)
    snapshot_result = SnapshotOperationResult::kSuccess;
  else if (success_count > 0)
    snapshot_result = SnapshotOperationResult::kPartialSuccess;

  if (error_count > 0) {
    base::UmaHistogramExactLinear("Downgrade.RestoreSnapshot.FailureCount",
                                  error_count, 100);
  }
  base::UmaHistogramEnumeration("Downgrade.RestoreSnapshot.Result",
                                snapshot_result);

  // Mark the snapshot directory for later deletion if its contents were moved
  // into User Data. If the snapshot directory cannot be renamed, fallback to
  // moving its contents.
  if (move_snapshot) {
    auto move_target =
        GetTempDirNameForDelete(user_data_dir_, base::FilePath(kSnapshotsDir))
            .Append(snapshot_dir.BaseName());

    // Cleans up the remnants of the moved snapshot directory. If moving the
    // folder fails, delete the "Last Version File" so that this snapshot is
    // considered incomplete and deleted later.
    MoveFolderForLaterDeletion(snapshot_dir, move_target,
                               "Downgrade.InvalidSnapshotMove.Result",
                               "Downgrade.InvalidSnapshotMove.FailureCount");

    auto last_version_file_path =
        snapshot_dir.Append(kDowngradeLastVersionFile);
    base::DeleteFile(last_version_file_path, /*recursive=*/false);

    base::UmaHistogramBoolean(
        "Downgrade.RestoreSnapshot.CleanupAfterFailure.Result",
        !base::PathExists(snapshot_dir));
  }
}

void SnapshotManager::PurgeInvalidAndOldSnapshots(
    int max_number_of_snapshots) const {
  const auto snapshot_dir = user_data_dir_.Append(kSnapshotsDir);

  // Move the invalid snapshots within from Snapshots/NN to Snapshots.DELETE/NN.
  const base::FilePath target =
      snapshot_dir.AddExtension(kDowngradeDeleteSuffix);
  base::CreateDirectory(target);

  // Moves all the invalid snapshots for later deletion.
  auto invalid_snapshots = GetInvalidSnapshots(snapshot_dir);
  for (const auto& path : invalid_snapshots) {
    MoveFolderForLaterDeletion(path, target.Append(path.BaseName()),
                               "Downgrade.InvalidSnapshotMove.Result",
                               "Downgrade.InvalidSnapshotMove.FailureCount");
  }

  auto available_snapshots = GetAvailableSnapshots(snapshot_dir);

  if (available_snapshots.size() <=
      base::checked_cast<size_t>(max_number_of_snapshots)) {
    return;
  }

  size_t number_of_snapshots_to_delete =
      available_snapshots.size() - max_number_of_snapshots;

  // Moves all the older snapshots for later deletion.
  for (const auto& snapshot : available_snapshots) {
    auto snapshot_path = snapshot_dir.AppendASCII(snapshot.GetString());
    MoveFolderForLaterDeletion(snapshot_path,
                               target.Append(snapshot_path.BaseName()),
                               "Downgrade.InvalidSnapshotMove.Result",
                               "Downgrade.InvalidSnapshotMove.FailureCount");
    if (--number_of_snapshots_to_delete == 0)
      break;
  }
}

std::vector<SnapshotItemDetails> SnapshotManager::GetProfileSnapshotItemDetails(
    uint16_t milestone) const {
  return CollectProfileItems(milestone);
}

std::vector<SnapshotItemDetails> SnapshotManager::GetUserSnapshotItemDetails(
    uint16_t milestone) const {
  return CollectUserDataItems(milestone);
}

}  // namespace downgrade
