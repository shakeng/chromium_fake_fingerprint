// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/power/ml/smart_dim/download_worker.h"

#include "base/bind.h"
#include "base/task/post_task.h"
#include "base/task/task_traits.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "chrome/browser/chromeos/power/ml/smart_dim/ml_agent_util.h"
#include "chromeos/services/machine_learning/public/cpp/service_connection.h"
#include "components/assist_ranker/proto/example_preprocessor.pb.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/resource/resource_bundle.h"

namespace chromeos {
namespace power {
namespace ml {

namespace {
using ::chromeos::machine_learning::mojom::FlatBufferModelSpec;
}  // namespace

DownloadWorker::DownloadWorker() : SmartDimWorker(), metrics_model_name_("") {}

DownloadWorker::~DownloadWorker() = default;

const assist_ranker::ExamplePreprocessorConfig*
DownloadWorker::GetPreprocessorConfig() {
  return preprocessor_config_.get();
}

const mojo::Remote<::chromeos::machine_learning::mojom::GraphExecutor>&
DownloadWorker::GetExecutor() {
  return executor_;
}

bool DownloadWorker::IsReady() {
  return preprocessor_config_ && model_ && executor_ &&
         expected_feature_size_ > 0 && metrics_model_name_ != "";
}

void DownloadWorker::InitializeFromComponent(
    const std::string& metadata_json,
    const std::string& preprocessor_proto,
    const std::string& model_flatbuffer) {
  // Meta data contains necessary info to construct model_spec_, and other
  // optional info.
  // TODO(crbug.com/1049886) move json parsing to the sandboxed separate parser.
  // TODO(crbug.com/1049888) add new UMA metrics to log the json errors.
  if (!ParseMetaInfoFromString(std::move(metadata_json), &metrics_model_name_,
                               &dim_threshold_, &expected_feature_size_,
                               &inputs_, &outputs_)) {
    DVLOG(1) << "Failed to parse metadata_json.";
    return;
  }

  preprocessor_config_ =
      std::make_unique<assist_ranker::ExamplePreprocessorConfig>();
  if (!preprocessor_config_->ParseFromString(preprocessor_proto)) {
    DVLOG(1) << "Failed to load preprocessor_config.";
    preprocessor_config_.reset();
    return;
  }

  base::PostTask(
      FROM_HERE, {content::BrowserThread::UI, base::TaskPriority::BEST_EFFORT},
      base::BindOnce(&DownloadWorker::LoadModelAndCreateGraphExecutor,
                     base::Unretained(this), std::move(model_flatbuffer)));
}

void DownloadWorker::LoadModelAndCreateGraphExecutor(
    const std::string& model_flatbuffer) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!model_.is_bound() && !executor_.is_bound());

  chromeos::machine_learning::ServiceConnection::GetInstance()
      ->LoadFlatBufferModel(
          FlatBufferModelSpec::New(std::move(model_flatbuffer), inputs_,
                                   outputs_, metrics_model_name_),
          model_.BindNewPipeAndPassReceiver(),
          base::BindOnce(&LoadModelCallback));
  model_->CreateGraphExecutor(executor_.BindNewPipeAndPassReceiver(),
                              base::BindOnce(&CreateGraphExecutorCallback));
  executor_.set_disconnect_handler(base::BindOnce(
      &DownloadWorker::OnConnectionError, base::Unretained(this)));
}

}  // namespace ml
}  // namespace power
}  // namespace chromeos
