// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_POWER_ML_SMART_DIM_DOWNLOAD_WORKER_H_
#define CHROME_BROWSER_CHROMEOS_POWER_ML_SMART_DIM_DOWNLOAD_WORKER_H_

#include "base/containers/flat_map.h"
#include "chrome/browser/chromeos/power/ml/smart_dim/smart_dim_worker.h"

namespace chromeos {
namespace power {
namespace ml {

// SmartDimWorker that loads meta info, preprocessor config and ML service model
// files from smart dim components.
class DownloadWorker : public SmartDimWorker {
 public:
  DownloadWorker();
  ~DownloadWorker() override;

  // SmartDimWorker overrides:
  const assist_ranker::ExamplePreprocessorConfig* GetPreprocessorConfig()
      override;
  const mojo::Remote<::chromeos::machine_learning::mojom::GraphExecutor>&
  GetExecutor() override;

  // Returns true if it has loaded components successfully.
  bool IsReady();

  // Loads meta info, preprocessor config and ML service model from smart dim
  // components.
  // Called by component updater when it gets a verified smart dim component and
  // DownloadWorker is not ready.
  // If IsReady(), this function won't be called again.
  void InitializeFromComponent(const std::string& metadata_json,
                               const std::string& preprocessor_proto,
                               const std::string& model_flatbuffer);

 private:
  base::flat_map<std::string, int> inputs_;
  base::flat_map<std::string, int> outputs_;
  std::string metrics_model_name_;

  void LoadModelAndCreateGraphExecutor(const std::string& model_flatbuffer);

  DISALLOW_COPY_AND_ASSIGN(DownloadWorker);
};

}  // namespace ml
}  // namespace power
}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_POWER_ML_SMART_DIM_DOWNLOAD_WORKER_H_
