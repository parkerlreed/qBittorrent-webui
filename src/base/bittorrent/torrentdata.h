/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2025  qBittorrent project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

#include <QDateTime>
#include <QString>
#include <QVariant>

#include "base/path.h"
#include "base/tagset.h"
#include "infohash.h"
#include "torrent.h"
#include "torrentannouncestatus.h"

namespace BitTorrent
{
    // Plain value type populated from /api/v2/sync/maindata torrent entries.
    // Fields mirror the KEY_TORRENT_* keys in serialize_torrent.h.
    // fromVariantMap() does a full population; mergeUpdate() applies a partial delta.
    struct TorrentData
    {
        TorrentID id;
        InfoHash  infoHash;            // constructed from infohash_v1 / infohash_v2
        QString   name;
        QString   comment;
        QString   creator;
        bool      hasMetadata       = false;
        bool      isPrivate         = false;

        // Sizes
        qlonglong totalSize         = 0;  // total_size (all files, including skipped)
        qlonglong size              = 0;  // size (wanted)
        qlonglong completedSize     = 0;  // completed
        qlonglong amountLeft        = 0;  // amount_left
        qlonglong wastedSize        = 0;  // total_wasted

        // Progress / state
        qreal     progress          = 0;  // 0..1
        TorrentState state          = TorrentState::StoppedDownloading;
        TorrentAnnounceStatus announceStatus;

        // Speeds / ETA
        int       downloadPayloadRate = 0;  // dlspeed (bytes/s)
        int       uploadPayloadRate   = 0;  // upspeed
        qlonglong eta               = -1;

        // Counts
        int       seedsCount        = 0;   // num_seeds
        int       totalSeedsCount   = 0;   // num_complete
        int       leechsCount       = 0;   // num_leechs
        int       totalLeechersCount = 0;  // num_incomplete
        int       connectionsCount  = 0;
        int       connectionsLimit  = 0;

        // Ratio / popularity
        qreal     ratio             = 0;
        qreal     popularity        = 0;
        qreal     distributedCopies = -1;

        // Piece info
        int       piecesCount       = 0;   // pieces_num
        int       piecesHave        = 0;   // pieces_have
        qlonglong pieceLength       = 0;   // piece_size

        // Limits
        int       uploadLimit       = -1;
        int       downloadLimit     = -1;
        qreal     ratioLimit        = Torrent::MAX_RATIO;     // ratio_limit
        int       seedingTimeLimit  = -2;  // seeding_time_limit
        int       inactiveSeedingTimeLimit = -2;
        ShareLimitAction shareLimitAction = ShareLimitAction::Default;

        // Timestamps
        QDateTime addedTime;          // added_on (epoch secs)
        QDateTime completedTime;      // completion_on
        QDateTime lastSeenComplete;   // seen_complete
        QDateTime lastActivity;       // last_activity
        QDateTime creationDate;       // creation_date
        qlonglong activeTime        = 0;   // time_active
        qlonglong finishedTime      = 0;   // seeding_time
        qlonglong timeSinceUpload   = 0;
        qlonglong timeSinceDownload = 0;
        qlonglong timeSinceActivity = 0;

        // Download totals
        qlonglong totalDownload        = 0;   // downloaded
        qlonglong totalUpload          = 0;   // uploaded
        qlonglong totalDownloadSession = 0;   // downloaded_session
        qlonglong totalUploadSession   = 0;   // uploaded_session
        qlonglong totalPayloadDownload = 0;
        qlonglong totalPayloadUpload   = 0;

        // Paths
        Path      savePath;
        Path      downloadPath;
        Path      rootPath;
        Path      contentPath;
        Path      actualStorageLocation;   // content_path for file-level access

        // Category / tags
        QString   category;
        TagSet    tags;

        // Tracker
        QString   currentTracker;    // tracker
        int       trackersCount     = 0;
        qlonglong nextAnnounce      = 0;   // reannounce

        // Flags
        bool      autoTMM              = false;  // auto_tmm
        bool      superSeeding         = false;  // super_seeding
        bool      sequentialDownload   = false;  // seq_dl
        bool      firstLastPiecePrio   = false;  // f_l_piece_prio
        bool      forceStart           = false;  // force_start
        bool      isDHTDisabled        = false;
        bool      isPEXDisabled        = false;
        bool      isLSDDisabled        = false;

        // Queue
        int       queuePosition        = 0;   // priority (0 = not queued)

        // Error
        QString   error;

        // Availability (fraction of pieces available from at least one peer)
        qreal     availability         = 0;

        // Announce status flags (synthesized from has_tracker_warning / has_tracker_error)
        bool      hasTrackerWarning    = false;
        bool      hasTrackerError      = false;
        bool      hasOtherAnnounceError = false;

        // Populate all fields from a single maindata torrent entry.
        // hashStr is the key in the maindata["torrents"] map.
        static TorrentData fromVariantMap(const QString &hashStr, const QVariantMap &map);

        // Merge a partial update delta (only keys present in `delta` are updated).
        void mergeUpdate(const QVariantMap &delta);
    };
}
