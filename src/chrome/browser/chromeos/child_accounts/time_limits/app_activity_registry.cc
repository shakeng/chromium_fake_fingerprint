// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/child_accounts/time_limits/app_activity_registry.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "base/time/default_tick_clock.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/chromeos/child_accounts/time_limits/app_time_limit_utils.h"
#include "chrome/browser/chromeos/child_accounts/time_limits/app_time_limits_whitelist_policy_wrapper.h"
#include "chrome/browser/chromeos/child_accounts/time_limits/app_time_notification_delegate.h"
#include "chrome/browser/chromeos/child_accounts/time_limits/app_time_policy_helpers.h"
#include "chrome/browser/chromeos/child_accounts/time_limits/persisted_app_info.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/common/pref_names.h"
#include "chrome/services/app_service/public/mojom/types.mojom.h"
#include "components/policy/proto/device_management_backend.pb.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"

namespace chromeos {
namespace app_time {

namespace {

constexpr base::TimeDelta kFiveMinutes = base::TimeDelta::FromMinutes(5);
constexpr base::TimeDelta kOneMinute = base::TimeDelta::FromMinutes(1);
constexpr base::TimeDelta kZeroMinutes = base::TimeDelta::FromMinutes(0);

enterprise_management::App::AppType AppTypeForReporting(
    apps::mojom::AppType type) {
  switch (type) {
    case apps::mojom::AppType::kArc:
      return enterprise_management::App::ARC;
    case apps::mojom::AppType::kBuiltIn:
      return enterprise_management::App::BUILT_IN;
    case apps::mojom::AppType::kCrostini:
      return enterprise_management::App::CROSTINI;
    case apps::mojom::AppType::kExtension:
      return enterprise_management::App::EXTENSION;
    case apps::mojom::AppType::kWeb:
      return enterprise_management::App::WEB;
    default:
      return enterprise_management::App::UNKNOWN;
  }
}

enterprise_management::AppActivity::AppState AppStateForReporting(
    AppState state) {
  switch (state) {
    case AppState::kAvailable:
      return enterprise_management::AppActivity::DEFAULT;
    case AppState::kAlwaysAvailable:
      return enterprise_management::AppActivity::ALWAYS_AVAILABLE;
    case AppState::kBlocked:
      return enterprise_management::AppActivity::BLOCKED;
    case AppState::kLimitReached:
      return enterprise_management::AppActivity::LIMIT_REACHED;
    case AppState::kUninstalled:
      return enterprise_management::AppActivity::UNINSTALLED;
    default:
      return enterprise_management::AppActivity::UNKNOWN;
  }
}

chromeos::app_time::AppId GetAndroidChromeAppId() {
  return chromeos::app_time::AppId(apps::mojom::AppType::kArc,
                                   "com.android.chrome");
}

}  // namespace

AppActivityRegistry::TestApi::TestApi(AppActivityRegistry* registry)
    : registry_(registry) {}

AppActivityRegistry::TestApi::~TestApi() = default;

const base::Optional<AppLimit>& AppActivityRegistry::TestApi::GetAppLimit(
    const AppId& app_id) const {
  DCHECK(base::Contains(registry_->activity_registry_, app_id));
  return registry_->activity_registry_.at(app_id).limit;
}

base::Optional<base::TimeDelta> AppActivityRegistry::TestApi::GetTimeLeft(
    const AppId& app_id) const {
  return registry_->GetTimeLeftForApp(app_id);
}

void AppActivityRegistry::TestApi::SaveAppActivity() {
  registry_->SaveAppActivity();
}

AppActivityRegistry::AppDetails::AppDetails() = default;

AppActivityRegistry::AppDetails::AppDetails(const AppActivity& activity)
    : activity(activity) {}

AppActivityRegistry::AppDetails::~AppDetails() = default;

void AppActivityRegistry::AppDetails::ResetTimeCheck() {
  activity.set_last_notification(AppNotification::kUnknown);
  if (app_limit_timer)
    app_limit_timer->AbandonAndStop();
}

bool AppActivityRegistry::AppDetails::IsLimitReached() const {
  if (!limit.has_value())
    return false;

  if (limit->restriction() != AppRestriction::kTimeLimit)
    return false;

  DCHECK(limit->daily_limit());
  if (limit->daily_limit() > activity.RunningActiveTime())
    return false;

  return true;
}

bool AppActivityRegistry::AppDetails::IsLimitEqual(
    const base::Optional<AppLimit>& another_limit) const {
  if (limit.has_value() != another_limit.has_value())
    return false;

  if (!limit.has_value())
    return true;

  if (limit->restriction() == another_limit->restriction() &&
      limit->daily_limit() == another_limit->daily_limit()) {
    return true;
  }

  return false;
}

// static
void AppActivityRegistry::RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(prefs::kPerAppTimeLimitsAppActivities);
}

AppActivityRegistry::AppActivityRegistry(
    AppServiceWrapper* app_service_wrapper,
    AppTimeNotificationDelegate* notification_delegate,
    Profile* profile)
    : profile_(profile),
      app_service_wrapper_(app_service_wrapper),
      notification_delegate_(notification_delegate) {
  DCHECK(app_service_wrapper_);
  DCHECK(notification_delegate_);
  DCHECK(profile_);

  InitializeRegistryFromPref();

  app_service_wrapper_->AddObserver(this);
}

AppActivityRegistry::~AppActivityRegistry() {
  app_service_wrapper_->RemoveObserver(this);
}

void AppActivityRegistry::OnAppInstalled(const AppId& app_id) {
  // App might be already present in registry, because we preserve info between
  // sessions and app service does not. Make sure not to override cached state.
  if (!base::Contains(activity_registry_, app_id))
    Add(app_id);
}

void AppActivityRegistry::OnAppUninstalled(const AppId& app_id) {
  // TODO(agawronska): Consider DCHECK instead of it. Not sure if there are
  // legit cases when we might go out of sync with AppService.
  if (base::Contains(activity_registry_, app_id))
    SetAppState(app_id, AppState::kUninstalled);
}

void AppActivityRegistry::OnAppAvailable(const AppId& app_id) {
  if (base::Contains(activity_registry_, app_id))
    SetAppState(app_id, AppState::kAvailable);
}

void AppActivityRegistry::OnAppBlocked(const AppId& app_id) {
  if (base::Contains(activity_registry_, app_id))
    SetAppState(app_id, AppState::kBlocked);
}

void AppActivityRegistry::OnAppActive(const AppId& app_id,
                                      aura::Window* window,
                                      base::Time timestamp) {
  if (!base::Contains(activity_registry_, app_id))
    return;

  if (app_id == GetChromeAppId())
    return;

  AppDetails& app_details = activity_registry_[app_id];

  DCHECK(IsAppAvailable(app_id));

  std::set<aura::Window*>& active_windows = app_details.active_windows;

  if (base::Contains(active_windows, window))
    return;

  active_windows.insert(window);

  // No need to set app as active if there were already active windows for the
  // app
  if (active_windows.size() > 1)
    return;

  SetAppActive(app_id, timestamp);
}

void AppActivityRegistry::OnAppInactive(const AppId& app_id,
                                        aura::Window* window,
                                        base::Time timestamp) {
  if (!base::Contains(activity_registry_, app_id))
    return;

  if (app_id == GetChromeAppId())
    return;

  std::set<aura::Window*>& active_windows =
      activity_registry_[app_id].active_windows;

  if (!base::Contains(active_windows, window))
    return;

  active_windows.erase(window);
  if (active_windows.size() > 0)
    return;

  SetAppInactive(app_id, timestamp);
}

bool AppActivityRegistry::IsAppInstalled(const AppId& app_id) const {
  if (base::Contains(activity_registry_, app_id))
    return GetAppState(app_id) != AppState::kUninstalled;
  return false;
}

bool AppActivityRegistry::IsAppAvailable(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  auto state = GetAppState(app_id);
  return state == AppState::kAvailable || state == AppState::kAlwaysAvailable;
}

bool AppActivityRegistry::IsAppBlocked(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  return GetAppState(app_id) == AppState::kBlocked;
}

bool AppActivityRegistry::IsAppTimeLimitReached(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  return GetAppState(app_id) == AppState::kLimitReached;
}

bool AppActivityRegistry::IsAppActive(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  return activity_registry_.at(app_id).activity.is_active();
}

bool AppActivityRegistry::IsWhitelistedApp(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  return GetAppState(app_id) == AppState::kAlwaysAvailable;
}

void AppActivityRegistry::AddAppStateObserver(
    AppActivityRegistry::AppStateObserver* observer) {
  app_state_observers_.AddObserver(observer);
}

void AppActivityRegistry::RemoveAppStateObserver(
    AppActivityRegistry::AppStateObserver* observer) {
  app_state_observers_.RemoveObserver(observer);
}

base::TimeDelta AppActivityRegistry::GetActiveTime(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  return activity_registry_.at(app_id).activity.RunningActiveTime();
}

AppState AppActivityRegistry::GetAppState(const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  return activity_registry_.at(app_id).activity.app_state();
}

std::vector<PauseAppInfo> AppActivityRegistry::GetPausedApps(
    bool show_pause_dialog) const {
  std::vector<PauseAppInfo> paused_apps;
  for (const auto& info : activity_registry_) {
    const AppId& app_id = info.first;
    const AppDetails& details = info.second;
    if (GetAppState(app_id) == AppState::kLimitReached) {
      DCHECK(details.limit.has_value());
      DCHECK(details.limit->daily_limit().has_value());
      paused_apps.push_back(PauseAppInfo(
          app_id, details.limit->daily_limit().value(), show_pause_dialog));
    }
  }

  return paused_apps;
}

AppActivityReportInterface::ReportParams
AppActivityRegistry::GenerateAppActivityReport(
    enterprise_management::ChildStatusReportRequest* report) const {
  // TODO(agawronska): We should also report the ongoing activity if it started
  // before the reporting, because it could have been going for a long time.
  const base::Time timestamp = base::Time::Now();
  bool anything_reported = false;

  for (const auto& entry : activity_registry_) {
    const AppId& app_id = entry.first;
    const AppActivity& registered_activity = entry.second.activity;

    // Do not report if there is no activity.
    if (registered_activity.active_times().empty())
      continue;

    enterprise_management::AppActivity* app_activity =
        report->add_app_activity();
    enterprise_management::App* app_info = app_activity->mutable_app_info();
    app_info->set_app_id(app_id.app_id());
    app_info->set_app_type(AppTypeForReporting(app_id.app_type()));
    // AppService is is only different for ARC++ apps.
    if (app_id.app_type() == apps::mojom::AppType::kArc) {
      app_info->add_additional_app_id(
          app_service_wrapper_->GetAppServiceId(app_id));
    }
    app_activity->set_app_state(
        AppStateForReporting(registered_activity.app_state()));
    app_activity->set_populated_at(timestamp.ToJavaTime());

    for (const auto& active_time : registered_activity.active_times()) {
      enterprise_management::TimePeriod* time_period =
          app_activity->add_active_time_periods();
      time_period->set_start_timestamp(active_time.active_from().ToJavaTime());
      time_period->set_end_timestamp(active_time.active_to().ToJavaTime());
    }
    anything_reported = true;
  }

  return AppActivityReportInterface::ReportParams{timestamp, anything_reported};
}

void AppActivityRegistry::CleanRegistry(base::Time timestamp) {
  for (auto it = activity_registry_.begin(); it != activity_registry_.end();) {
    const AppId& app_id = it->first;
    AppActivity& registered_activity = it->second.activity;
    // TODO(agawronska): Update data stored in user pref.
    registered_activity.RemoveActiveTimeEarlierThan(timestamp);
    // Remove app that was uninstalled and does not have any past activity
    // stored.
    if (GetAppState(app_id) == AppState::kUninstalled &&
        registered_activity.active_times().empty()) {
      it = activity_registry_.erase(it);
    } else {
      ++it;
    }
  }
}

void AppActivityRegistry::UpdateAppLimits(
    const std::map<AppId, AppLimit>& app_limits) {
  for (auto& entry : activity_registry_) {
    const AppId& app_id = entry.first;
    base::Optional<AppLimit> new_limit;
    if (base::Contains(app_limits, app_id))
      new_limit = app_limits.at(app_id);

    bool is_web = ContributesToWebTimeLimit(app_id, GetAppState(app_id));
    if (is_web && base::Contains(app_limits, GetChromeAppId()))
      new_limit = app_limits.at(GetChromeAppId());

    // In Family Link app Chrome on Chrome OS is combined together with Android
    // Chrome. Therefore, use Android Chrome's time limit.
    if (is_web && !base::Contains(app_limits, GetChromeAppId()) &&
        base::Contains(app_limits, GetAndroidChromeAppId())) {
      new_limit = app_limits.at(GetAndroidChromeAppId());
    }

    SetAppLimit(app_id, new_limit);
  }
}

void AppActivityRegistry::SetAppLimit(
    const AppId& app_id,
    const base::Optional<AppLimit>& app_limit) {
  DCHECK(base::Contains(activity_registry_, app_id));
  AppDetails& details = activity_registry_.at(app_id);

  // Limit 'data' are considered equal if only the |last_updated_| is different.
  // Update the limit to store new |last_updated_| value.
  bool did_change = !details.IsLimitEqual(app_limit);
  ShowLimitUpdatedNotificationIfNeeded(app_id, details.limit, app_limit);
  details.limit = app_limit;

  // Limit 'data' is the same - no action needed.
  if (!did_change)
    return;

  if (IsWhitelistedApp(app_id)) {
    if (app_limit.has_value()) {
      VLOG(1) << "Tried to set time limit for " << app_id
              << " which is whitelisted.";
    }

    details.limit = base::nullopt;
    return;
  }

  // TODO(agawronska): Handle web limit changes here.

  // Limit for the active app changed - adjust the timers.
  // Handling of active app is different, because ongoing activity needs to be
  // taken into account.
  if (IsAppActive(app_id)) {
    details.ResetTimeCheck();
    ScheduleTimeLimitCheckForApp(app_id);
    return;
  }

  // Inactive available app reached the limit - update the state.
  // If app is in any other state than |kAvailable| the limit does not take
  // effect.
  if (IsAppAvailable(app_id) && details.IsLimitReached()) {
    SetAppInactive(app_id, base::Time::Now());
    SetAppState(app_id, AppState::kLimitReached);
    return;
  }

  // Paused inactive app is below the limit again - update the state.
  // This can happen if the limit was removed or new limit is greater the the
  // previous one. We know that the state should be available, because app can
  // only reach the limit if it is available.
  if (IsAppTimeLimitReached(app_id) && !details.IsLimitReached()) {
    SetAppState(app_id, AppState::kAvailable);
    return;
  }
}

void AppActivityRegistry::OnChromeAppActivityChanged(
    ChromeAppActivityState state,
    base::Time timestamp) {
  AppId chrome_app_id = GetChromeAppId();
  if (!base::Contains(activity_registry_, chrome_app_id))
    return;

  AppDetails& details = activity_registry_[chrome_app_id];
  bool was_active = details.activity.is_active();

  bool is_active = (state == ChromeAppActivityState::kActive);

  // No change in state.
  if (was_active == is_active)
    return;

  if (is_active) {
    SetAppActive(chrome_app_id, timestamp);
    return;
  }

  SetAppInactive(chrome_app_id, timestamp);
}

void AppActivityRegistry::OnTimeLimitWhitelistChanged(
    const AppTimeLimitsWhitelistPolicyWrapper& wrapper) {
  std::vector<AppId> whitelisted_apps = wrapper.GetWhitelistAppList();
  for (const AppId& app : whitelisted_apps) {
    if (!base::Contains(activity_registry_, app))
      continue;

    if (GetAppState(app) == AppState::kAlwaysAvailable)
      continue;

    base::Optional<AppLimit>& limit = activity_registry_.at(app).limit;
    if (limit.has_value())
      limit = base::nullopt;

    SetAppState(app, AppState::kAlwaysAvailable);
  }
}

void AppActivityRegistry::OnResetTimeReached(base::Time timestamp) {
  for (std::pair<const AppId, AppDetails>& info : activity_registry_) {
    const AppId& app = info.first;
    AppDetails& details = info.second;

    // Reset running active time.
    details.activity.ResetRunningActiveTime(timestamp);

    // If timer is running, stop timer. Abandon all tasks set.
    details.ResetTimeCheck();

    // If the time limit has been reached, mark the app as available.
    if (details.activity.app_state() == AppState::kLimitReached)
      SetAppState(app, AppState::kAvailable);

    // If the application is currently active, schedule a time limit
    // check.
    if (details.activity.is_active())
      ScheduleTimeLimitCheckForApp(app);
  }
}

void AppActivityRegistry::Add(const AppId& app_id) {
  activity_registry_[app_id].activity = AppActivity(AppState::kAvailable);
  newly_installed_apps_.push_back(app_id);
  for (auto& observer : app_state_observers_)
    observer.OnAppInstalled(app_id);
}

void AppActivityRegistry::SetAppState(const AppId& app_id, AppState app_state) {
  DCHECK(base::Contains(activity_registry_, app_id));
  AppDetails& app_details = activity_registry_.at(app_id);
  AppActivity& app_activity = app_details.activity;
  AppState previous_state = app_activity.app_state();
  app_activity.SetAppState(app_state);

  if (app_activity.app_state() == AppState::kLimitReached) {
    bool was_active = false;
    if (app_activity.is_active()) {
      was_active = true;
      app_details.active_windows.clear();
      app_activity.SetAppInactive(base::Time::Now());
    }

    for (auto& observer : app_state_observers_) {
      const base::Optional<AppLimit>& limit =
          activity_registry_.at(app_id).limit;
      DCHECK(limit->daily_limit());
      observer.OnAppLimitReached(app_id, limit->daily_limit().value(),
                                 was_active);
    }
    return;
  }

  if (previous_state == AppState::kLimitReached &&
      app_activity.app_state() != AppState::kLimitReached) {
    for (auto& observer : app_state_observers_)
      observer.OnAppLimitRemoved(app_id);
    return;
  }
}

void AppActivityRegistry::SetAppActive(const AppId& app_id,
                                       base::Time timestamp) {
  DCHECK(base::Contains(activity_registry_, app_id));
  AppDetails& app_details = activity_registry_[app_id];
  DCHECK(!app_details.activity.is_active());
  if (ContributesToWebTimeLimit(app_id, GetAppState(app_id)))
    app_details.activity.set_running_active_time(GetWebActiveRunningTime());

  app_details.activity.SetAppActive(timestamp);

  ScheduleTimeLimitCheckForApp(app_id);
}

void AppActivityRegistry::SetAppInactive(const AppId& app_id,
                                         base::Time timestamp) {
  DCHECK(base::Contains(activity_registry_, app_id));
  auto& details = activity_registry_.at(app_id);

  details.activity.SetAppInactive(timestamp);
  details.ResetTimeCheck();

  // If the application is a web app, synchronize its running active time with
  // those of other inactive web apps.
  if (ContributesToWebTimeLimit(app_id, GetAppState(app_id))) {
    base::TimeDelta active_time = details.activity.RunningActiveTime();
    for (auto& app_info : activity_registry_) {
      const AppId& app_id = app_info.first;
      if (!ContributesToWebTimeLimit(app_id, GetAppState(app_id))) {
        continue;
      }

      AppDetails& details = app_info.second;
      if (!details.activity.is_active())
        details.activity.set_running_active_time(active_time);
    }
  }
}

void AppActivityRegistry::ScheduleTimeLimitCheckForApp(const AppId& app_id) {
  DCHECK(base::Contains(activity_registry_, app_id));
  AppDetails& app_details = activity_registry_[app_id];

  // If there is no time limit information, don't set the timer.
  if (!app_details.limit.has_value())
    return;

  const AppLimit& limit = app_details.limit.value();
  if (limit.restriction() != AppRestriction::kTimeLimit)
    return;

  if (!app_details.app_limit_timer) {
    app_details.app_limit_timer = std::make_unique<base::OneShotTimer>(
        base::DefaultTickClock::GetInstance());
  }

  DCHECK(!app_details.app_limit_timer->IsRunning());

  // Check that the timer instance has been created.
  base::Optional<base::TimeDelta> time_limit = GetTimeLeftForApp(app_id);
  DCHECK(time_limit.has_value());

  if (time_limit > kFiveMinutes) {
    time_limit = time_limit.value() - kFiveMinutes;
  } else if (time_limit > kOneMinute) {
    time_limit = time_limit.value() - kOneMinute;
  }

  VLOG(1) << "Schedule app time limit check for " << app_id << " for "
          << time_limit.value();

  app_details.app_limit_timer->Start(
      FROM_HERE, time_limit.value(),
      base::BindRepeating(&AppActivityRegistry::CheckTimeLimitForApp,
                          base::Unretained(this), app_id));
}

base::Optional<base::TimeDelta> AppActivityRegistry::GetTimeLeftForApp(
    const AppId& app_id) const {
  DCHECK(base::Contains(activity_registry_, app_id));
  const AppDetails& app_details = activity_registry_.at(app_id);

  // If |app_details.limit| doesn't have value, the app has no restriction.
  if (!app_details.limit.has_value())
    return base::nullopt;

  const AppLimit& limit = app_details.limit.value();

  if (limit.restriction() != AppRestriction::kTimeLimit)
    return base::nullopt;

  // If the app has kTimeLimit restriction, DCHECK that daily limit has value.
  DCHECK(limit.daily_limit().has_value());

  AppState state = app_details.activity.app_state();
  if (state == AppState::kAlwaysAvailable || state == AppState::kBlocked)
    return base::nullopt;

  if (state == AppState::kLimitReached)
    return kZeroMinutes;

  DCHECK(state == AppState::kAvailable);

  base::TimeDelta time_limit = limit.daily_limit().value();

  base::TimeDelta active_time;
  if (ContributesToWebTimeLimit(app_id, GetAppState(app_id))) {
    active_time = GetWebActiveRunningTime();
  } else {
    active_time = app_details.activity.RunningActiveTime();
  }

  if (active_time >= time_limit)
    return kZeroMinutes;

  return time_limit - active_time;
}

void AppActivityRegistry::CheckTimeLimitForApp(const AppId& app_id) {
  AppDetails& details = activity_registry_[app_id];

  base::Optional<base::TimeDelta> time_left = GetTimeLeftForApp(app_id);
  AppNotification last_notification = details.activity.last_notification();

  if (!time_left.has_value())
    return;

  DCHECK(details.limit.has_value());
  DCHECK(details.limit->daily_limit().has_value());
  const base::TimeDelta time_limit = details.limit->daily_limit().value();

  if (time_left <= kFiveMinutes && time_left > kOneMinute &&
      last_notification != AppNotification::kFiveMinutes) {
    notification_delegate_->ShowAppTimeLimitNotification(
        app_id, time_limit, AppNotification::kFiveMinutes);
    details.activity.set_last_notification(AppNotification::kFiveMinutes);
    ScheduleTimeLimitCheckForApp(app_id);
    return;
  }

  if (time_left <= kOneMinute && time_left > kZeroMinutes &&
      last_notification != AppNotification::kOneMinute) {
    notification_delegate_->ShowAppTimeLimitNotification(
        app_id, time_limit, AppNotification::kOneMinute);
    details.activity.set_last_notification(AppNotification::kOneMinute);
    ScheduleTimeLimitCheckForApp(app_id);
    return;
  }

  if (time_left == kZeroMinutes &&
      last_notification != AppNotification::kTimeLimitReached) {
    details.activity.set_last_notification(AppNotification::kTimeLimitReached);

    if (ContributesToWebTimeLimit(app_id, GetAppState(app_id))) {
      WebTimeLimitReached(base::Time::Now());
    } else {
      SetAppState(app_id, AppState::kLimitReached);
    }

    notification_delegate_->ShowAppTimeLimitNotification(
        app_id, time_limit, AppNotification::kTimeLimitReached);
  }
}

void AppActivityRegistry::ShowLimitUpdatedNotificationIfNeeded(
    const AppId& app_id,
    const base::Optional<AppLimit>& old_limit,
    const base::Optional<AppLimit>& new_limit) {
  // Web app limit changes are covered by Chrome notification.
  if (app_id.app_type() == apps::mojom::AppType::kWeb)
    return;

  const bool had_time_limit =
      old_limit && old_limit->restriction() == AppRestriction::kTimeLimit;
  const bool has_time_limit =
      new_limit && new_limit->restriction() == AppRestriction::kTimeLimit;

  // Time limit was removed.
  if (!has_time_limit && had_time_limit) {
    notification_delegate_->ShowAppTimeLimitNotification(
        app_id, base::nullopt, AppNotification::kTimeLimitChanged);
    return;
  }

  // Time limit was set or value changed.
  if (has_time_limit && (!had_time_limit || old_limit->daily_limit() !=
                                                new_limit->daily_limit())) {
    notification_delegate_->ShowAppTimeLimitNotification(
        app_id, new_limit->daily_limit(), AppNotification::kTimeLimitChanged);
    return;
  }
}

base::TimeDelta AppActivityRegistry::GetWebActiveRunningTime() const {
  base::TimeDelta active_running_time = base::TimeDelta::FromSeconds(0);
  for (const auto& app_info : activity_registry_) {
    const AppId& app_id = app_info.first;
    const AppDetails& details = app_info.second;
    if (!ContributesToWebTimeLimit(app_id, GetAppState(app_id))) {
      continue;
    }

    active_running_time = details.activity.RunningActiveTime();

    // If the app is active, then it has the most up to date active running
    // time.
    if (details.activity.is_active())
      return active_running_time;
  }

  return active_running_time;
}

void AppActivityRegistry::WebTimeLimitReached(base::Time timestamp) {
  for (auto& app_info : activity_registry_) {
    const AppId& app_id = app_info.first;
    AppDetails& details = app_info.second;
    if (!ContributesToWebTimeLimit(app_id, GetAppState(app_id))) {
      continue;
    }

    if (details.activity.app_state() == AppState::kLimitReached)
      return;

    SetAppState(app_id, AppState::kLimitReached);
  }
}

void AppActivityRegistry::InitializeRegistryFromPref() {
  PrefService* pref_service = profile_->GetPrefs();
  const base::Value* value =
      pref_service->GetList(prefs::kPerAppTimeLimitsAppActivities);
  DCHECK(value);

  const std::vector<PersistedAppInfo> applications_info =
      PersistedAppInfo::PersistedAppInfosFromList(
          value,
          /* include_app_activity_array */ false);

  for (const auto& app_info : applications_info) {
    DCHECK(!base::Contains(activity_registry_, app_info.app_id()));

    // Don't restore uninstalled application's data.
    if (app_info.app_state() == AppState::kUninstalled)
      continue;

    activity_registry_[app_info.app_id()].activity =
        AppActivity(app_info.app_state(), app_info.active_running_time());
  }
}

PersistedAppInfo AppActivityRegistry::GetPersistedAppInfoForApp(
    const AppId& app_id,
    base::Time timestamp) {
  DCHECK(base::Contains(activity_registry_, app_id));

  AppDetails& details = activity_registry_.at(app_id);

  // Updates |AppActivity::active_times_| to include the current activity up to
  // |timestamp|.
  details.activity.CaptureOngoingActivity(timestamp);

  return PersistedAppInfo(app_id, details.activity.app_state(),
                          details.activity.RunningActiveTime(),
                          details.activity.TakeActiveTimes());
}

void AppActivityRegistry::SaveAppActivity() {
  {
    ListPrefUpdate update(profile_->GetPrefs(),
                          prefs::kPerAppTimeLimitsAppActivities);
    base::ListValue* list_value = update.Get();

    const base::Time now = base::Time::Now();

    base::Value::ListView list_view = list_value->GetList();
    for (base::Value& entry : list_view) {
      base::Optional<AppId> app_id = policy::AppIdFromAppInfoDict(entry);
      DCHECK(app_id.has_value());

      if (!base::Contains(activity_registry_, app_id.value())) {
        base::Optional<AppState> state =
            PersistedAppInfo::GetAppStateFromDict(&entry);
        DCHECK(state.has_value() && state.value() == AppState::kUninstalled);
        continue;
      }

      const PersistedAppInfo info =
          GetPersistedAppInfoForApp(app_id.value(), now);
      info.UpdateAppActivityPreference(&entry);
    }

    for (const AppId& app_id : newly_installed_apps_) {
      const PersistedAppInfo info = GetPersistedAppInfoForApp(app_id, now);
      base::Value value(base::Value::Type::DICTIONARY);
      info.UpdateAppActivityPreference(&value);
      list_value->Append(std::move(value));
    }
    newly_installed_apps_.clear();
  }

  // Ensure that the app activity is persisted.
  profile_->GetPrefs()->CommitPendingWrite();
}

}  // namespace app_time
}  // namespace chromeos
