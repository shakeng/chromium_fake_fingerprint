// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/assistant/service.h"

#include <memory>
#include <utility>
#include <vector>

#include "ash/public/mojom/assistant_state_controller.mojom.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chromeos/audio/cras_audio_handler.h"
#include "chromeos/constants/chromeos_features.h"
#include "chromeos/dbus/power/fake_power_manager_client.h"
#include "chromeos/services/assistant/assistant_state_proxy.h"
#include "chromeos/services/assistant/fake_assistant_manager_service_impl.h"
#include "chromeos/services/assistant/public/cpp/assistant_prefs.h"
#include "chromeos/services/assistant/test_support/fake_client.h"
#include "chromeos/services/assistant/test_support/fully_initialized_assistant_state.h"
#include "components/prefs/testing_pref_service.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/signin/public/identity_manager/identity_test_environment.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {
namespace assistant {

namespace {
constexpr base::TimeDelta kDefaultTokenExpirationDelay =
    base::TimeDelta::FromMilliseconds(60000);

#define EXPECT_STATE(_state) EXPECT_EQ(_state, assistant_manager()->GetState())

const char* kAccessToken = "fake access token";
const char* kGaiaId = "gaia_id_for_user_gmail.com";
const char* kEmailAddress = "user@gmail.com";
}  // namespace

class FakeAssistantClient : public FakeClient {
 public:
  explicit FakeAssistantClient(ash::AssistantState* assistant_state)
      : assistant_state_(assistant_state),
        status_(ash::mojom::AssistantState::NOT_READY) {}

  ash::mojom::AssistantState status() { return status_; }

 private:
  // FakeClient:

  void OnAssistantStatusChanged(ash::mojom::AssistantState new_state) override {
    status_ = new_state;
  }

  void RequestAssistantStateController(
      mojo::PendingReceiver<ash::mojom::AssistantStateController> receiver)
      override {
    assistant_state_->BindReceiver(std::move(receiver));
  }

  ash::AssistantState* const assistant_state_;
  ash::mojom::AssistantState status_;

  DISALLOW_COPY_AND_ASSIGN(FakeAssistantClient);
};

class FakeDeviceActions : mojom::DeviceActions {
 public:
  FakeDeviceActions() {}

  mojo::PendingRemote<mojom::DeviceActions> CreatePendingRemoteAndBind() {
    return receiver_.BindNewPipeAndPassRemote();
  }

 private:
  // mojom::DeviceActions:
  void SetWifiEnabled(bool enabled) override {}
  void SetBluetoothEnabled(bool enabled) override {}
  void GetScreenBrightnessLevel(
      GetScreenBrightnessLevelCallback callback) override {
    std::move(callback).Run(true, 1.0);
  }
  void SetScreenBrightnessLevel(double level, bool gradual) override {}
  void SetNightLightEnabled(bool enabled) override {}
  void SetSwitchAccessEnabled(bool enabled) override {}
  void OpenAndroidApp(chromeos::assistant::mojom::AndroidAppInfoPtr app_info,
                      OpenAndroidAppCallback callback) override {}
  void VerifyAndroidApp(
      std::vector<chromeos::assistant::mojom::AndroidAppInfoPtr> apps_info,
      VerifyAndroidAppCallback callback) override {}
  void LaunchAndroidIntent(const std::string& intent) override {}
  void AddAppListEventSubscriber(
      mojo::PendingRemote<chromeos::assistant::mojom::AppListEventSubscriber>
          subscriber) override {}

  mojo::Receiver<mojom::DeviceActions> receiver_{this};

  DISALLOW_COPY_AND_ASSIGN(FakeDeviceActions);
};

class AssistantServiceTest : public testing::Test {
 public:
  AssistantServiceTest() = default;

  ~AssistantServiceTest() override = default;

  void SetUp() override {
    scoped_feature_list_.InitAndEnableFeature(
        chromeos::features::kAmbientModeFeature);

    chromeos::CrasAudioHandler::InitializeForTesting();

    PowerManagerClient::InitializeFake();
    FakePowerManagerClient::Get()->SetTabletMode(
        PowerManagerClient::TabletMode::OFF, base::TimeTicks());

    shared_url_loader_factory_ =
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_);

    prefs::RegisterProfilePrefs(pref_service_.registry());
    pref_service_.SetBoolean(prefs::kAssistantEnabled, true);
    pref_service_.SetBoolean(prefs::kAssistantHotwordEnabled, true);

    // In production the primary account is set before the service is created.
    identity_test_env_.MakeUnconsentedPrimaryAccountAvailable(kEmailAddress);

    service_ = std::make_unique<Service>(
        remote_service_.BindNewPipeAndPassReceiver(),
        shared_url_loader_factory_->Clone(),
        identity_test_env_.identity_manager(), &pref_service_);
    service_->SetAssistantManagerServiceForTesting(
        std::make_unique<FakeAssistantManagerServiceImpl>());

    remote_service_->Init(fake_assistant_client_.MakeRemote(),
                          fake_device_actions_.CreatePendingRemoteAndBind());
    // Wait for AssistantManagerService to be set.
    base::RunLoop().RunUntilIdle();

    IssueAccessToken(kAccessToken);
  }

  void TearDown() override {
    service_.reset();
    PowerManagerClient::Shutdown();
    chromeos::CrasAudioHandler::Shutdown();
  }

  void StartAssistantAndWait() {
    pref_service()->SetBoolean(prefs::kAssistantEnabled, true);
    base::RunLoop().RunUntilIdle();
  }

  void StopAssistantAndWait() {
    pref_service()->SetBoolean(prefs::kAssistantEnabled, false);
    base::RunLoop().RunUntilIdle();
  }

  void IssueAccessToken(const std::string& access_token) {
    identity_test_env_.WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
        access_token, base::Time::Now() + kDefaultTokenExpirationDelay);
  }

  Service* service() { return service_.get(); }

  FakeAssistantManagerServiceImpl* assistant_manager() {
    auto* result = static_cast<FakeAssistantManagerServiceImpl*>(
        service_->assistant_manager_service_.get());
    DCHECK(result);
    return result;
  }

  void ResetFakeAssistantManager() {
    assistant_manager()->SetUser(base::nullopt);
  }

  signin::IdentityTestEnvironment* identity_test_env() {
    return &identity_test_env_;
  }

  ash::AssistantState* assistant_state() { return &assistant_state_; }

  PrefService* pref_service() { return &pref_service_; }

  FakeAssistantClient* client() { return &fake_assistant_client_; }

  base::test::TaskEnvironment* task_environment() { return &task_environment_; }

  ash::AmbientModeState* ambient_mode_state() { return &ambient_mode_state_; }

 private:
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::test::ScopedFeatureList scoped_feature_list_;

  std::unique_ptr<Service> service_;
  mojo::Remote<mojom::AssistantService> remote_service_;

  FullyInitializedAssistantState assistant_state_;
  signin::IdentityTestEnvironment identity_test_env_;
  FakeAssistantClient fake_assistant_client_{&assistant_state_};
  FakeDeviceActions fake_device_actions_;

  TestingPrefServiceSimple pref_service_;

  network::TestURLLoaderFactory url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_url_loader_factory_;

  ash::AmbientModeState ambient_mode_state_;

  DISALLOW_COPY_AND_ASSIGN(AssistantServiceTest);
};

TEST_F(AssistantServiceTest, RefreshTokenAfterExpire) {
  ASSERT_FALSE(identity_test_env()->IsAccessTokenRequestPending());
  task_environment()->FastForwardBy(kDefaultTokenExpirationDelay / 2);

  // Before token expire, should not request new token.
  EXPECT_FALSE(identity_test_env()->IsAccessTokenRequestPending());

  task_environment()->FastForwardBy(kDefaultTokenExpirationDelay);

  // After token expire, should request once.
  EXPECT_TRUE(identity_test_env()->IsAccessTokenRequestPending());
}

TEST_F(AssistantServiceTest, RetryRefreshTokenAfterFailure) {
  ASSERT_FALSE(identity_test_env()->IsAccessTokenRequestPending());

  // Let the first token expire. Another will be requested.
  task_environment()->FastForwardBy(kDefaultTokenExpirationDelay);
  EXPECT_TRUE(identity_test_env()->IsAccessTokenRequestPending());

  // Reply with an error.
  identity_test_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithError(
      GoogleServiceAuthError(GoogleServiceAuthError::State::CONNECTION_FAILED));
  EXPECT_FALSE(identity_test_env()->IsAccessTokenRequestPending());

  // Token request automatically retry.
  // The failure delay has jitter so fast forward a bit more, but before
  // the returned token would expire again.
  task_environment()->FastForwardBy(kDefaultTokenExpirationDelay / 2);

  EXPECT_TRUE(identity_test_env()->IsAccessTokenRequestPending());
}

TEST_F(AssistantServiceTest, RetryRefreshTokenAfterDeviceWakeup) {
  ASSERT_FALSE(identity_test_env()->IsAccessTokenRequestPending());

  FakePowerManagerClient::Get()->SendSuspendDone();
  // Token requested immediately after suspend done.
  EXPECT_TRUE(identity_test_env()->IsAccessTokenRequestPending());
}

TEST_F(AssistantServiceTest, StopImmediatelyIfAssistantIsRunning) {
  // Test is set up as |State::STARTED|.
  assistant_manager()->FinishStart();
  EXPECT_STATE(AssistantManagerService::State::RUNNING);

  StopAssistantAndWait();

  EXPECT_STATE(AssistantManagerService::State::STOPPED);
}

TEST_F(AssistantServiceTest, StopDelayedIfAssistantNotFinishedStarting) {
  EXPECT_STATE(AssistantManagerService::State::STARTING);

  // Turning settings off will trigger logic to try to stop it.
  StopAssistantAndWait();

  EXPECT_STATE(AssistantManagerService::State::STARTING);

  task_environment()->FastForwardBy(kUpdateAssistantManagerDelay);

  // No change of state because it is still starting.
  EXPECT_STATE(AssistantManagerService::State::STARTING);

  assistant_manager()->FinishStart();

  task_environment()->FastForwardBy(kUpdateAssistantManagerDelay);

  EXPECT_STATE(AssistantManagerService::State::STOPPED);
}

TEST_F(AssistantServiceTest, ShouldSendUserInfoWhenStarting) {
  // First stop the service and reset the AssistantManagerService
  assistant_manager()->FinishStart();
  StopAssistantAndWait();
  ResetFakeAssistantManager();

  // Now start the service
  StartAssistantAndWait();

  EXPECT_EQ(kAccessToken, assistant_manager()->access_token());
  EXPECT_EQ(kGaiaId, assistant_manager()->gaia_id());
}

TEST_F(AssistantServiceTest, ShouldSendUserInfoWhenAccessTokenIsRefreshed) {
  assistant_manager()->FinishStart();

  // Reset the AssistantManagerService so it forgets the user info sent when
  // starting the service.
  ResetFakeAssistantManager();

  // Now force an access token refresh
  task_environment()->FastForwardBy(kDefaultTokenExpirationDelay);
  IssueAccessToken("new token");

  EXPECT_EQ("new token", assistant_manager()->access_token());
  EXPECT_EQ(kGaiaId, assistant_manager()->gaia_id());
}

TEST_F(AssistantServiceTest, ShouldSetClientStatusToNotReadyWhenStarting) {
  assistant_manager()->SetStateAndInformObservers(
      AssistantManagerService::State::STARTING);
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(client()->status(), ash::mojom::AssistantState::NOT_READY);
}

TEST_F(AssistantServiceTest, ShouldSetClientStatusToReadyWhenStarted) {
  assistant_manager()->SetStateAndInformObservers(
      AssistantManagerService::State::STARTED);
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(client()->status(), ash::mojom::AssistantState::READY);
}

TEST_F(AssistantServiceTest, ShouldSetClientStatusToNewReadyWhenRunning) {
  assistant_manager()->SetStateAndInformObservers(
      AssistantManagerService::State::RUNNING);
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(client()->status(), ash::mojom::AssistantState::NEW_READY);
}

TEST_F(AssistantServiceTest, ShouldSetClientStatusToNotReadyWhenStopped) {
  assistant_manager()->SetStateAndInformObservers(
      AssistantManagerService::State::RUNNING);
  base::RunLoop().RunUntilIdle();

  StopAssistantAndWait();

  EXPECT_EQ(client()->status(), ash::mojom::AssistantState::NOT_READY);
}

TEST_F(AssistantServiceTest,
       ShouldResetAccessTokenWhenAmbientModeStateChanged) {
  assistant_manager()->FinishStart();
  EXPECT_STATE(AssistantManagerService::State::RUNNING);
  EXPECT_FALSE(identity_test_env()->IsAccessTokenRequestPending());
  ASSERT_TRUE(assistant_manager()->access_token().has_value());
  ASSERT_EQ(assistant_manager()->access_token().value(), kAccessToken);

  ambient_mode_state()->SetAmbientModeEnabled(true);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(identity_test_env()->IsAccessTokenRequestPending());
  ASSERT_FALSE(assistant_manager()->access_token().has_value());

  // Disabling ambient mode requests a new token.
  ambient_mode_state()->SetAmbientModeEnabled(false);
  EXPECT_TRUE(identity_test_env()->IsAccessTokenRequestPending());

  // Assistant manager receives the new token.
  IssueAccessToken("new token");
  EXPECT_FALSE(identity_test_env()->IsAccessTokenRequestPending());
  ASSERT_TRUE(assistant_manager()->access_token().has_value());
  ASSERT_EQ(assistant_manager()->access_token().value(), "new token");
}

}  // namespace assistant
}  // namespace chromeos
