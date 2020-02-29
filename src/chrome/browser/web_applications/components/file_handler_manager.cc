// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/components/file_handler_manager.h"

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/task/post_task.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/web_applications/components/web_app_file_handler_registration.h"
#include "chrome/browser/web_applications/components/web_app_prefs_utils.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/web_launch/file_handling_expiry.mojom.h"

namespace web_app {

bool FileHandlerManager::disable_automatic_file_handler_cleanup_for_testing_ =
    false;

FileHandlerManager::FileHandlerManager(Profile* profile)
    : profile_(profile), registrar_observer_(this) {}

FileHandlerManager::~FileHandlerManager() = default;

void FileHandlerManager::SetSubsystems(AppRegistrar* registrar) {
  registrar_ = registrar;
}

void FileHandlerManager::Start() {
  DCHECK(registrar_);

  registrar_observer_.Add(registrar_);

  if (!FileHandlerManager::
          disable_automatic_file_handler_cleanup_for_testing_) {
    base::PostTask(
        FROM_HERE,
        {content::BrowserThread::UI, base::TaskPriority::BEST_EFFORT},
        base::BindOnce(
            base::IgnoreResult(&FileHandlerManager::CleanupAfterOriginTrials),
            weak_ptr_factory_.GetWeakPtr()));
  }
}

void FileHandlerManager::DisableOsIntegrationForTesting() {
  disable_os_integration_for_testing_ = true;
}

int FileHandlerManager::TriggerFileHandlerCleanupForTesting() {
  return CleanupAfterOriginTrials();
}

void FileHandlerManager::SetOnFileHandlingExpiryUpdatedForTesting(
    base::RepeatingCallback<void()> on_file_handling_expiry_updated) {
  on_file_handling_expiry_updated_for_testing_ =
      on_file_handling_expiry_updated;
}

void FileHandlerManager::EnableAndRegisterOsFileHandlers(const AppId& app_id) {
  if (!IsFileHandlingAPIAvailable(app_id))
    return;

  UpdateBoolWebAppPref(profile()->GetPrefs(), app_id, kFileHandlersEnabled,
                       /*value=*/true);

  if (!ShouldRegisterFileHandlersWithOs() ||
      disable_os_integration_for_testing_) {
    return;
  }

  std::string app_name = registrar_->GetAppShortName(app_id);
  const std::vector<apps::FileHandlerInfo>* file_handlers =
      GetAllFileHandlers(app_id);
  if (!file_handlers)
    return;
  std::set<std::string> file_extensions =
      GetFileExtensionsFromFileHandlers(*file_handlers);
  std::set<std::string> mime_types =
      GetMimeTypesFromFileHandlers(*file_handlers);
  RegisterFileHandlersWithOs(app_id, app_name, profile(), file_extensions,
                             mime_types);
}

void FileHandlerManager::DisableAndUnregisterOsFileHandlers(
    const AppId& app_id) {
  UpdateBoolWebAppPref(profile()->GetPrefs(), app_id, kFileHandlersEnabled,
                       /*value=*/false);

  if (!ShouldRegisterFileHandlersWithOs() ||
      disable_os_integration_for_testing_) {
    return;
  }

  UnregisterFileHandlersWithOs(app_id, profile());
}

void FileHandlerManager::UpdateFileHandlingOriginTrialExpiry(
    content::WebContents* web_contents,
    const AppId& app_id) {
  mojo::AssociatedRemote<blink::mojom::FileHandlingExpiry> expiry_service;
  web_contents->GetMainFrame()->GetRemoteAssociatedInterfaces()->GetInterface(
      &expiry_service);
  DCHECK(expiry_service);

  auto* raw = expiry_service.get();
  raw->RequestOriginTrialExpiryTime(base::BindOnce(
      &FileHandlerManager::OnOriginTrialExpiryTimeReceived,
      weak_ptr_factory_.GetWeakPtr(), std::move(expiry_service), app_id));
}

const std::vector<apps::FileHandlerInfo>*
FileHandlerManager::GetEnabledFileHandlers(const AppId& app_id) {
  if (AreFileHandlersEnabled(app_id) && IsFileHandlingAPIAvailable(app_id))
    return GetAllFileHandlers(app_id);

  return nullptr;
}

bool FileHandlerManager::IsFileHandlingAPIAvailable(const AppId& app_id) {
  return base::FeatureList::IsEnabled(blink::features::kFileHandlingAPI) ||
         base::Time::FromDoubleT(GetDoubleWebAppPref(
             profile()->GetPrefs(), app_id,
             kFileHandlingOriginTrialExpiryTime)) >= base::Time::Now();
}

bool FileHandlerManager::AreFileHandlersEnabled(const AppId& app_id) const {
  return GetBoolWebAppPref(profile()->GetPrefs(), app_id, kFileHandlersEnabled);
}

void FileHandlerManager::OnOriginTrialExpiryTimeReceived(
    mojo::AssociatedRemote<blink::mojom::FileHandlingExpiry> /*interface*/,
    const AppId& app_id,
    base::Time expiry_time) {
  web_app::UpdateDoubleWebAppPref(profile_->GetPrefs(), app_id,
                                  kFileHandlingOriginTrialExpiryTime,
                                  expiry_time.ToDoubleT());
  // Only enable/disable file handlers if the state is changing, as
  // enabling/disabling is a potentially expensive operation (it may involve
  // creating an app shim, and will almost certainly involve IO).
  const bool file_handlers_enabled = AreFileHandlersEnabled(app_id);

  // If the trial is valid, ensure the file handlers are enabled.
  // Otherwise disable them.
  if (IsFileHandlingAPIAvailable(app_id)) {
    if (!file_handlers_enabled)
      EnableAndRegisterOsFileHandlers(app_id);
  } else if (file_handlers_enabled) {
    DisableAndUnregisterOsFileHandlers(app_id);
  }

  if (on_file_handling_expiry_updated_for_testing_)
    on_file_handling_expiry_updated_for_testing_.Run();
}

void FileHandlerManager::DisableAutomaticFileHandlerCleanupForTesting() {
  disable_automatic_file_handler_cleanup_for_testing_ = true;
}

int FileHandlerManager::CleanupAfterOriginTrials() {
  int cleaned_up_count = 0;
  for (const AppId& app_id : registrar_->GetAppIds()) {
    if (!AreFileHandlersEnabled(app_id))
      continue;

    if (IsFileHandlingAPIAvailable(app_id))
      continue;

    // If the trial has expired, unregister handlers.
    DisableAndUnregisterOsFileHandlers(app_id);
    cleaned_up_count++;
  }

  return cleaned_up_count;
}

void FileHandlerManager::OnWebAppUninstalled(const AppId& app_id) {
  DisableAndUnregisterOsFileHandlers(app_id);
}

void FileHandlerManager::OnWebAppProfileWillBeDeleted(const AppId& app_id) {
  DisableAndUnregisterOsFileHandlers(app_id);
}

void FileHandlerManager::OnAppRegistrarDestroyed() {
  registrar_observer_.RemoveAll();
}

const base::Optional<GURL> FileHandlerManager::GetMatchingFileHandlerURL(
    const AppId& app_id,
    const std::vector<base::FilePath>& launch_files) {
  if (!IsFileHandlingAPIAvailable(app_id))
    return base::nullopt;

  const std::vector<apps::FileHandlerInfo>* file_handlers =
      GetAllFileHandlers(app_id);
  if (!file_handlers || launch_files.empty())
    return base::nullopt;

  // Leading `.` for each file extension must be removed to match those given by
  // FileHandlerInfo.extensions below.
  std::set<std::string> file_extensions;
  for (const auto& file_path : launch_files) {
    std::string extension =
        base::FilePath(file_path.Extension()).AsUTF8Unsafe();
    if (extension.length() <= 1)
      return base::nullopt;
    file_extensions.insert(extension.substr(1));
  }

  for (const auto& file_handler : *file_handlers) {
    bool all_extensions_supported = true;
    for (const auto& extension : file_extensions) {
      if (!file_handler.extensions.count(extension)) {
        all_extensions_supported = false;
        break;
      }
    }
    if (all_extensions_supported)
      return GURL(file_handler.id);
  }

  return base::nullopt;
}

std::set<std::string> GetFileExtensionsFromFileHandlers(
    const std::vector<apps::FileHandlerInfo>& file_handlers) {
  std::set<std::string> file_extensions;
  for (const auto& file_handler : file_handlers) {
    for (const auto& file_ext : file_handler.extensions)
      file_extensions.insert(file_ext);
  }
  return file_extensions;
}

std::set<std::string> GetMimeTypesFromFileHandlers(
    const std::vector<apps::FileHandlerInfo>& file_handlers) {
  std::set<std::string> mime_types;
  for (const auto& file_handler : file_handlers) {
    for (const auto& mime_type : file_handler.types)
      mime_types.insert(mime_type);
  }
  return mime_types;
}

}  // namespace web_app
