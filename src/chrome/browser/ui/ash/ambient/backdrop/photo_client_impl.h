// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_ASH_AMBIENT_BACKDROP_PHOTO_CLIENT_IMPL_H_
#define CHROME_BROWSER_UI_ASH_AMBIENT_BACKDROP_PHOTO_CLIENT_IMPL_H_

#include <memory>

#include "base/callback.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/ash/ambient/photo_client.h"
#include "chromeos/assistant/internal/ambient/backdrop_client_config.h"

class BackdropURLLoader;
class GoogleServiceAuthError;

namespace signin {
class AccessTokenFetcher;
struct AccessTokenInfo;
}  // namespace signin

// The photo client impl talks to Backdrop service.
class PhotoClientImpl : public PhotoClient {
 public:
  PhotoClientImpl();
  ~PhotoClientImpl() override;
  PhotoClientImpl(const PhotoClientImpl&) = delete;
  PhotoClientImpl& operator=(const PhotoClientImpl&) = delete;

  // PhotoClient:
  void FetchTopicInfo(OnTopicInfoFetchedCallback callback) override;

 private:
  using GetAccessTokenCallback =
      base::OnceCallback<void(const std::string& gaia_id,
                              GoogleServiceAuthError error,
                              signin::AccessTokenInfo access_token_info)>;

  void RequestAccessToken(GetAccessTokenCallback callback);

  void StartToFetchTopicInfo(OnTopicInfoFetchedCallback callback,
                             const std::string& gaia_id,
                             GoogleServiceAuthError error,
                             signin::AccessTokenInfo access_token_info);

  void OnTopicInfoFetched(OnTopicInfoFetchedCallback callback,
                          std::unique_ptr<std::string> response);

  // The url loader for the Backdrop service request.
  std::unique_ptr<BackdropURLLoader> backdrop_url_loader_;

  std::unique_ptr<signin::AccessTokenFetcher> access_token_fetcher_;

  chromeos::ambient::BackdropClientConfig backdrop_client_config_;

  base::WeakPtrFactory<PhotoClientImpl> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_ASH_AMBIENT_BACKDROP_PHOTO_CLIENT_IMPL_H_
