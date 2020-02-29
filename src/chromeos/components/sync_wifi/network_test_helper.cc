// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/components/sync_wifi/network_test_helper.h"

#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "chromeos/components/sync_wifi/network_type_conversions.h"
#include "chromeos/login/login_state/login_state.h"
#include "chromeos/network/network_handler.h"
#include "chromeos/services/network_config/in_process_instance.h"
#include "components/onc/onc_pref_names.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/user_manager/fake_user_manager.h"

namespace chromeos {

namespace sync_wifi {

NetworkTestHelper::NetworkTestHelper()
    : CrosNetworkConfigTestHelper(/*initialize= */ false) {
  LoginState::Initialize();
  PrefProxyConfigTrackerImpl::RegisterProfilePrefs(user_prefs_.registry());
  PrefProxyConfigTrackerImpl::RegisterPrefs(local_state_.registry());
  ::onc::RegisterProfilePrefs(user_prefs_.registry());
  ::onc::RegisterPrefs(local_state_.registry());

  network_profile_handler_ = NetworkProfileHandler::InitializeForTesting();
  network_configuration_handler_ =
      base::WrapUnique<NetworkConfigurationHandler>(
          NetworkConfigurationHandler::InitializeForTest(
              network_state_helper_->network_state_handler(),
              network_device_handler_.get()));
  ui_proxy_config_service_ = std::make_unique<chromeos::UIProxyConfigService>(
      &user_prefs_, &local_state_,
      network_state_helper_->network_state_handler(),
      network_profile_handler_.get());
  managed_network_configuration_handler_ =
      ManagedNetworkConfigurationHandler::InitializeForTesting(
          network_state_helper_->network_state_handler(),
          network_profile_handler_.get(), network_device_handler_.get(),
          network_configuration_handler_.get(), ui_proxy_config_service_.get());
  managed_network_configuration_handler_->SetPolicy(
      ::onc::ONC_SOURCE_DEVICE_POLICY,
      /*userhash=*/std::string(),
      /*network_configs_onc=*/base::ListValue(),
      /*global_network_config=*/base::DictionaryValue());

  auto fake_user_manager = std::make_unique<user_manager::FakeUserManager>();
  scoped_user_manager_ = std::make_unique<user_manager::ScopedUserManager>(
      std::move(fake_user_manager));

  LoginState::Get()->SetLoggedInState(LoginState::LOGGED_IN_ACTIVE,
                                      LoginState::LOGGED_IN_USER_REGULAR);

  Initialize(managed_network_configuration_handler_.get());
}

NetworkTestHelper::~NetworkTestHelper() {
  LoginState::Shutdown();
  ui_proxy_config_service_.reset();
}

void NetworkTestHelper::SetUp() {
  NetworkHandler::Initialize();
  network_state_helper_->ResetDevicesAndServices();

  base::RunLoop().RunUntilIdle();
}

void NetworkTestHelper::ConfigureWiFiNetwork(const std::string& ssid,
                                             bool is_secured,
                                             bool in_profile) {
  std::string security_entry =
      is_secured ? R"("SecurityClass": "psk", "Passphrase": "secretsauce", )"
                 : R"("SecurityClass": "none", )";
  std::string profile_entry =
      in_profile ? base::StringPrintf(R"("Profile": "%s", )",
                                      network_state_helper_->UserHash())
                 : std::string();
  network_state_helper_->ConfigureService(base::StringPrintf(
      R"({"GUID": "%s_guid", "Type": "wifi", "SSID": "%s",
            %s "State": "ready", "Strength": 100,
            %s "AutoConnect": true, "Connectable": true})",
      ssid.c_str(), ssid.c_str(), security_entry.c_str(),
      profile_entry.c_str()));
  base::RunLoop().RunUntilIdle();
}

}  // namespace sync_wifi

}  // namespace chromeos