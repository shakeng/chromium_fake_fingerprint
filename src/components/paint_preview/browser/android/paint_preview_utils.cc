// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/paint_preview/browser/android/paint_preview_utils.h"

#include <jni.h>

#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "base/task/post_task.h"
#include "base/unguessable_token.h"
#include "components/paint_preview/browser/android/jni_headers/PaintPreviewUtils_jni.h"
#include "components/paint_preview/browser/file_manager.h"
#include "components/paint_preview/browser/paint_preview_client.h"
#include "components/paint_preview/buildflags/buildflags.h"
#include "components/ukm/content/source_url_recorder.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "services/metrics/public/cpp/metrics_utils.h"
#include "services/metrics/public/cpp/ukm_builders.h"
#include "services/metrics/public/cpp/ukm_recorder.h"

namespace paint_preview {

namespace {

const char kPaintPreviewTestTag[] = "PaintPreviewTest ";
const char kPaintPreviewDir[] = "paint_preview";
const char kCaptureTestDir[] = "capture_test";
const char kProtoFilename[] = "proto.pb";

struct CaptureMetrics {
  int compressed_size_bytes;
  base::TimeDelta capture_time;
  ukm::SourceId source_id;
};

void CleanupOnFailure(const base::FilePath& root_dir,
                      FinishedCallback finished) {
  VLOG(1) << kPaintPreviewTestTag << "Capture Failed\n";
  base::DeleteFileRecursively(root_dir);
  std::move(finished).Run(base::nullopt);
}

void CleanupAndLogResult(const base::FilePath& zip_path,
                         const CaptureMetrics& metrics,
                         FinishedCallback finished,
                         bool keep_zip) {
  VLOG(1) << kPaintPreviewTestTag << "Capture Finished Successfully:\n"
          << "Compressed size " << metrics.compressed_size_bytes << " bytes\n"
          << "Time taken in native " << metrics.capture_time.InMilliseconds()
          << " ms";

  if (!keep_zip)
    base::DeleteFileRecursively(zip_path.DirName());

  base::UmaHistogramMemoryKB(
      "Browser.PaintPreview.CaptureExperiment.CompressedOnDiskSize",
      metrics.compressed_size_bytes / 1000);
  if (metrics.source_id != ukm::kInvalidSourceId) {
    ukm::builders::PaintPreviewCapture(metrics.source_id)
        .SetCompressedOnDiskSize(
            ukm::GetExponentialBucketMinForBytes(metrics.compressed_size_bytes))
        .Record(ukm::UkmRecorder::Get());
  }
  std::move(finished).Run(zip_path);
}

void CompressAndMeasureSize(const base::FilePath& root_dir,
                            const GURL& url,
                            std::unique_ptr<PaintPreviewProto> proto,
                            CaptureMetrics metrics,
                            FinishedCallback finished,
                            bool keep_zip) {
  FileManager manager(root_dir);
  base::FilePath path;
  bool success = manager.CreateOrGetDirectoryFor(url, &path);
  if (!success) {
    VLOG(1) << kPaintPreviewTestTag << "Failure: could not find url dir.";
    CleanupOnFailure(root_dir, std::move(finished));
    return;
  }
  base::File file(path.AppendASCII(kProtoFilename),
                  base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
  std::string str_proto = proto->SerializeAsString();
  file.WriteAtCurrentPos(str_proto.data(), str_proto.size());
  manager.CompressDirectoryFor(url);
  metrics.compressed_size_bytes = manager.GetSizeOfArtifactsFor(url);

  CleanupAndLogResult(path.AddExtensionASCII("zip"), metrics,
                      std::move(finished), keep_zip);
}

void OnCaptured(base::TimeTicks start_time,
                const base::FilePath& root_dir,
                const GURL& url,
                ukm::SourceId source_id,
                FinishedCallback finished,
                bool keep_zip,
                base::UnguessableToken guid,
                mojom::PaintPreviewStatus status,
                std::unique_ptr<PaintPreviewProto> proto) {
  base::TimeDelta time_delta = base::TimeTicks::Now() - start_time;

  bool success = status == mojom::PaintPreviewStatus::kOk;
  base::UmaHistogramBoolean("Browser.PaintPreview.CaptureExperiment.Success",
                            success);
  if (!success) {
    base::PostTask(
        FROM_HERE, {base::ThreadPool(), base::MayBlock()},
        base::BindOnce(&CleanupOnFailure, root_dir, std::move(finished)));
    return;
  }

  CaptureMetrics result = {0, time_delta, source_id};
  base::PostTask(
      FROM_HERE, {base::ThreadPool(), base::MayBlock()},
      base::BindOnce(&CompressAndMeasureSize, root_dir, url, std::move(proto),
                     result, std::move(finished), keep_zip));
}

base::Optional<base::FilePath> CreateDirectoryForURL(
    const base::FilePath& root_path,
    const GURL& url) {
  base::FilePath url_path;
  FileManager manager(root_path);
  if (!manager.CreateOrGetDirectoryFor(url, &url_path)) {
    VLOG(1) << kPaintPreviewTestTag << "Failure: could not create output dir.";
    return base::nullopt;
  }
  return url_path;
}

void InitiateCapture(content::WebContents* contents,
                     FinishedCallback finished,
                     bool keep_zip,
                     base::Optional<base::FilePath> url_path) {
  if (!url_path.has_value()) {
    std::move(finished).Run(base::nullopt);
    return;
  }

  auto* client = PaintPreviewClient::FromWebContents(contents);
  if (!client) {
    VLOG(1) << kPaintPreviewTestTag << "Failure: client could not be created.";
    CleanupOnFailure(url_path->DirName(), std::move(finished));
    return;
  }

  PaintPreviewClient::PaintPreviewParams params;
  params.document_guid = base::UnguessableToken::Create();
  params.is_main_frame = true;
  params.root_dir = url_path.value();

  ukm::SourceId source_id = ukm::GetSourceIdForWebContentsDocument(contents);
  auto start_time = base::TimeTicks::Now();
  client->CapturePaintPreview(
      params, contents->GetMainFrame(),
      base::BindOnce(&OnCaptured, start_time, params.root_dir.DirName(),
                     contents->GetLastCommittedURL(), source_id,
                     std::move(finished), keep_zip));
}

}  // namespace

void Capture(content::WebContents* contents,
             FinishedCallback finished,
             bool keep_zip) {
  PaintPreviewClient::CreateForWebContents(contents);

  base::FilePath root_path = contents->GetBrowserContext()
                                 ->GetPath()
                                 .AppendASCII(kPaintPreviewDir)
                                 .AppendASCII(kCaptureTestDir);

  base::PostTaskAndReplyWithResult(
      FROM_HERE, {base::ThreadPool(), base::MayBlock()},
      base::BindOnce(&CreateDirectoryForURL, root_path,
                     contents->GetLastCommittedURL()),
      base::BindOnce(&InitiateCapture, base::Unretained(contents),
                     std::move(finished), keep_zip));
}

}  // namespace paint_preview

// If the ENABLE_PAINT_PREVIEW buildflags is set this method will trigger a
// series of actions;
// 1. Capture a paint preview via the client and measure the time taken.
// 2. Zip a folder containing the artifacts and measure the size of the zip.
// 3. Delete the resulting zip archive.
// 4. Log the results.
// If the buildflag is not set this is just a stub.
static void JNI_PaintPreviewUtils_CapturePaintPreview(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& jweb_contents) {
#if BUILDFLAG(ENABLE_PAINT_PREVIEW)
  auto* contents = content::WebContents::FromJavaWebContents(jweb_contents);
  paint_preview::Capture(contents, base::DoNothing(), /* keep_zip= */ false);
#else
  // In theory this is unreachable as the codepath to reach here is only exposed
  // if the buildflag for ENABLE_PAINT_PREVIEW is set. However, this function
  // will still be compiled as it is called from JNI so this is here as a
  // placeholder.
  VLOG(1) << paint_preview::kPaintPreviewTestTag
          << "Failure: compiled without buildflag ENABLE_PAINT_PREVIEW.";
#endif  // BUILDFLAG(ENABLE_PAINT_PREVIEW)
}
