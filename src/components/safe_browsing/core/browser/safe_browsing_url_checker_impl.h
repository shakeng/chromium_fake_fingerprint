// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SAFE_BROWSING_CORE_BROWSER_SAFE_BROWSING_URL_CHECKER_IMPL_H_
#define COMPONENTS_SAFE_BROWSING_CORE_BROWSER_SAFE_BROWSING_URL_CHECKER_IMPL_H_

#include <vector>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "components/safe_browsing/core/common/safe_browsing_url_checker.mojom.h"
#include "components/safe_browsing/core/db/database_manager.h"
#include "components/safe_browsing/core/proto/realtimeapi.pb.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"

namespace blink {
namespace mojom {
enum class ResourceType;
}  // namespace mojom
}  // namespace blink

namespace content {
class WebContents;
}

namespace signin {
class IdentityManager;
}

namespace safe_browsing {

enum class ResourceType;

class UrlCheckerDelegate;

class VerdictCacheManager;

class RealTimeUrlLookupService;

// A SafeBrowsingUrlCheckerImpl instance is used to perform SafeBrowsing check
// for a URL and its redirect URLs. It implements Mojo interface so that it can
// be used to handle queries from renderers. But it is also used to handle
// queries from the browser. In that case, the public methods are called
// directly instead of through Mojo.
//
// To be considered "safe", a URL must not appear in the SafeBrowsing blacklists
// (see SafeBrowsingService for details).
//
// Note that the SafeBrowsing check takes at most kCheckUrlTimeoutMs
// milliseconds. If it takes longer than this, then the system defaults to
// treating the URL as safe.
//
// If the URL is classified as dangerous, a warning interstitial page is
// displayed. In that case, the user can click through the warning page if they
// decides to procced with loading the URL anyway.
class SafeBrowsingUrlCheckerImpl : public mojom::SafeBrowsingUrlChecker,
                                   public SafeBrowsingDatabaseManager::Client {
 public:
  using NativeUrlCheckNotifier =
      base::OnceCallback<void(bool /* proceed */,
                              bool /* showed_interstitial */)>;

  // If |slow_check_notifier| is not null, the callback is supposed to update
  // this output parameter with a callback to receive complete notification. In
  // that case, |proceed| and |showed_interstitial| should be ignored.
  using NativeCheckUrlCallback =
      base::OnceCallback<void(NativeUrlCheckNotifier* /* slow_check_notifier */,
                              bool /* proceed */,
                              bool /* showed_interstitial */)>;

  // Constructor for SafeBrowsingUrlCheckerImpl. |real_time_lookup_enabled|
  // indicates whether or not the profile has enabled real time URL lookups, as
  // computed by the RealTimePolicyEngine. This must be computed in advance,
  // since this class only exists on the IO thread.
  // TODO(crbug.com/1050859): Move |real_time_lookup_enabled|,
  // |cache_manager_on_ui| and |identity_manager_on_ui| into
  // url_lookup_service_on_ui, and reduce the number of parameters in this
  // constructor.
  SafeBrowsingUrlCheckerImpl(
      const net::HttpRequestHeaders& headers,
      int load_flags,
      blink::mojom::ResourceType resource_type,
      bool has_user_gesture,
      scoped_refptr<UrlCheckerDelegate> url_checker_delegate,
      const base::RepeatingCallback<content::WebContents*()>&
          web_contents_getter,
      bool real_time_lookup_enabled,
      base::WeakPtr<VerdictCacheManager> cache_manager_on_ui,
      signin::IdentityManager* identity_manager_on_ui,
      base::WeakPtr<RealTimeUrlLookupService> url_lookup_service_on_ui);

  ~SafeBrowsingUrlCheckerImpl() override;

  // mojom::SafeBrowsingUrlChecker implementation.
  // NOTE: |callback| could be run synchronously before this method returns. Be
  // careful if |callback| could destroy this object.
  void CheckUrl(const GURL& url,
                const std::string& method,
                CheckUrlCallback callback) override;

  // NOTE: |callback| could be run synchronously before this method returns. Be
  // careful if |callback| could destroy this object.
  void CheckUrl(const GURL& url,
                const std::string& method,
                NativeCheckUrlCallback callback);

 private:
  class Notifier {
   public:
    explicit Notifier(CheckUrlCallback callback);
    explicit Notifier(NativeCheckUrlCallback native_callback);

    ~Notifier();

    Notifier(Notifier&& other);
    Notifier& operator=(Notifier&& other);

    void OnStartSlowCheck();
    void OnCompleteCheck(bool proceed, bool showed_interstitial);

   private:
    // Used in the mojo interface case.
    CheckUrlCallback callback_;
    mojo::Remote<mojom::UrlCheckNotifier> slow_check_notifier_;

    // Used in the native call case.
    NativeCheckUrlCallback native_callback_;
    NativeUrlCheckNotifier native_slow_check_notifier_;
  };

  // SafeBrowsingDatabaseManager::Client implementation:
  void OnCheckBrowseUrlResult(const GURL& url,
                              SBThreatType threat_type,
                              const ThreatMetadata& metadata) override;
  void OnCheckUrlForHighConfidenceAllowlist(bool did_match_allowlist) override;

  // This function has to be static because it is called in UI thread,
  // |weak_checker_on_io| can only be accessed from IO thread.
  // This function is called if the url doesn't match the allowlist.
  static void StartGetCachedRealTimeUrlVerdictOnUI(
      base::WeakPtr<SafeBrowsingUrlCheckerImpl> weak_checker_on_io,
      base::WeakPtr<VerdictCacheManager> cache_manager_on_ui,
      const GURL& url,
      base::TimeTicks get_cache_start_time);

  // This function will start real time url lookup if there is no cache match.
  void OnGetCachedRealTimeUrlVerdictDoneOnIO(
      RTLookupResponse::ThreatInfo::VerdictType verdict_type,
      std::unique_ptr<RTLookupResponse::ThreatInfo> cached_threat_info,
      const GURL& url,
      base::TimeTicks get_cache_start_time);

  void OnTimeout();

  void OnUrlResult(const GURL& url,
                   SBThreatType threat_type,
                   const ThreatMetadata& metadata);

  void CheckUrlImpl(const GURL& url,
                    const std::string& method,
                    Notifier notifier);

  // NOTE: this method runs callbacks which could destroy this object.
  void ProcessUrls();

  // NOTE: this method runs callbacks which could destroy this object.
  void BlockAndProcessUrls(bool showed_interstitial);

  void OnBlockingPageComplete(bool proceed, bool showed_interstitial);

  // Helper method that checks whether |url|'s reputation can be checked using
  // real time lookups.
  bool CanPerformFullURLLookup(const GURL& url);

  SBThreatType CheckWebUIUrls(const GURL& url);

  // Returns false if this object has been destroyed by the callback. In that
  // case none of the members of this object should be touched again.
  bool RunNextCallback(bool proceed, bool showed_interstitial);

  // Perform the hash based check for the url.
  void PerformHashBasedCheck(const GURL& url);

  // This function has to be static because it is called in UI thread.
  // This function starts a real time url check if |url_lookup_service_on_ui| is
  // available and is not in backoff mode. Otherwise, hop back to IO thread and
  // perform hash based check.
  static void StartLookupOnUIThread(
      base::WeakPtr<SafeBrowsingUrlCheckerImpl> weak_checker_on_io,
      const GURL& url,
      base::WeakPtr<RealTimeUrlLookupService> url_lookup_service_on_ui,
      scoped_refptr<SafeBrowsingDatabaseManager> database_manager,
      signin::IdentityManager* identity_manager);

  // Called when the |request| from the real-time lookup service is sent.
  void OnRTLookupRequest(std::unique_ptr<RTLookupRequest> request);

  // Called when the |response| from the real-time lookup service is received.
  void OnRTLookupResponse(std::unique_ptr<RTLookupResponse> response);

  void SetWebUIToken(int token);

  enum State {
    // Haven't started checking or checking is complete.
    STATE_NONE,
    // We have one outstanding URL-check.
    STATE_CHECKING_URL,
    // We're displaying a blocking page.
    STATE_DISPLAYING_BLOCKING_PAGE,
    // The blocking page has returned *not* to proceed.
    STATE_BLOCKED
  };

  struct UrlInfo {
    UrlInfo(const GURL& url, const std::string& method, Notifier notifier);
    UrlInfo(UrlInfo&& other);

    ~UrlInfo();

    GURL url;
    std::string method;
    Notifier notifier;
  };

  const net::HttpRequestHeaders headers_;
  const int load_flags_;
  const ResourceType resource_type_;
  const bool has_user_gesture_;
  base::RepeatingCallback<content::WebContents*()> web_contents_getter_;
  scoped_refptr<UrlCheckerDelegate> url_checker_delegate_;
  scoped_refptr<SafeBrowsingDatabaseManager> database_manager_;

  // The redirect chain for this resource, including the original URL and
  // subsequent redirect URLs.
  std::vector<UrlInfo> urls_;
  // |urls_| before |next_index_| have been checked. If |next_index_| is smaller
  // than the size of |urls_|, the URL at |next_index_| is being processed.
  size_t next_index_ = 0;

  // Token used for displaying url real time lookup pings. A single token is
  // sufficient since real time check only happens on main frame url.
  int url_web_ui_token_ = -1;

  State state_ = STATE_NONE;

  // Timer to abort the SafeBrowsing check if it takes too long.
  base::OneShotTimer timer_;

  // Whether real time lookup is enabled for this request.
  bool real_time_lookup_enabled_;

  // Whether the browse url check request is sent to |database_manager_|.
  // This boolean is set to true once the first url check is sent, and never
  // reset to false, because there are separate pending checks for each request
  // to |database_manager_|. As long as the redirection is still happening,
  // there is at least one check that needs to be cancelled.
  bool browse_url_check_sent_ = false;

  // Unowned object used for getting and storing real time url check cache.
  // Must be NOT nullptr when real time url check is enabled and profile is not
  // delete. Can only be accessed in UI thread.
  base::WeakPtr<VerdictCacheManager> cache_manager_on_ui_;

  // This object is used to obtain access token when real time url check with
  // token is enabled. Can only be accessed in UI thread.
  signin::IdentityManager* identity_manager_on_ui_;

  // This object is used to perform real time url check. Can only be accessed in
  // UI thread.
  base::WeakPtr<RealTimeUrlLookupService> url_lookup_service_on_ui_;

  base::WeakPtrFactory<SafeBrowsingUrlCheckerImpl> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(SafeBrowsingUrlCheckerImpl);
};

}  // namespace safe_browsing

#endif  // COMPONENTS_SAFE_BROWSING_CORE_BROWSER_SAFE_BROWSING_URL_CHECKER_IMPL_H_
