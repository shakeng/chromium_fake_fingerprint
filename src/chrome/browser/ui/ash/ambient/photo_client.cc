// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ash/ambient/photo_client.h"

#include "build/buildflag.h"
#include "chromeos/assistant/buildflags.h"

#if BUILDFLAG(ENABLE_CROS_LIBASSISTANT)
#include "chrome/browser/ui/ash/ambient/backdrop/photo_client_impl.h"
#endif

// static
std::unique_ptr<PhotoClient> PhotoClient::Create() {
#if BUILDFLAG(ENABLE_CROS_LIBASSISTANT)
  return std::make_unique<PhotoClientImpl>();
#else
  return std::make_unique<PhotoClient>();
#endif
}

void PhotoClient::FetchTopicInfo(OnTopicInfoFetchedCallback callback) {
  std::move(callback).Run(/*success=*/false, base::nullopt);
}
