// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/policy/remote_commands/device_command_run_routine_job.h"

#include <limits>
#include <memory>

#include "base/json/json_writer.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/test/bind_test_util.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chromeos/dbus/cros_healthd/cros_healthd_client.h"
#include "chromeos/dbus/cros_healthd/fake_cros_healthd_client.h"
#include "chromeos/services/cros_healthd/public/mojom/cros_healthd_diagnostics.mojom.h"
#include "components/policy/proto/device_management_backend.pb.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace policy {

namespace em = enterprise_management;

namespace {

// String constant identifying the id field in the result payload.
constexpr char kIdFieldName[] = "id";
// String constant identifying the status field in the result payload.
constexpr char kStatusFieldName[] = "status";

// String constant identifying the routine enum field in the command payload.
constexpr char kRoutineEnumFieldName[] = "routineEnum";
// String constant identifying the parameter dictionary field in the command
// payload.
constexpr char kParamsFieldName[] = "params";

// String constants identifying the parameter fields for the battery capacity
// routine.
constexpr char kLowMahFieldName[] = "lowMah";
constexpr char kHighMahFieldName[] = "highMah";

// String constants identifying the parameter fields for the battery health
// routine.
constexpr char kMaximumCycleCountFieldName[] = "maximumCycleCount";
constexpr char kPercentBatteryWearAllowedFieldName[] =
    "percentBatteryWearAllowed";

// String constants identifying the parameter fields for the urandom routine.
constexpr char kLengthSecondsFieldName[] = "lengthSeconds";

// String constants identifying the parameter fields for the AC power routine.
constexpr char kExpectedStatusFieldName[] = "expectedStatus";
constexpr char kExpectedPowerTypeFieldName[] = "expectedPowerType";

// Dummy values to populate cros_healthd's RunRoutineResponse.
constexpr uint32_t kId = 11;
constexpr chromeos::cros_healthd::mojom::DiagnosticRoutineStatusEnum kStatus =
    chromeos::cros_healthd::mojom::DiagnosticRoutineStatusEnum::kRunning;

constexpr RemoteCommandJob::UniqueIDType kUniqueID = 987123;

em::RemoteCommand GenerateCommandProto(
    RemoteCommandJob::UniqueIDType unique_id,
    base::TimeDelta age_of_command,
    base::TimeDelta idleness_cutoff,
    bool terminate_upon_input,
    base::Optional<chromeos::cros_healthd::mojom::DiagnosticRoutineEnum>
        routine,
    base::Optional<base::Value> params) {
  em::RemoteCommand command_proto;
  command_proto.set_type(em::RemoteCommand_Type_DEVICE_RUN_DIAGNOSTIC_ROUTINE);
  command_proto.set_command_id(unique_id);
  command_proto.set_age_of_command(age_of_command.InMilliseconds());
  base::Value root_dict(base::Value::Type::DICTIONARY);
  if (routine.has_value()) {
    root_dict.SetIntKey(kRoutineEnumFieldName,
                        static_cast<int>(routine.value()));
  }
  if (params.has_value())
    root_dict.SetKey(kParamsFieldName, std::move(params).value());
  std::string payload;
  base::JSONWriter::Write(root_dict, &payload);
  command_proto.set_payload(payload);
  return command_proto;
}

std::string CreateSuccessPayload(
    uint32_t id,
    chromeos::cros_healthd::mojom::DiagnosticRoutineStatusEnum status) {
  std::string payload;
  base::Value root_dict(base::Value::Type::DICTIONARY);
  root_dict.SetIntKey(kIdFieldName, static_cast<int>(id));
  root_dict.SetIntKey(kStatusFieldName, static_cast<int>(status));
  base::JSONWriter::Write(root_dict, &payload);
  return payload;
}

std::string CreateInvalidParametersFailurePayload() {
  std::string payload;
  base::Value root_dict(base::Value::Type::DICTIONARY);
  root_dict.SetIntKey(
      kIdFieldName,
      static_cast<int>(chromeos::cros_healthd::mojom::kFailedToStartId));
  root_dict.SetIntKey(
      kStatusFieldName,
      static_cast<int>(chromeos::cros_healthd::mojom::
                           DiagnosticRoutineStatusEnum::kFailedToStart));
  base::JSONWriter::Write(root_dict, &payload);
  return payload;
}

}  // namespace

class DeviceCommandRunRoutineJobTest : public testing::Test {
 protected:
  DeviceCommandRunRoutineJobTest();
  DeviceCommandRunRoutineJobTest(const DeviceCommandRunRoutineJobTest&) =
      delete;
  DeviceCommandRunRoutineJobTest& operator=(
      const DeviceCommandRunRoutineJobTest&) = delete;
  ~DeviceCommandRunRoutineJobTest() override;

  void InitializeJob(
      RemoteCommandJob* job,
      RemoteCommandJob::UniqueIDType unique_id,
      base::TimeTicks issued_time,
      base::TimeDelta idleness_cutoff,
      bool terminate_upon_input,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum routine,
      base::Value params);

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};

  base::TimeTicks test_start_time_;
};

DeviceCommandRunRoutineJobTest::DeviceCommandRunRoutineJobTest() {
  chromeos::CrosHealthdClient::InitializeFake();
  test_start_time_ = base::TimeTicks::Now();
}

DeviceCommandRunRoutineJobTest::~DeviceCommandRunRoutineJobTest() {
  chromeos::CrosHealthdClient::Shutdown();

  // Wait for DeviceCommandRunRoutineJobTest to observe the
  // destruction of the client.
  base::RunLoop().RunUntilIdle();
}

void DeviceCommandRunRoutineJobTest::InitializeJob(
    RemoteCommandJob* job,
    RemoteCommandJob::UniqueIDType unique_id,
    base::TimeTicks issued_time,
    base::TimeDelta idleness_cutoff,
    bool terminate_upon_input,
    chromeos::cros_healthd::mojom::DiagnosticRoutineEnum routine,
    base::Value params) {
  EXPECT_TRUE(job->Init(
      base::TimeTicks::Now(),
      GenerateCommandProto(unique_id, base::TimeTicks::Now() - issued_time,
                           idleness_cutoff, terminate_upon_input, routine,
                           std::move(params)),
      nullptr));

  EXPECT_EQ(unique_id, job->unique_id());
  EXPECT_EQ(RemoteCommandJob::NOT_STARTED, job->status());
}

TEST_F(DeviceCommandRunRoutineJobTest, InvalidRoutineEnumInCommandPayload) {
  base::Value params_dict(base::Value::Type::DICTIONARY);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  EXPECT_FALSE(job->Init(
      base::TimeTicks::Now(),
      GenerateCommandProto(
          kUniqueID, base::TimeTicks::Now() - test_start_time_,
          base::TimeDelta::FromSeconds(30),
          /*terminate_upon_input=*/false,
          static_cast<chromeos::cros_healthd::mojom::DiagnosticRoutineEnum>(
              std::numeric_limits<std::underlying_type<
                  chromeos::cros_healthd::mojom::DiagnosticRoutineEnum>::type>::
                  max()),
          std::move(params_dict)),
      nullptr));

  EXPECT_EQ(kUniqueID, job->unique_id());
  EXPECT_EQ(RemoteCommandJob::INVALID, job->status());
}

TEST_F(DeviceCommandRunRoutineJobTest, CommandPayloadMissingRoutine) {
  // Test that not specifying a routine causes the job initialization to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  EXPECT_FALSE(job->Init(
      base::TimeTicks::Now(),
      GenerateCommandProto(kUniqueID, base::TimeTicks::Now() - test_start_time_,
                           base::TimeDelta::FromSeconds(30),
                           /*terminate_upon_input=*/false,
                           /*routine=*/base::nullopt, std::move(params_dict)),
      nullptr));

  EXPECT_EQ(kUniqueID, job->unique_id());
  EXPECT_EQ(RemoteCommandJob::INVALID, job->status());
}

TEST_F(DeviceCommandRunRoutineJobTest, CommandPayloadMissingParamDict) {
  // Test that not including a parameters dictionary causes the routine
  // initialization to fail.
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  EXPECT_FALSE(job->Init(
      base::TimeTicks::Now(),
      GenerateCommandProto(
          kUniqueID, base::TimeTicks::Now() - test_start_time_,
          base::TimeDelta::FromSeconds(30),
          /*terminate_upon_input=*/false,
          chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kSmartctlCheck,
          /*params=*/base::nullopt),
      nullptr));

  EXPECT_EQ(kUniqueID, job->unique_id());
  EXPECT_EQ(RemoteCommandJob::INVALID, job->status());
}

TEST_F(DeviceCommandRunRoutineJobTest, RunBatteryCapacityRoutineSuccess) {
  auto run_routine_response =
      chromeos::cros_healthd::mojom::RunRoutineResponse::New(kId, kStatus);
  chromeos::cros_healthd::FakeCrosHealthdClient::Get()
      ->SetRunRoutineResponseForTesting(run_routine_response);
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kLowMahFieldName, /*low_mah=*/90812);
  params_dict.SetIntKey(kHighMahFieldName, /*high_mah=*/986909);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryCapacity,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::SUCCEEDED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateSuccessPayload(kId, kStatus), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunBatteryCapacityRoutineMissingLowMah) {
  // Test that leaving out the lowMah parameter causes the routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kHighMahFieldName, /*high_mah=*/986909);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryCapacity,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunBatteryCapacityRoutineMissingHighMah) {
  // Test that leaving out the highMah parameter causes the routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kLowMahFieldName, /*low_mah=*/90812);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryCapacity,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunBatteryCapacityRoutineInvalidLowMah) {
  // Test that a negative lowMah parameter causes the routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kLowMahFieldName, /*low_mah=*/-1);
  params_dict.SetIntKey(kHighMahFieldName, /*high_mah=*/986909);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryCapacity,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunBatteryCapacityRoutineInvalidHighMah) {
  // Test that a negative highMah parameter causes the routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kLowMahFieldName, /*low_mah=*/90812);
  params_dict.SetIntKey(kHighMahFieldName, /*high_mah=*/-1);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryCapacity,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunBatteryHealthRoutineSuccess) {
  auto run_routine_response =
      chromeos::cros_healthd::mojom::RunRoutineResponse::New(kId, kStatus);
  chromeos::cros_healthd::FakeCrosHealthdClient::Get()
      ->SetRunRoutineResponseForTesting(run_routine_response);
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kMaximumCycleCountFieldName,
                        /*maximum_cycle_count=*/12);
  params_dict.SetIntKey(kPercentBatteryWearAllowedFieldName,
                        /*percent_battery_wear_allowed=*/78);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryHealth,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::SUCCEEDED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateSuccessPayload(kId, kStatus), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunBatteryHealthRoutineMissingMaximumCycleCount) {
  // Test that leaving out the maximumCycleCount parameter causes the routine to
  // fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kPercentBatteryWearAllowedFieldName,
                        /*percent_battery_wear_allowed=*/78);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryHealth,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunBatteryHealthRoutineMissingPercentBatteryWearAllowed) {
  // Test that leaving out the percentBatteryWearAllowed parameter causes the
  // routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kMaximumCycleCountFieldName,
                        /*maximum_cycle_count=*/12);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryHealth,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunBatteryHealthRoutineInvalidMaximumCycleCount) {
  // Test that a negative maximumCycleCount parameter causes the routine to
  // fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kMaximumCycleCountFieldName,
                        /*maximum_cycle_count=*/-1);
  params_dict.SetIntKey(kPercentBatteryWearAllowedFieldName,
                        /*percent_battery_wear_allowed=*/78);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryHealth,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunBatteryHealthRoutineInvalidPercentBatteryWearAllowed) {
  // Test that a negative percentBatteryWearAllowed parameter causes the routine
  // to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kMaximumCycleCountFieldName,
                        /*maximum_cycle_count=*/12);
  params_dict.SetIntKey(kPercentBatteryWearAllowedFieldName,
                        /*percent_battery_wear_allowed=*/-1);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kBatteryHealth,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunUrandomRoutineSuccess) {
  auto run_routine_response =
      chromeos::cros_healthd::mojom::RunRoutineResponse::New(kId, kStatus);
  chromeos::cros_healthd::FakeCrosHealthdClient::Get()
      ->SetRunRoutineResponseForTesting(run_routine_response);
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kLengthSecondsFieldName,
                        /*length_seconds=*/2342);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kUrandom,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::SUCCEEDED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateSuccessPayload(kId, kStatus), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunUrandomRoutineMissingLengthSeconds) {
  // Test that leaving out the lengthSeconds parameter causes the routine to
  // fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kUrandom,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunUrandomRoutineInvalidLengthSeconds) {
  // Test that a negative lengthSeconds parameter causes the routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(kLengthSecondsFieldName,
                        /*length_seconds=*/-1);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kUrandom,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

// Note that the smartctl check routine has no parameters, so we only need to
// test that it can be run successfully.
TEST_F(DeviceCommandRunRoutineJobTest, RunSmartctlCheckRoutineSuccess) {
  auto run_routine_response =
      chromeos::cros_healthd::mojom::RunRoutineResponse::New(kId, kStatus);
  chromeos::cros_healthd::FakeCrosHealthdClient::Get()
      ->SetRunRoutineResponseForTesting(run_routine_response);
  base::Value params_dict(base::Value::Type::DICTIONARY);
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(
      job.get(), kUniqueID, test_start_time_, base::TimeDelta::FromSeconds(30),
      /*terminate_upon_input=*/false,
      chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kSmartctlCheck,
      std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::SUCCEEDED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateSuccessPayload(kId, kStatus), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunAcPowerRoutineSuccess) {
  // Test that the routine succeeds with all parameters specified.
  auto run_routine_response =
      chromeos::cros_healthd::mojom::RunRoutineResponse::New(kId, kStatus);
  chromeos::cros_healthd::FakeCrosHealthdClient::Get()
      ->SetRunRoutineResponseForTesting(run_routine_response);
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(
      kExpectedStatusFieldName,
      /*expected_status=*/static_cast<int>(
          chromeos::cros_healthd::mojom::AcPowerStatusEnum::kConnected));
  params_dict.SetStringKey(kExpectedPowerTypeFieldName,
                           /*expected_power_type=*/"power_type");
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kAcPower,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::SUCCEEDED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateSuccessPayload(kId, kStatus), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest,
       RunAcPowerRoutineNoOptionalExpectedPowerType) {
  // Test that the routine succeeds without the optional parameter
  // expectedPowerType specified.
  auto run_routine_response =
      chromeos::cros_healthd::mojom::RunRoutineResponse::New(kId, kStatus);
  chromeos::cros_healthd::FakeCrosHealthdClient::Get()
      ->SetRunRoutineResponseForTesting(run_routine_response);
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetIntKey(
      kExpectedStatusFieldName,
      /*expected_status=*/static_cast<int>(
          chromeos::cros_healthd::mojom::AcPowerStatusEnum::kConnected));
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kAcPower,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::SUCCEEDED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateSuccessPayload(kId, kStatus), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunAcPowerRoutineMissingExpectedStatus) {
  // Test that leaving out the expectedStatus parameter causes the routine to
  // fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  params_dict.SetStringKey(kExpectedPowerTypeFieldName,
                           /*expected_power_type=*/"power_type");
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kAcPower,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

TEST_F(DeviceCommandRunRoutineJobTest, RunAcPowerRoutineInvalidExpectedStatus) {
  // Test that an invalid value for the expectedStatus parameter causes the
  // routine to fail.
  base::Value params_dict(base::Value::Type::DICTIONARY);
  auto expected_status =
      static_cast<chromeos::cros_healthd::mojom::AcPowerStatusEnum>(
          std::numeric_limits<std::underlying_type<
              chromeos::cros_healthd::mojom::AcPowerStatusEnum>::type>::max());
  params_dict.SetIntKey(kExpectedStatusFieldName,
                        static_cast<int>(expected_status));
  params_dict.SetStringKey(kExpectedPowerTypeFieldName,
                           /*expected_power_type=*/"power_type");
  std::unique_ptr<RemoteCommandJob> job =
      std::make_unique<DeviceCommandRunRoutineJob>();
  InitializeJob(job.get(), kUniqueID, test_start_time_,
                base::TimeDelta::FromSeconds(30),
                /*terminate_upon_input=*/false,
                chromeos::cros_healthd::mojom::DiagnosticRoutineEnum::kAcPower,
                std::move(params_dict));
  base::RunLoop run_loop;
  bool success =
      job->Run(base::Time::Now(), base::TimeTicks::Now(),
               base::BindLambdaForTesting([&]() {
                 EXPECT_EQ(job->status(), RemoteCommandJob::FAILED);
                 std::unique_ptr<std::string> payload = job->GetResultPayload();
                 EXPECT_TRUE(payload);
                 EXPECT_EQ(CreateInvalidParametersFailurePayload(), *payload);
                 run_loop.Quit();
               }));
  EXPECT_TRUE(success);
  run_loop.Run();
}

}  // namespace policy
