// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media/history/media_history_store.h"

#include "base/optional.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/media/history/media_history_images_table.h"
#include "chrome/browser/media/history/media_history_keyed_service.h"
#include "chrome/browser/media/history/media_history_keyed_service_factory.h"
#include "chrome/browser/media/history/media_history_session_images_table.h"
#include "chrome/browser/media/history/media_history_session_table.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/media_session.h"
#include "content/public/test/test_utils.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/test/mock_media_session.h"

namespace media_history {

namespace {

constexpr base::TimeDelta kTestClipDuration =
    base::TimeDelta::FromMilliseconds(26771);

}  // namespace

class MediaHistoryBrowserTest : public InProcessBrowserTest {
 public:
  MediaHistoryBrowserTest() = default;
  ~MediaHistoryBrowserTest() override = default;

  void SetUp() override {
    scoped_feature_list_.InitAndEnableFeature(media::kUseMediaHistoryStore);

    InProcessBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());

    InProcessBrowserTest::SetUpOnMainThread();
  }

  bool SetupPageAndStartPlaying(const GURL& url) {
    ui_test_utils::NavigateToURL(browser(), url);

    bool played = false;
    EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
        GetWebContents(), "attemptPlay();", &played));
    return played;
  }

  bool SetMediaMetadata() {
    return content::ExecuteScript(GetWebContents(), "setMediaMetadata();");
  }

  bool SetMediaMetadataWithArtwork() {
    return content::ExecuteScript(GetWebContents(),
                                  "setMediaMetadataWithArtwork();");
  }

  bool FinishPlaying() {
    return content::ExecuteScript(GetWebContents(), "finishPlaying();");
  }

  std::vector<mojom::MediaHistoryPlaybackSessionRowPtr> GetPlaybackSessionsSync(
      int max_sessions) {
    return GetPlaybackSessionsSync(
        max_sessions, base::BindRepeating([](const base::TimeDelta& duration,
                                             const base::TimeDelta& position) {
          return duration.InSeconds() != position.InSeconds();
        }));
  }

  std::vector<mojom::MediaHistoryPlaybackSessionRowPtr> GetPlaybackSessionsSync(
      int max_sessions,
      MediaHistoryStore::GetPlaybackSessionsFilter filter) {
    base::RunLoop run_loop;
    std::vector<mojom::MediaHistoryPlaybackSessionRowPtr> out;

    GetMediaHistoryStore()->GetPlaybackSessions(
        max_sessions, std::move(filter),
        base::BindLambdaForTesting(
            [&](std::vector<mojom::MediaHistoryPlaybackSessionRowPtr>
                    sessions) {
              out = std::move(sessions);
              run_loop.Quit();
            }));

    run_loop.Run();
    return out;
  }

  mojom::MediaHistoryStatsPtr GetStatsSync() {
    base::RunLoop run_loop;
    mojom::MediaHistoryStatsPtr stats_out;

    GetMediaHistoryStore()->GetMediaHistoryStats(
        base::BindLambdaForTesting([&](mojom::MediaHistoryStatsPtr stats) {
          stats_out = std::move(stats);
          run_loop.Quit();
        }));

    run_loop.Run();
    return stats_out;
  }

  media_session::MediaMetadata GetExpectedMetadata() {
    media_session::MediaMetadata expected_metadata =
        GetExpectedDefaultMetadata();
    expected_metadata.title = base::ASCIIToUTF16("Big Buck Bunny");
    expected_metadata.artist = base::ASCIIToUTF16("Test Footage");
    expected_metadata.album = base::ASCIIToUTF16("The Chrome Collection");
    return expected_metadata;
  }

  std::vector<media_session::MediaImage> GetExpectedArtwork() {
    std::vector<media_session::MediaImage> images;

    {
      media_session::MediaImage image;
      image.src = embedded_test_server()->GetURL("/artwork-96.png");
      image.sizes.push_back(gfx::Size(96, 96));
      image.type = base::ASCIIToUTF16("image/png");
      images.push_back(image);
    }

    {
      media_session::MediaImage image;
      image.src = embedded_test_server()->GetURL("/artwork-128.png");
      image.sizes.push_back(gfx::Size(128, 128));
      image.type = base::ASCIIToUTF16("image/png");
      images.push_back(image);
    }

    {
      media_session::MediaImage image;
      image.src = embedded_test_server()->GetURL("/artwork-big.jpg");
      image.sizes.push_back(gfx::Size(192, 192));
      image.sizes.push_back(gfx::Size(256, 256));
      image.type = base::ASCIIToUTF16("image/jpg");
      images.push_back(image);
    }

    {
      media_session::MediaImage image;
      image.src = embedded_test_server()->GetURL("/artwork-any.jpg");
      image.sizes.push_back(gfx::Size(0, 0));
      image.type = base::ASCIIToUTF16("image/jpg");
      images.push_back(image);
    }

    {
      media_session::MediaImage image;
      image.src = embedded_test_server()->GetURL("/artwork-notype.jpg");
      image.sizes.push_back(gfx::Size(0, 0));
      images.push_back(image);
    }

    {
      media_session::MediaImage image;
      image.src = embedded_test_server()->GetURL("/artwork-nosize.jpg");
      image.type = base::ASCIIToUTF16("image/jpg");
      images.push_back(image);
    }

    return images;
  }

  media_session::MediaMetadata GetExpectedDefaultMetadata() {
    media_session::MediaMetadata expected_metadata;
    expected_metadata.title = base::ASCIIToUTF16("Media History");
    expected_metadata.source_title = base::ASCIIToUTF16(base::StringPrintf(
        "%s:%u", embedded_test_server()->GetIPLiteralString().c_str(),
        embedded_test_server()->port()));
    return expected_metadata;
  }

  void SimulateNavigationToCommit() {
    // Navigate to trigger the session to be saved.
    ui_test_utils::NavigateToURL(browser(), embedded_test_server()->base_url());

    // Wait until the session has finished saving.
    content::RunAllTasksUntilIdle();
  }

  const GURL GetTestURL() const {
    return embedded_test_server()->GetURL("/media/media_history.html");
  }

  const GURL GetTestAltURL() const {
    return embedded_test_server()->GetURL("/media/media_history.html?alt=1");
  }

  content::WebContents* GetWebContents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  content::MediaSession* GetMediaSession() {
    return content::MediaSession::Get(GetWebContents());
  }

  MediaHistoryStore* GetMediaHistoryStore() {
    return MediaHistoryKeyedServiceFactory::GetForProfile(browser()->profile())
        ->GetMediaHistoryStore();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(MediaHistoryBrowserTest,
                       RecordMediaSession_OnNavigate_Incomplete) {
  EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
  EXPECT_TRUE(SetMediaMetadataWithArtwork());

  auto expected_metadata = GetExpectedMetadata();
  auto expected_artwork = GetExpectedArtwork();

  {
    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_metadata);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kArtwork,
        expected_artwork);
  }

  SimulateNavigationToCommit();

  // Verify the session in the database.
  auto sessions = GetPlaybackSessionsSync(1);
  EXPECT_EQ(1u, sessions.size());
  EXPECT_EQ(GetTestURL(), sessions[0]->url);
  EXPECT_EQ(kTestClipDuration, sessions[0]->duration);
  EXPECT_LT(base::TimeDelta(), sessions[0]->position);
  EXPECT_EQ(expected_metadata.title, sessions[0]->metadata.title);
  EXPECT_EQ(expected_metadata.artist, sessions[0]->metadata.artist);
  EXPECT_EQ(expected_metadata.album, sessions[0]->metadata.album);
  EXPECT_EQ(expected_metadata.source_title, sessions[0]->metadata.source_title);
  EXPECT_EQ(expected_artwork, sessions[0]->artwork);

  {
    // Check the tables have the expected number of records
    mojom::MediaHistoryStatsPtr stats = GetStatsSync();
    EXPECT_EQ(1, stats->table_row_counts[MediaHistoryOriginTable::kTableName]);
    EXPECT_EQ(1, stats->table_row_counts[MediaHistorySessionTable::kTableName]);
    EXPECT_EQ(
        7, stats->table_row_counts[MediaHistorySessionImagesTable::kTableName]);
    EXPECT_EQ(6, stats->table_row_counts[MediaHistoryImagesTable::kTableName]);
  }
}

IN_PROC_BROWSER_TEST_F(MediaHistoryBrowserTest,
                       RecordMediaSession_DefaultMetadata) {
  EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));

  media_session::MediaMetadata expected_metadata = GetExpectedDefaultMetadata();

  {
    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_metadata);
  }

  SimulateNavigationToCommit();

  // Verify the session in the database.
  auto sessions = GetPlaybackSessionsSync(1);
  EXPECT_EQ(1u, sessions.size());
  EXPECT_EQ(GetTestURL(), sessions[0]->url);
  EXPECT_EQ(kTestClipDuration, sessions[0]->duration);
  EXPECT_LT(base::TimeDelta(), sessions[0]->position);
  EXPECT_EQ(expected_metadata.title, sessions[0]->metadata.title);
  EXPECT_EQ(expected_metadata.artist, sessions[0]->metadata.artist);
  EXPECT_EQ(expected_metadata.album, sessions[0]->metadata.album);
  EXPECT_EQ(expected_metadata.source_title, sessions[0]->metadata.source_title);
  EXPECT_TRUE(sessions[0]->artwork.empty());
}

IN_PROC_BROWSER_TEST_F(MediaHistoryBrowserTest,
                       RecordMediaSession_OnNavigate_Complete) {
  EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
  EXPECT_TRUE(FinishPlaying());

  media_session::MediaMetadata expected_metadata = GetExpectedDefaultMetadata();

  {
    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_metadata);
  }

  SimulateNavigationToCommit();

  {
    // The session will not be returned since it is complete.
    auto sessions = GetPlaybackSessionsSync(1);
    EXPECT_TRUE(sessions.empty());
  }

  {
    // If we remove the filter when we get the sessions we should see a result.
    auto sessions = GetPlaybackSessionsSync(
        1, base::BindRepeating(
               [](const base::TimeDelta& duration,
                  const base::TimeDelta& position) { return true; }));

    EXPECT_EQ(1u, sessions.size());
    EXPECT_EQ(GetTestURL(), sessions[0]->url);
  }
}

IN_PROC_BROWSER_TEST_F(MediaHistoryBrowserTest, DoNotRecordSessionIfNotActive) {
  ui_test_utils::NavigateToURL(browser(), GetTestURL());
  EXPECT_TRUE(SetMediaMetadata());

  media_session::MediaMetadata expected_metadata = GetExpectedDefaultMetadata();

  {
    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kInactive);
    observer.WaitForExpectedMetadata(expected_metadata);
  }

  SimulateNavigationToCommit();

  // Verify the session has not been stored in the database.
  auto sessions = GetPlaybackSessionsSync(1);
  EXPECT_TRUE(sessions.empty());
}

IN_PROC_BROWSER_TEST_F(MediaHistoryBrowserTest, GetPlaybackSessions) {
  auto expected_default_metadata = GetExpectedDefaultMetadata();

  {
    // Start a session.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
    EXPECT_TRUE(SetMediaMetadataWithArtwork());

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(GetExpectedMetadata());
  }

  SimulateNavigationToCommit();

  {
    // Start a second session on a different URL.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestAltURL()));

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_default_metadata);
  }

  SimulateNavigationToCommit();

  {
    // Get the two most recent playback sessions and check they are in order.
    auto sessions = GetPlaybackSessionsSync(2);
    EXPECT_EQ(2u, sessions.size());
    EXPECT_EQ(GetTestAltURL(), sessions[0]->url);
    EXPECT_EQ(GetTestURL(), sessions[1]->url);
  }

  {
    // Get the last playback session.
    auto sessions = GetPlaybackSessionsSync(1);
    EXPECT_EQ(1u, sessions.size());
    EXPECT_EQ(GetTestAltURL(), sessions[0]->url);
  }

  {
    // Start the first page again and seek to 4 seconds in with different
    // metadata.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
    EXPECT_TRUE(content::ExecuteScript(GetWebContents(), "seekToFour()"));

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_default_metadata);
  }

  SimulateNavigationToCommit();

  {
    // Check that recent playback sessions only returns two playback sessions
    // because the first one was collapsed into the third one since they
    // have the same URL. We should also use the data from the most recent
    // playback.
    auto sessions = GetPlaybackSessionsSync(3);
    EXPECT_EQ(2u, sessions.size());
    EXPECT_EQ(GetTestURL(), sessions[0]->url);
    EXPECT_EQ(GetTestAltURL(), sessions[1]->url);

    EXPECT_EQ(kTestClipDuration, sessions[0]->duration);
    EXPECT_EQ(4, sessions[0]->position.InSeconds());
    EXPECT_EQ(expected_default_metadata.title, sessions[0]->metadata.title);
    EXPECT_EQ(expected_default_metadata.artist, sessions[0]->metadata.artist);
    EXPECT_EQ(expected_default_metadata.album, sessions[0]->metadata.album);
    EXPECT_EQ(expected_default_metadata.source_title,
              sessions[0]->metadata.source_title);
  }

  {
    // Start the first page again and finish playing.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
    EXPECT_TRUE(FinishPlaying());

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_default_metadata);
  }

  SimulateNavigationToCommit();

  {
    // Get the recent playbacks and the test URL should not appear at all
    // because playback has completed for that URL.
    auto sessions = GetPlaybackSessionsSync(4);
    EXPECT_EQ(1u, sessions.size());
    EXPECT_EQ(GetTestAltURL(), sessions[0]->url);
  }

  {
    // Start the first session again.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
    EXPECT_TRUE(SetMediaMetadata());

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(GetExpectedMetadata());
  }

  SimulateNavigationToCommit();

  {
    // The test URL should now appear in the recent playbacks list again since
    // it is incomplete again.
    auto sessions = GetPlaybackSessionsSync(2);
    EXPECT_EQ(2u, sessions.size());
    EXPECT_EQ(GetTestURL(), sessions[0]->url);
    EXPECT_EQ(GetTestAltURL(), sessions[1]->url);
  }
}

IN_PROC_BROWSER_TEST_F(MediaHistoryBrowserTest,
                       SaveImagesWithDifferentSessions) {
  auto expected_metadata = GetExpectedMetadata();
  auto expected_artwork = GetExpectedArtwork();

  {
    // Start a session.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestURL()));
    EXPECT_TRUE(SetMediaMetadataWithArtwork());

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_metadata);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kArtwork,
        expected_artwork);
  }

  SimulateNavigationToCommit();

  std::vector<media_session::MediaImage> expected_alt_artwork;

  {
    media_session::MediaImage image;
    image.src = embedded_test_server()->GetURL("/artwork-96.png");
    image.sizes.push_back(gfx::Size(96, 96));
    image.type = base::ASCIIToUTF16("image/png");
    expected_alt_artwork.push_back(image);
  }

  {
    media_session::MediaImage image;
    image.src = embedded_test_server()->GetURL("/artwork-alt.png");
    image.sizes.push_back(gfx::Size(128, 128));
    image.type = base::ASCIIToUTF16("image/png");
    expected_alt_artwork.push_back(image);
  }

  {
    // Start a second session on a different URL.
    EXPECT_TRUE(SetupPageAndStartPlaying(GetTestAltURL()));
    EXPECT_TRUE(content::ExecuteScript(GetWebContents(),
                                       "setMediaMetadataWithAltArtwork();"));

    media_session::test::MockMediaSessionMojoObserver observer(
        *GetMediaSession());
    observer.WaitForState(
        media_session::mojom::MediaSessionInfo::SessionState::kActive);
    observer.WaitForExpectedMetadata(expected_metadata);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kArtwork,
        expected_alt_artwork);
  }

  SimulateNavigationToCommit();

  // Verify the session in the database.
  auto sessions = GetPlaybackSessionsSync(2);
  EXPECT_EQ(2u, sessions.size());
  EXPECT_EQ(GetTestAltURL(), sessions[0]->url);
  EXPECT_EQ(expected_alt_artwork, sessions[0]->artwork);
  EXPECT_EQ(GetTestURL(), sessions[1]->url);
  EXPECT_EQ(expected_artwork, sessions[1]->artwork);
}

}  // namespace media_history
