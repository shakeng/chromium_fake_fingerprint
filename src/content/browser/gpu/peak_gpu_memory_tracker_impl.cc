// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/gpu/peak_gpu_memory_tracker_impl.h"

#include <memory>

#include "base/bind.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/task/post_task.h"
#include "base/threading/thread_task_runner_handle.h"
#include "content/browser/gpu/gpu_process_host.h"
#include "content/public/browser/gpu_data_manager.h"
#include "services/viz/privileged/mojom/gl/gpu_service.mojom.h"

namespace content {

namespace {

// Callback provided to the GpuService, which will be notified of the
// |peak_memory| used. This will then report that to UMA Histograms, for the
// requested |usage|. Some tests may provide an optional |testing_callback| in
// order to sync tests with the work done here on the IO thread.
void PeakMemoryCallback(PeakGpuMemoryTracker::Usage usage,
                        base::OnceClosure testing_callback,
                        const uint64_t peak_memory) {
  uint64_t memory_in_kb = peak_memory / 1024u;
  switch (usage) {
    case PeakGpuMemoryTracker::Usage::CHANGE_TAB:
      UMA_HISTOGRAM_MEMORY_KB("Memory.GPU.PeakMemoryUsage.ChangeTab",
                              memory_in_kb);
      break;
    case PeakGpuMemoryTracker::Usage::PAGE_LOAD:
      UMA_HISTOGRAM_MEMORY_KB("Memory.GPU.PeakMemoryUsage.PageLoad",
                              memory_in_kb);
      break;
    case PeakGpuMemoryTracker::Usage::SCROLL:
      UMA_HISTOGRAM_MEMORY_KB("Memory.GPU.PeakMemoryUsage.Scroll",
                              memory_in_kb);
      break;
  }
  std::move(testing_callback).Run();
}

}  // namespace

// static
std::unique_ptr<PeakGpuMemoryTracker> PeakGpuMemoryTracker::Create(
    PeakGpuMemoryTracker::Usage usage) {
  return std::make_unique<PeakGpuMemoryTrackerImpl>(usage);
}

// static
uint32_t PeakGpuMemoryTrackerImpl::next_sequence_number_ = 0;

PeakGpuMemoryTrackerImpl::PeakGpuMemoryTrackerImpl(
    PeakGpuMemoryTracker::Usage usage)
    : usage_(usage) {
  // Actually performs request to GPU service to begin memory tracking for
  // |sequence_number_|. This will normally be created from the UI thread, so
  // repost to the IO thread.
  GpuProcessHost::CallOnIO(
      GPU_PROCESS_KIND_SANDBOXED, /* force_create=*/false,
      base::BindOnce(
          [](uint32_t sequence_num, GpuProcessHost* host) {
            // There may be no host nor service available. This may occur during
            // shutdown, when the service is fully disabled, and in some tests.
            // In those cases do nothing.
            if (!host)
              return;
            if (auto* gpu_service = host->gpu_service()) {
              gpu_service->StartPeakMemoryMonitor(sequence_num);
            }
          },
          sequence_num_));
}

PeakGpuMemoryTrackerImpl::~PeakGpuMemoryTrackerImpl() {
  if (canceled_)
    return;

  GpuProcessHost::CallOnIO(
      GPU_PROCESS_KIND_SANDBOXED, /* force_create=*/false,
      base::BindOnce(
          [](uint32_t sequence_num, PeakGpuMemoryTracker::Usage usage,
             base::OnceClosure testing_callback, GpuProcessHost* host) {
            // There may be no host nor service available. This may occur during
            // shutdown, when the service is fully disabled, and in some tests.
            // In those cases there is nothing to report to UMA. However we
            // still run the optional testing callback.
            if (!host) {
              std::move(testing_callback).Run();
              return;
            }
            if (auto* gpu_service = host->gpu_service()) {
              gpu_service->GetPeakMemoryUsage(
                  sequence_num, base::BindOnce(&PeakMemoryCallback, usage,
                                               std::move(testing_callback)));
            }
          },
          sequence_num_, usage_,
          std::move(post_gpu_service_callback_for_testing_)));
}

void PeakGpuMemoryTrackerImpl::Cancel() {
  canceled_ = true;
  // Notify the GpuProcessHost that we are done observing this sequence.
  GpuProcessHost::CallOnIO(GPU_PROCESS_KIND_SANDBOXED, /* force_create=*/false,
                           base::BindOnce(
                               [](uint32_t sequence_num, GpuProcessHost* host) {
                                 if (!host)
                                   return;
                                 if (auto* gpu_service = host->gpu_service())
                                   gpu_service->GetPeakMemoryUsage(
                                       sequence_num, base::DoNothing());
                               },
                               sequence_num_));
}

}  // namespace content
