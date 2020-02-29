// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MEDIA_HISTORY_MEDIA_HISTORY_STORE_H_
#define CHROME_BROWSER_MEDIA_HISTORY_MEDIA_HISTORY_STORE_H_

#include <memory>
#include <vector>

#include "base/callback_forward.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/optional.h"
#include "base/updateable_sequenced_task_runner.h"
#include "chrome/browser/media/history/media_history_origin_table.h"
#include "chrome/browser/media/history/media_history_playback_table.h"
#include "chrome/browser/media/history/media_history_store.mojom.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/media_player_watch_time.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "sql/database.h"
#include "sql/init_status.h"
#include "sql/meta_table.h"

namespace media_session {
struct MediaImage;
struct MediaMetadata;
struct MediaPosition;
}  // namespace media_session

namespace sql {
class Database;
}  // namespace sql

namespace media_history {

class MediaHistoryStoreInternal;

class MediaHistoryStore {
 public:
  MediaHistoryStore(
      Profile* profile,
      scoped_refptr<base::UpdateableSequencedTaskRunner> db_task_runner);
  ~MediaHistoryStore();

  // Saves a playback from a single player in the media history store.
  void SavePlayback(const content::MediaPlayerWatchTime& watch_time);

  void GetMediaHistoryStats(
      base::OnceCallback<void(mojom::MediaHistoryStatsPtr)> callback);

  // Returns all the rows in the origin table. This should only be used for
  // debugging because it is very slow.
  void GetOriginRowsForDebug(
      base::OnceCallback<void(std::vector<mojom::MediaHistoryOriginRowPtr>)>
          callback);

  // Returns all the rows in the playback table. This is only used for
  // debugging because it loads all rows in the table.
  void GetMediaHistoryPlaybackRowsForDebug(
      base::OnceCallback<void(std::vector<mojom::MediaHistoryPlaybackRowPtr>)>
          callback);

  // Gets the playback sessions from the media history store. The results will
  // be ordered by most recent first and be limited to the first |num_sessions|.
  // For each session it calls |filter| and if that returns |true| then that
  // session will be included in the results.
  using GetPlaybackSessionsFilter =
      base::RepeatingCallback<bool(const base::TimeDelta& duration,
                                   const base::TimeDelta& position)>;
  void GetPlaybackSessions(
      base::Optional<unsigned int> num_sessions,
      base::Optional<GetPlaybackSessionsFilter> filter,
      base::OnceCallback<void(
          std::vector<mojom::MediaHistoryPlaybackSessionRowPtr>)> callback);

  // Saves a playback session in the media history store.
  void SavePlaybackSession(
      const GURL& url,
      const media_session::MediaMetadata& metadata,
      const base::Optional<media_session::MediaPosition>& position,
      const std::vector<media_session::MediaImage>& artwork);

  scoped_refptr<base::UpdateableSequencedTaskRunner> GetDBTaskRunnerForTest();

 protected:
  friend class MediaHistoryKeyedService;

  void EraseDatabaseAndCreateNew();

 private:
  scoped_refptr<MediaHistoryStoreInternal> db_;

  Profile* const profile_;
};

}  // namespace media_history

#endif  // CHROME_BROWSER_MEDIA_HISTORY_MEDIA_HISTORY_STORE_H_
