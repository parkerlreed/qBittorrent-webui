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

#include "torrentdata.h"

#include <QDateTime>
#include <QMetaEnum>

#include "base/global.h"
#include "base/tag.h"
#include "base/tagset.h"
#include "base/utils/string.h"
#include "infohash.h"
#include "sharelimits.h"

namespace
{
    // Parse epoch-seconds from a QVariant (int or qlonglong) to QDateTime.
    // Returns invalid QDateTime for negative/zero values (API convention for "never").
    QDateTime epochToDateTime(const QVariant &v)
    {
        const qlonglong secs = v.toLongLong();
        if (secs <= 0)
            return {};
        return QDateTime::fromSecsSinceEpoch(secs);
    }

    // Parse API state string → TorrentState enum.
    BitTorrent::TorrentState parseState(const QString &s)
    {
        if (s == u"error"_s)                return BitTorrent::TorrentState::Error;
        if (s == u"missingFiles"_s)         return BitTorrent::TorrentState::MissingFiles;
        if (s == u"uploading"_s)            return BitTorrent::TorrentState::Uploading;
        if (s == u"stoppedUP"_s)            return BitTorrent::TorrentState::StoppedUploading;
        if (s == u"queuedUP"_s)             return BitTorrent::TorrentState::QueuedUploading;
        if (s == u"stalledUP"_s)            return BitTorrent::TorrentState::StalledUploading;
        if (s == u"checkingUP"_s)           return BitTorrent::TorrentState::CheckingUploading;
        if (s == u"forcedUP"_s)             return BitTorrent::TorrentState::ForcedUploading;
        if (s == u"downloading"_s)          return BitTorrent::TorrentState::Downloading;
        if (s == u"metaDL"_s)               return BitTorrent::TorrentState::DownloadingMetadata;
        if (s == u"forcedMetaDL"_s)         return BitTorrent::TorrentState::ForcedDownloadingMetadata;
        if (s == u"stoppedDL"_s)            return BitTorrent::TorrentState::StoppedDownloading;
        if (s == u"queuedDL"_s)             return BitTorrent::TorrentState::QueuedDownloading;
        if (s == u"stalledDL"_s)            return BitTorrent::TorrentState::StalledDownloading;
        if (s == u"checkingDL"_s)           return BitTorrent::TorrentState::CheckingDownloading;
        if (s == u"forcedDL"_s)             return BitTorrent::TorrentState::ForcedDownloading;
        if (s == u"checkingResumeData"_s)   return BitTorrent::TorrentState::CheckingResumeData;
        if (s == u"moving"_s)               return BitTorrent::TorrentState::Moving;
        return BitTorrent::TorrentState::StoppedDownloading;
    }

    // Parse comma-and-space-separated tag string → TagSet.
    TagSet parseTags(const QString &s)
    {
        TagSet result;
        if (s.isEmpty())
            return result;
        const QStringList parts = s.split(u", "_s, Qt::SkipEmptyParts);
        for (const QString &part : parts)
            result.insert(Tag(part.trimmed()));
        return result;
    }

    // API adjusts queuePosition: -1 internal → 0 in API; 0 internal → 1 in API.
    // Reverse: 0 in API → -1 internal; N in API → N-1 internal.
    int parseQueuePosition(int apiValue)
    {
        return (apiValue <= 0) ? -1 : (apiValue - 1);
    }

    // API adjusts ratio: MAX_RATIO or above → -1. Reverse: -1 → MAX_RATIO.
    qreal parseRatio(qreal apiValue)
    {
        return (apiValue < 0) ? BitTorrent::Torrent::MAX_RATIO : apiValue;
    }

    // Build TorrentAnnounceStatus from the three announce-status booleans.
    BitTorrent::TorrentAnnounceStatus parseAnnounceStatus(bool warning, bool error, bool other)
    {
        BitTorrent::TorrentAnnounceStatus status;
        if (warning)
            status |= BitTorrent::TorrentAnnounceStatusFlag::HasWarning;
        if (error)
            status |= BitTorrent::TorrentAnnounceStatusFlag::HasTrackerError;
        if (other)
            status |= BitTorrent::TorrentAnnounceStatusFlag::HasOtherError;
        return status;
    }

    // Apply a single key/value pair from the map to the TorrentData struct.
    // Used by both fromVariantMap and mergeUpdate.
    void applyField(BitTorrent::TorrentData &d, const QString &key, const QVariant &v)
    {
        if (key == u"name"_s)
        {
            d.name = v.toString();
        }
        else if (key == u"has_metadata"_s)
        {
            d.hasMetadata = v.toBool();
        }
        else if (key == u"private"_s)
        {
            if (!v.isNull())
                d.isPrivate = v.toBool();
        }
        else if (key == u"created_by"_s)
        {
            d.creator = v.toString();
        }
        else if (key == u"creation_date"_s)
        {
            d.creationDate = epochToDateTime(v);
        }
        else if (key == u"comment"_s)
        {
            d.comment = v.toString();
        }
        else if (key == u"total_size"_s)
        {
            d.totalSize = v.toLongLong();
        }
        else if (key == u"size"_s)
        {
            d.size = v.toLongLong();
        }
        else if (key == u"completed"_s)
        {
            d.completedSize = v.toLongLong();
        }
        else if (key == u"amount_left"_s)
        {
            d.amountLeft = v.toLongLong();
        }
        else if (key == u"total_wasted"_s)
        {
            d.wastedSize = v.toLongLong();
        }
        else if (key == u"progress"_s)
        {
            d.progress = v.toDouble();
        }
        else if (key == u"state"_s)
        {
            d.state = parseState(v.toString());
        }
        else if (key == u"dlspeed"_s)
        {
            d.downloadPayloadRate = v.toInt();
        }
        else if (key == u"upspeed"_s)
        {
            d.uploadPayloadRate = v.toInt();
        }
        else if (key == u"eta"_s)
        {
            d.eta = v.toLongLong();
        }
        else if (key == u"num_seeds"_s)
        {
            d.seedsCount = v.toInt();
        }
        else if (key == u"num_complete"_s)
        {
            d.totalSeedsCount = v.toInt();
        }
        else if (key == u"num_leechs"_s)
        {
            d.leechsCount = v.toInt();
        }
        else if (key == u"num_incomplete"_s)
        {
            d.totalLeechersCount = v.toInt();
        }
        else if (key == u"connections_count"_s)
        {
            d.connectionsCount = v.toInt();
        }
        else if (key == u"connections_limit"_s)
        {
            d.connectionsLimit = v.toInt();
        }
        else if (key == u"ratio"_s)
        {
            d.ratio = parseRatio(v.toDouble());
        }
        else if (key == u"popularity"_s)
        {
            d.popularity = v.toDouble();
        }
        else if (key == u"availability"_s)
        {
            d.distributedCopies = v.toDouble();
            d.availability = d.distributedCopies;
        }
        else if (key == u"pieces_num"_s)
        {
            d.piecesCount = v.toInt();
        }
        else if (key == u"pieces_have"_s)
        {
            d.piecesHave = v.toInt();
        }
        else if (key == u"piece_size"_s)
        {
            d.pieceLength = v.toLongLong();
        }
        else if (key == u"up_limit"_s)
        {
            d.uploadLimit = v.toInt();
        }
        else if (key == u"dl_limit"_s)
        {
            d.downloadLimit = v.toInt();
        }
        else if (key == u"ratio_limit"_s)
        {
            d.ratioLimit = v.toDouble();
        }
        else if (key == u"seeding_time_limit"_s)
        {
            d.seedingTimeLimit = v.toInt();
        }
        else if (key == u"inactive_seeding_time_limit"_s)
        {
            d.inactiveSeedingTimeLimit = v.toInt();
        }
        else if (key == u"share_limit_action"_s)
        {
            d.shareLimitAction = Utils::String::toEnum<BitTorrent::ShareLimitAction>(
                v.toString(), BitTorrent::ShareLimitAction::Default);
        }
        else if (key == u"added_on"_s)
        {
            d.addedTime = epochToDateTime(v);
        }
        else if (key == u"completion_on"_s)
        {
            d.completedTime = epochToDateTime(v);
        }
        else if (key == u"seen_complete"_s)
        {
            d.lastSeenComplete = epochToDateTime(v);
        }
        else if (key == u"last_activity"_s)
        {
            // last_activity is an epoch timestamp for the last activity
            const qlonglong epochSecs = v.toLongLong();
            d.lastActivity = (epochSecs > 0) ? QDateTime::fromSecsSinceEpoch(epochSecs) : QDateTime{};
            // Compute timeSinceActivity relative to now
            if (epochSecs > 0)
                d.timeSinceActivity = QDateTime::currentSecsSinceEpoch() - epochSecs;
            else
                d.timeSinceActivity = -1;
        }
        else if (key == u"time_active"_s)
        {
            d.activeTime = v.toLongLong();
        }
        else if (key == u"seeding_time"_s)
        {
            d.finishedTime = v.toLongLong();
        }
        else if (key == u"downloaded"_s)
        {
            d.totalDownload = v.toLongLong();
        }
        else if (key == u"uploaded"_s)
        {
            d.totalUpload = v.toLongLong();
        }
        else if (key == u"downloaded_session"_s)
        {
            d.totalDownloadSession = v.toLongLong();
        }
        else if (key == u"uploaded_session"_s)
        {
            d.totalUploadSession = v.toLongLong();
        }
        else if (key == u"save_path"_s)
        {
            d.savePath = Path(v.toString());
        }
        else if (key == u"download_path"_s)
        {
            d.downloadPath = Path(v.toString());
        }
        else if (key == u"root_path"_s)
        {
            d.rootPath = Path(v.toString());
        }
        else if (key == u"content_path"_s)
        {
            d.contentPath = Path(v.toString());
            d.actualStorageLocation = d.contentPath;
        }
        else if (key == u"category"_s)
        {
            d.category = v.toString();
        }
        else if (key == u"tags"_s)
        {
            d.tags = parseTags(v.toString());
        }
        else if (key == u"tracker"_s)
        {
            d.currentTracker = v.toString();
        }
        else if (key == u"trackers_count"_s)
        {
            d.trackersCount = v.toInt();
        }
        else if (key == u"reannounce"_s)
        {
            d.nextAnnounce = v.toLongLong();
        }
        else if (key == u"auto_tmm"_s)
        {
            d.autoTMM = v.toBool();
        }
        else if (key == u"super_seeding"_s)
        {
            d.superSeeding = v.toBool();
        }
        else if (key == u"seq_dl"_s)
        {
            d.sequentialDownload = v.toBool();
        }
        else if (key == u"f_l_piece_prio"_s)
        {
            d.firstLastPiecePrio = v.toBool();
        }
        else if (key == u"force_start"_s)
        {
            d.forceStart = v.toBool();
        }
        else if (key == u"priority"_s)
        {
            d.queuePosition = parseQueuePosition(v.toInt());
        }
        else if (key == u"infohash_v1"_s || key == u"infohash_v2"_s)
        {
            // Handled separately in fromVariantMap; ignored in mergeUpdate
        }
        else if (key == u"has_tracker_warning"_s)
        {
            d.hasTrackerWarning = v.toBool();
        }
        else if (key == u"has_tracker_error"_s)
        {
            d.hasTrackerError = v.toBool();
        }
        else if (key == u"has_other_announce_error"_s)
        {
            d.hasOtherAnnounceError = v.toBool();
        }
        // Fields intentionally ignored (magnet_uri, hash):
        // hash is the map key, already set as id; magnet_uri is not needed for display.
    }
}

namespace BitTorrent
{
    TorrentData TorrentData::fromVariantMap(const QString &hashStr, const QVariantMap &map)
    {
        TorrentData d;
        d.id = TorrentID::fromString(hashStr);

        // Build InfoHash from v1/v2 strings
        const QString v1str = map.value(u"infohash_v1"_s).toString();
        const QString v2str = map.value(u"infohash_v2"_s).toString();
#ifdef QBT_USES_LIBTORRENT2
        {
            const SHA1Hash v1hash = v1str.isEmpty() ? SHA1Hash{} : SHA1Hash::fromString(v1str);
            const SHA256Hash v2hash = v2str.isEmpty() ? SHA256Hash{} : SHA256Hash::fromString(v2str);
            if (v1hash.isValid() || v2hash.isValid())
                d.infoHash = InfoHash(v1hash, v2hash);
        }
#else
        {
            const QString sha1str = v1str.isEmpty() ? hashStr : v1str;
            const SHA1Hash sha1hash = SHA1Hash::fromString(sha1str);
            if (sha1hash.isValid())
                d.infoHash = InfoHash(sha1hash);
        }
#endif

        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            applyField(d, it.key(), it.value());

        // Synthesize announce status from flags
        d.announceStatus = parseAnnounceStatus(
            d.hasTrackerWarning, d.hasTrackerError, d.hasOtherAnnounceError);

        return d;
    }

    void TorrentData::mergeUpdate(const QVariantMap &delta)
    {
        for (auto it = delta.constBegin(); it != delta.constEnd(); ++it)
            applyField(*this, it.key(), it.value());

        // Re-synthesize announce status in case flags changed
        announceStatus = parseAnnounceStatus(
            hasTrackerWarning, hasTrackerError, hasOtherAnnounceError);
    }
}
