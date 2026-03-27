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

#include "remotetorrent.h"

#include <QBitArray>
#include <QFuture>
#include <QList>
#include <QUrl>

#include "base/3rdparty/expected.hpp"
#include "base/global.h"
#include "base/net/apiclient.h"
#include "base/path.h"
#include "base/tag.h"
#include "peeraddress.h"
#include "peerinfo.h"
#include "remotesession.h"
#include "session.h"
#include "sharelimits.h"
#include "sslparameters.h"
#include "torrentinfo.h"
#include "trackerentry.h"
#include "trackerentrystatus.h"

namespace BitTorrent
{
    RemoteTorrent::RemoteTorrent(Session *session, const TorrentData &data,
                                 Net::ApiClient *client, QObject *parent)
        : Torrent(parent)
        , m_session(session)
        , m_client(client)
        , m_data(data)
    {
        // Always fetch the tracker list — needed for the sidebar filter regardless of metadata.
        refreshTrackerList();

        if (m_data.hasMetadata)
        {
            refreshFileList();
            // Backfill piece info if maindata didn't include it (e.g. server version < 5.2.0)
            if (m_data.piecesCount == 0)
                fetchPieceInfo();
        }
    }

    void RemoteTorrent::applyUpdate(const QVariantMap &delta)
    {
        const bool hadMetadata = m_data.hasMetadata;
        m_data.mergeUpdate(delta);

        if (delta.contains(u"magnet_uri"_s))
            m_magnetUri = delta.value(u"magnet_uri"_s).toString();

        // Re-fetch tracker list if the active tracker changed.
        if (delta.contains(u"tracker"_s) && m_trackerCacheFetched)
            refreshTrackerList();

        // Refresh file list when metadata first becomes available
        if (!hadMetadata && m_data.hasMetadata)
        {
            refreshFileList();
            fetchPieceInfo();
        }
        else if (m_data.hasMetadata && !m_fileListRefreshPending)
        {
            // Keep the file list in sync with the server on each delta update.
            // The pending flag prevents piling up requests if the server is slow.
            refreshFileList();
        }
    }

    // ----- AbstractFileStorage -----

    int RemoteTorrent::filesCount() const
    {
        return m_fileList.size();
    }

    Path RemoteTorrent::filePath(int index) const
    {
        return Path(fileEntry(index).value(u"name"_s).toString());
    }

    qlonglong RemoteTorrent::fileSize(int index) const
    {
        return fileEntry(index).value(u"size"_s).toLongLong();
    }

    void RemoteTorrent::renameFile(int index, const Path &newPath)
    {
        const Path oldPath = filePath(index);
        if (!oldPath.isEmpty())
            m_client->torrentsRenameFile(hashStr(), oldPath.toString(), newPath.toString());
    }

    // ----- TorrentContentHandler -----

    bool RemoteTorrent::hasMetadata() const { return m_data.hasMetadata; }
    Path RemoteTorrent::actualStorageLocation() const { return m_data.actualStorageLocation; }

    Path RemoteTorrent::actualFilePath(int fileIndex) const
    {
        const QString name = fileEntry(fileIndex).value(u"name"_s).toString();
        if (name.isEmpty())
            return {};
        return m_data.actualStorageLocation / Path(name);
    }

    QList<DownloadPriority> RemoteTorrent::filePriorities() const
    {
        QList<DownloadPriority> result;
        result.reserve(m_fileList.size());
        for (const QVariant &entry : m_fileList)
        {
            const int prio = entry.toMap().value(u"priority"_s).toInt();
            result.append(static_cast<DownloadPriority>(prio));
        }
        return result;
    }

    QList<qreal> RemoteTorrent::filesProgress() const
    {
        QList<qreal> result;
        result.reserve(m_fileList.size());
        for (const QVariant &entry : m_fileList)
            result.append(entry.toMap().value(u"progress"_s).toDouble());
        return result;
    }

    QFuture<QList<qreal>> RemoteTorrent::fetchAvailableFileFractions() const
    {
        return m_client->torrentsFiles(hashStr()).then(
            [](const QVariantList &files) -> QList<qreal>
            {
                QList<qreal> result;
                result.reserve(files.size());
                for (const QVariant &f : files)
                    result.append(f.toMap().value(u"availability"_s).toDouble());
                return result;
            });
    }

    void RemoteTorrent::prioritizeFiles(const QList<DownloadPriority> &priorities)
    {
        // The API endpoint filePrio takes a single priority value and a set of file
        // indices. Diff the incoming list against the cached file list to find what
        // changed, group by priority, and make one call per unique new priority.
        QHash<int, QList<int>> byPriority;  // priority value → file indices
        for (int i = 0; i < m_fileList.size() && i < priorities.size(); ++i)
        {
            const int newPrio = static_cast<int>(priorities.at(i));
            const int oldPrio = m_fileList.at(i).toMap().value(u"priority"_s).toInt();
            if (newPrio != oldPrio)
                byPriority[newPrio].append(i);
        }

        for (auto it = byPriority.constBegin(); it != byPriority.constEnd(); ++it)
            m_client->torrentsSetFilePrio(hashStr(), it.value(), it.key());

        // Optimistically update the cached file list so that the next synchronous
        // filePriorities() call (from TorrentContentModel::refresh()) returns the
        // new values instead of reverting to the old ones.
        for (int i = 0; i < m_fileList.size() && i < priorities.size(); ++i)
        {
            QVariantMap entry = m_fileList.at(i).toMap();
            entry[u"priority"_s] = static_cast<int>(priorities.at(i));
            m_fileList[i] = entry;
        }
    }

    void RemoteTorrent::flushCache() const {}  // No-op for remote

    void RemoteTorrent::scheduleContentRefresh()
    {
        if (m_data.hasMetadata && !m_fileListRefreshPending)
            refreshFileList();
    }

    // ----- Torrent: identity -----

    Session *RemoteTorrent::session() const { return m_session; }
    InfoHash RemoteTorrent::infoHash() const { return m_data.infoHash; }
    QString RemoteTorrent::name() const { return m_data.name; }
    QDateTime RemoteTorrent::creationDate() const { return m_data.creationDate; }
    QString RemoteTorrent::creator() const { return m_data.creator; }
    QString RemoteTorrent::comment() const { return m_data.comment; }
    bool RemoteTorrent::isPrivate() const { return m_data.isPrivate; }

    void RemoteTorrent::setComment(const QString &comment)
    {
        m_client->torrentsSetComment(hashStr(), comment);
    }

    // ----- Torrent: sizes -----

    qlonglong RemoteTorrent::totalSize() const { return m_data.totalSize; }
    qlonglong RemoteTorrent::wantedSize() const { return m_data.size; }
    qlonglong RemoteTorrent::completedSize() const { return m_data.completedSize; }
    qlonglong RemoteTorrent::pieceLength() const { return m_data.pieceLength; }
    qlonglong RemoteTorrent::wastedSize() const { return m_data.wastedSize; }
    QString RemoteTorrent::currentTracker() const { return m_data.currentTracker; }
    int RemoteTorrent::piecesCount() const { return m_data.piecesCount; }
    int RemoteTorrent::piecesHave() const { return m_data.piecesHave; }

    // ----- Torrent: paths -----

    bool RemoteTorrent::isAutoTMMEnabled() const { return m_data.autoTMM; }
    Path RemoteTorrent::savePath() const { return m_data.savePath; }
    Path RemoteTorrent::downloadPath() const { return m_data.downloadPath; }
    Path RemoteTorrent::rootPath() const { return m_data.rootPath; }
    Path RemoteTorrent::contentPath() const { return m_data.contentPath; }

    void RemoteTorrent::setAutoTMMEnabled(bool enabled)
    {
        m_client->torrentsSetAutoManagement({hashStr()}, enabled);
    }

    void RemoteTorrent::setSavePath(const Path &savePath)
    {
        m_client->torrentsSetSavePath({hashStr()}, savePath.toString());
    }

    void RemoteTorrent::setDownloadPath(const Path &downloadPath)
    {
        m_client->torrentsSetDownloadPath({hashStr()}, downloadPath.toString());
    }

    PathList RemoteTorrent::filePaths() const
    {
        PathList result;
        result.reserve(m_fileList.size());
        for (const QVariant &entry : m_fileList)
            result.append(Path(entry.toMap().value(u"name"_s).toString()));
        return result;
    }

    PathList RemoteTorrent::actualFilePaths() const
    {
        PathList result;
        const Path base = m_data.actualStorageLocation;
        result.reserve(m_fileList.size());
        for (const QVariant &entry : m_fileList)
        {
            const QString name = entry.toMap().value(u"name"_s).toString();
            result.append(name.isEmpty() ? Path{} : base / Path(name));
        }
        return result;
    }

    // ----- Torrent: category / tags -----

    QString RemoteTorrent::category() const { return m_data.category; }

    bool RemoteTorrent::belongsToCategory(const QString &cat) const
    {
        if (m_data.category == cat)
            return true;
        // Also match sub-categories: "cat/sub" belongs to "cat"
        return m_data.category.startsWith(cat + u'/');
    }

    bool RemoteTorrent::setCategory(const QString &cat)
    {
        m_client->torrentsSetCategory({hashStr()}, cat);
        return true;
    }

    TagSet RemoteTorrent::tags() const { return m_data.tags; }

    bool RemoteTorrent::hasTag(const Tag &tag) const
    {
        return m_data.tags.contains(tag);
    }

    bool RemoteTorrent::addTag(const Tag &tag)
    {
        m_client->torrentsAddTags({hashStr()}, tag.toString());
        return true;
    }

    bool RemoteTorrent::removeTag(const Tag &tag)
    {
        m_client->torrentsRemoveTags({hashStr()}, tag.toString());
        return true;
    }

    void RemoteTorrent::removeAllTags()
    {
        QStringList tagStrings;
        tagStrings.reserve(m_data.tags.size());
        for (const Tag &t : m_data.tags)
            tagStrings.append(t.toString());
        m_client->torrentsRemoveTags({hashStr()}, tagStrings.join(u','));
    }

    // ----- Torrent: progress / state -----

    qreal RemoteTorrent::progress() const { return m_data.progress; }
    TorrentState RemoteTorrent::state() const { return m_data.state; }

    bool RemoteTorrent::isFinished() const
    {
        return m_data.state == TorrentState::StoppedUploading
            || m_data.state == TorrentState::Uploading
            || m_data.state == TorrentState::StalledUploading
            || m_data.state == TorrentState::ForcedUploading
            || m_data.state == TorrentState::QueuedUploading
            || m_data.state == TorrentState::CheckingUploading;
    }

    bool RemoteTorrent::isStopped() const
    {
        return m_data.state == TorrentState::StoppedDownloading
            || m_data.state == TorrentState::StoppedUploading;
    }

    bool RemoteTorrent::isQueued() const
    {
        return m_data.state == TorrentState::QueuedDownloading
            || m_data.state == TorrentState::QueuedUploading;
    }

    bool RemoteTorrent::isForced() const
    {
        return m_data.state == TorrentState::ForcedDownloading
            || m_data.state == TorrentState::ForcedDownloadingMetadata
            || m_data.state == TorrentState::ForcedUploading;
    }

    bool RemoteTorrent::isChecking() const
    {
        return m_data.state == TorrentState::CheckingDownloading
            || m_data.state == TorrentState::CheckingUploading
            || m_data.state == TorrentState::CheckingResumeData;
    }

    bool RemoteTorrent::isDownloading() const
    {
        return m_data.state == TorrentState::Downloading
            || m_data.state == TorrentState::DownloadingMetadata
            || m_data.state == TorrentState::ForcedDownloading
            || m_data.state == TorrentState::ForcedDownloadingMetadata
            || m_data.state == TorrentState::StalledDownloading
            || m_data.state == TorrentState::QueuedDownloading
            || m_data.state == TorrentState::CheckingDownloading;
    }

    bool RemoteTorrent::isMoving() const
    {
        return m_data.state == TorrentState::Moving;
    }

    bool RemoteTorrent::isUploading() const
    {
        return m_data.state == TorrentState::Uploading
            || m_data.state == TorrentState::ForcedUploading
            || m_data.state == TorrentState::StalledUploading
            || m_data.state == TorrentState::QueuedUploading
            || m_data.state == TorrentState::CheckingUploading;
    }

    bool RemoteTorrent::isCompleted() const { return isFinished(); }

    bool RemoteTorrent::isActive() const
    {
        return !isStopped() && !isQueued() && !isMoving()
            && m_data.state != TorrentState::CheckingResumeData;
    }

    bool RemoteTorrent::isInactive() const { return !isActive(); }

    bool RemoteTorrent::isErrored() const
    {
        return m_data.state == TorrentState::Error
            || m_data.state == TorrentState::MissingFiles;
    }

    bool RemoteTorrent::hasMissingFiles() const
    {
        return m_data.state == TorrentState::MissingFiles;
    }

    bool RemoteTorrent::hasError() const
    {
        return m_data.state == TorrentState::Error || !m_data.error.isEmpty();
    }

    // ----- Torrent: times -----

    QDateTime RemoteTorrent::addedTime() const { return m_data.addedTime; }
    QDateTime RemoteTorrent::completedTime() const { return m_data.completedTime; }
    QDateTime RemoteTorrent::lastSeenComplete() const { return m_data.lastSeenComplete; }
    qlonglong RemoteTorrent::activeTime() const { return m_data.activeTime; }
    qlonglong RemoteTorrent::finishedTime() const { return m_data.finishedTime; }
    qlonglong RemoteTorrent::timeSinceUpload() const { return m_data.timeSinceUpload; }
    qlonglong RemoteTorrent::timeSinceDownload() const { return m_data.timeSinceDownload; }
    qlonglong RemoteTorrent::timeSinceActivity() const { return m_data.timeSinceActivity; }

    // ----- Torrent: share limits -----

    qreal RemoteTorrent::ratioLimit() const { return m_data.ratioLimit; }
    int RemoteTorrent::seedingTimeLimit() const { return m_data.seedingTimeLimit; }
    int RemoteTorrent::inactiveSeedingTimeLimit() const { return m_data.inactiveSeedingTimeLimit; }
    ShareLimitAction RemoteTorrent::shareLimitAction() const { return m_data.shareLimitAction; }

    qreal RemoteTorrent::effectiveRatioLimit() const
    {
        if (m_data.ratioLimit == DEFAULT_RATIO_LIMIT)
            return m_session->globalMaxRatio();
        if (m_data.ratioLimit == NO_RATIO_LIMIT)
            return NO_RATIO_LIMIT;
        return m_data.ratioLimit;
    }

    int RemoteTorrent::effectiveSeedingTimeLimit() const
    {
        if (m_data.seedingTimeLimit == DEFAULT_SEEDING_TIME_LIMIT)
            return m_session->globalMaxSeedingMinutes();
        return m_data.seedingTimeLimit;
    }

    int RemoteTorrent::effectiveInactiveSeedingTimeLimit() const
    {
        if (m_data.inactiveSeedingTimeLimit == DEFAULT_SEEDING_TIME_LIMIT)
            return m_session->globalMaxInactiveSeedingMinutes();
        return m_data.inactiveSeedingTimeLimit;
    }

    ShareLimitAction RemoteTorrent::effectiveShareLimitAction() const
    {
        if (m_data.shareLimitAction == ShareLimitAction::Default)
            return m_session->shareLimitAction();
        return m_data.shareLimitAction;
    }

    void RemoteTorrent::setRatioLimit(qreal limit)
    {
        m_client->torrentsSetShareLimits({hashStr()}, limit,
            m_data.seedingTimeLimit, m_data.inactiveSeedingTimeLimit);
    }

    void RemoteTorrent::setSeedingTimeLimit(int limit)
    {
        m_client->torrentsSetShareLimits({hashStr()}, m_data.ratioLimit,
            limit, m_data.inactiveSeedingTimeLimit);
    }

    void RemoteTorrent::setInactiveSeedingTimeLimit(int limit)
    {
        m_client->torrentsSetShareLimits({hashStr()}, m_data.ratioLimit,
            m_data.seedingTimeLimit, limit);
    }

    void RemoteTorrent::setShareLimitAction(ShareLimitAction action)
    {
        // share_limit_action is set via the general preferences API, not per-torrent
        // For now this is a no-op; it would require a dedicated API endpoint.
        Q_UNUSED(action)
    }

    // ----- Torrent: torrent info -----

    TorrentInfo RemoteTorrent::info() const { return {}; }  // Cannot reconstruct from API

    bool RemoteTorrent::isSequentialDownload() const { return m_data.sequentialDownload; }
    bool RemoteTorrent::hasFirstLastPiecePriority() const { return m_data.firstLastPiecePrio; }
    int RemoteTorrent::queuePosition() const { return m_data.queuePosition; }

    QList<TrackerEntryStatus> RemoteTorrent::trackers() const
    {
        return m_trackerCache;
    }

    QList<QUrl> RemoteTorrent::urlSeeds() const
    {
        // Not in maindata; returned lazily via fetchURLSeeds().
        return {};
    }

    QString RemoteTorrent::error() const { return m_data.error; }
    qlonglong RemoteTorrent::totalDownload() const { return m_data.totalDownload; }
    qlonglong RemoteTorrent::totalUpload() const { return m_data.totalUpload; }
    qlonglong RemoteTorrent::eta() const { return m_data.eta; }
    int RemoteTorrent::seedsCount() const { return m_data.seedsCount; }
    int RemoteTorrent::peersCount() const { return m_data.leechsCount; }
    int RemoteTorrent::leechsCount() const { return m_data.leechsCount; }
    int RemoteTorrent::totalSeedsCount() const { return m_data.totalSeedsCount; }
    int RemoteTorrent::totalPeersCount() const { return m_data.totalLeechersCount; }
    int RemoteTorrent::totalLeechersCount() const { return m_data.totalLeechersCount; }
    int RemoteTorrent::downloadLimit() const { return m_data.downloadLimit; }
    int RemoteTorrent::uploadLimit() const { return m_data.uploadLimit; }
    bool RemoteTorrent::superSeeding() const { return m_data.superSeeding; }
    bool RemoteTorrent::isDHTDisabled() const { return m_data.isDHTDisabled; }
    bool RemoteTorrent::isPEXDisabled() const { return m_data.isPEXDisabled; }
    bool RemoteTorrent::isLSDDisabled() const { return m_data.isLSDDisabled; }
    QBitArray RemoteTorrent::pieces() const { return {}; }
    qreal RemoteTorrent::distributedCopies() const { return m_data.distributedCopies; }
    qreal RemoteTorrent::realRatio() const { return m_data.ratio; }
    qreal RemoteTorrent::popularity() const { return m_data.popularity; }
    int RemoteTorrent::uploadPayloadRate() const { return m_data.uploadPayloadRate; }
    int RemoteTorrent::downloadPayloadRate() const { return m_data.downloadPayloadRate; }
    qlonglong RemoteTorrent::totalPayloadUpload() const { return m_data.totalUploadSession; }
    qlonglong RemoteTorrent::totalPayloadDownload() const { return m_data.totalDownloadSession; }
    int RemoteTorrent::connectionsCount() const { return m_data.connectionsCount; }
    int RemoteTorrent::connectionsLimit() const { return m_data.connectionsLimit; }
    qlonglong RemoteTorrent::nextAnnounce() const { return m_data.nextAnnounce; }
    TorrentAnnounceStatus RemoteTorrent::announceStatus() const { return m_data.announceStatus; }

    // ----- Torrent: mutations -----

    void RemoteTorrent::setName(const QString &name)
    {
        m_client->torrentsRename(hashStr(), name);
    }

    void RemoteTorrent::setSequentialDownload(bool enable)
    {
        if (enable != m_data.sequentialDownload)
            m_client->torrentsSetSequentialDownload({hashStr()});
    }

    void RemoteTorrent::setFirstLastPiecePriority(bool enabled)
    {
        if (enabled != m_data.firstLastPiecePrio)
            m_client->torrentsSetFirstLastPiecePrio({hashStr()});
    }

    void RemoteTorrent::stop()
    {
        m_client->torrentsStop({hashStr()});
    }

    void RemoteTorrent::start(TorrentOperatingMode mode)
    {
        if (mode == TorrentOperatingMode::Forced)
            m_client->torrentsSetForceStart({hashStr()}, true);
        else
            m_client->torrentsStart({hashStr()});
    }

    void RemoteTorrent::forceReannounce(int /*index*/)
    {
        m_client->torrentsReannounce({hashStr()});
    }

    void RemoteTorrent::forceDHTAnnounce() {}  // Not exposed via API

    void RemoteTorrent::forceRecheck()
    {
        m_client->torrentsRecheck({hashStr()});
    }

    void RemoteTorrent::setUploadLimit(int limit)
    {
        m_client->torrentsSetUploadLimit({hashStr()}, limit);
    }

    void RemoteTorrent::setDownloadLimit(int limit)
    {
        m_client->torrentsSetDownloadLimit({hashStr()}, limit);
    }

    void RemoteTorrent::setSuperSeeding(bool enable)
    {
        m_client->torrentsSetSuperSeeding({hashStr()}, enable);
    }

    void RemoteTorrent::setDHTDisabled(bool /*disable*/) {}   // Not exposed via API
    void RemoteTorrent::setPEXDisabled(bool /*disable*/) {}   // Not exposed via API
    void RemoteTorrent::setLSDDisabled(bool /*disable*/) {}   // Not exposed via API

    void RemoteTorrent::addTrackers(QList<TrackerEntry> trackers)
    {
        QStringList urls;
        urls.reserve(trackers.size());
        for (const TrackerEntry &t : trackers)
            urls.append(t.url);
        m_client->torrentsAddTrackers(hashStr(), urls.join(u'\n'));
    }

    void RemoteTorrent::removeTrackers(const QStringList &trackers)
    {
        m_client->torrentsRemoveTrackers(hashStr(), trackers);
    }

    void RemoteTorrent::replaceTrackers(QList<TrackerEntry> trackers)
    {
        // The API doesn't have a direct "replace all trackers" endpoint.
        // As a reasonable approximation: remove all existing, then add the new ones.
        // This is done by setting trackers via addTrackers after clearing.
        // For now just add; the remote instance manages its own tracker list.
        addTrackers(std::move(trackers));
    }

    void RemoteTorrent::addUrlSeeds(const QList<QUrl> &urlSeeds)
    {
        QStringList urls;
        urls.reserve(urlSeeds.size());
        for (const QUrl &u : urlSeeds)
            urls.append(u.toString());
        m_client->torrentsAddWebSeeds(hashStr(), urls);
    }

    void RemoteTorrent::removeUrlSeeds(const QList<QUrl> &urlSeeds)
    {
        QStringList urls;
        urls.reserve(urlSeeds.size());
        for (const QUrl &u : urlSeeds)
            urls.append(u.toString());
        m_client->torrentsRemoveWebSeeds(hashStr(), urls);
    }

    bool RemoteTorrent::connectPeer(const PeerAddress &peerAddress)
    {
        // The API endpoint is /api/v2/torrents/addPeers but it is not in ApiClient yet.
        // Stub: return false (peer not added).
        Q_UNUSED(peerAddress)
        return false;
    }

    void RemoteTorrent::clearPeers() {}  // Not exposed via API

    void RemoteTorrent::setMetadata(const TorrentInfo &) {}  // Not applicable for remote

    RemoteTorrent::StopCondition RemoteTorrent::stopCondition() const
    {
        return StopCondition::None;
    }

    void RemoteTorrent::setStopCondition(StopCondition) {}

    SSLParameters RemoteTorrent::getSSLParameters() const
    {
        return {};
    }

    void RemoteTorrent::setSSLParameters(const SSLParameters &sslParams)
    {
        QVariantMap params;
        params[u"certificate"_s] = QString::fromLatin1(
            sslParams.certificate.toPem());
        params[u"private_key"_s] = QString::fromLatin1(
            sslParams.privateKey.toPem());
        m_client->torrentsSetSSLParameters(hashStr(), params);
    }

    QString RemoteTorrent::createMagnetURI() const
    {
        return m_magnetUri;
    }

    nonstd::expected<QByteArray, QString> RemoteTorrent::exportToBuffer() const
    {
        return nonstd::make_unexpected(tr("Export not available for remote torrents."));
    }

    nonstd::expected<void, QString> RemoteTorrent::exportToFile(const Path &) const
    {
        return nonstd::make_unexpected(tr("Export not available for remote torrents."));
    }

    // ----- Async reads -----

    QFuture<QList<PeerInfo>> RemoteTorrent::fetchPeerInfo() const
    {
        return m_client->torrentsPeers(hashStr()).then(
            [](const QVariantMap &data) -> QList<PeerInfo>
            {
                const QVariantMap peers = data.value(u"peers"_s).toMap();
                QList<PeerInfo> result;
                result.reserve(peers.size());
                for (const QVariant &peerVar : peers)
                    result.append(PeerInfo(peerVar.toMap()));
                return result;
            });
    }

    QFuture<QList<QUrl>> RemoteTorrent::fetchURLSeeds() const
    {
        return m_client->torrentsWebSeeds(hashStr()).then(
            [](const QVariantList &seeds) -> QList<QUrl>
            {
                QList<QUrl> result;
                result.reserve(seeds.size());
                for (const QVariant &s : seeds)
                    result.append(QUrl(s.toMap().value(u"url"_s).toString()));
                return result;
            });
    }

    QFuture<QList<int>> RemoteTorrent::fetchPieceAvailability() const
    {
        return m_client->torrentsPieceStates(hashStr()).then(
            [](const QVariantList &states) -> QList<int>
            {
                // API returns piece states as integers (0=not downloaded, 1=downloading, 2=downloaded)
                // GUI expects availability ints; repurpose as direct state values.
                QList<int> result;
                result.reserve(states.size());
                for (const QVariant &s : states)
                    result.append(s.toInt());
                return result;
            });
    }

    QFuture<QBitArray> RemoteTorrent::fetchDownloadingPieces() const
    {
        return m_client->torrentsPieceStates(hashStr()).then(
            [](const QVariantList &states) -> QBitArray
            {
                QBitArray bits(states.size(), false);
                for (int i = 0; i < states.size(); ++i)
                {
                    // State 1 = downloading (partial)
                    if (states.at(i).toInt() == 1)
                        bits.setBit(i, true);
                }
                return bits;
            });
    }

    // ----- Private helpers -----

    QString RemoteTorrent::hashStr() const
    {
        return m_data.id.toString();
    }

    void RemoteTorrent::refreshFileList()
    {
        m_fileListRefreshPending = true;
        m_client->torrentsFiles(hashStr()).then(this,
            [this](const QVariantList &files)
            {
                m_fileListRefreshPending = false;
                applyFileList(files);
            });
    }

    void RemoteTorrent::applyFileList(const QVariantList &files)
    {
        m_fileList = files;
        // Notify the UI so TorrentContentModel re-reads file priorities/progress.
        static_cast<RemoteSession *>(m_session)->emitTorrentsUpdated(this);
    }

    void RemoteTorrent::refreshTrackerList()
    {
        m_client->torrentsTrackers(hashStr()).then(this,
            [this](const QVariantList &trackers)
            {
                applyTrackerList(trackers);
            });
    }

    void RemoteTorrent::applyTrackerList(const QVariantList &rawTrackers)
    {
        const bool firstFetch = !m_trackerCacheFetched;
        m_trackerCacheFetched = true;

        QList<TrackerEntryStatus> newCache;
        QList<TrackerEntry> addedEntries;

        for (const QVariant &v : rawTrackers)
        {
            const QVariantMap m = v.toMap();
            const QString url = m.value(u"url"_s).toString();
            // Skip synthetic entries like "** [DHT] **", "** [PeX] **", "** [LSD] **"
            if (url.startsWith(u"** ["_s))
                continue;

            TrackerEntryStatus status;
            status.url = url;
            status.tier = m.value(u"tier"_s).toInt();
            newCache.append(status);

            // On first fetch, collect entries to emit trackersAdded for the sidebar.
            if (firstFetch)
                addedEntries.append(TrackerEntry{url, status.tier});
        }

        m_trackerCache = newCache;

        auto *rs = static_cast<RemoteSession *>(m_session);
        if (firstFetch && !addedEntries.isEmpty())
            rs->emitTrackersAdded(this, addedEntries);
        else if (!firstFetch)
            rs->emitTorrentsUpdated(this);
    }

    void RemoteTorrent::fetchPieceInfo()
    {
        m_client->torrentsProperties(hashStr()).then(this, [this](const QVariantMap &props)
        {
            if (props.isEmpty())
                return;

            bool changed = false;
            const auto applyInt = [&](const QString &key, int &field)
            {
                const auto it = props.constFind(key);
                if (it != props.constEnd())
                {
                    field = it.value().toInt();
                    changed = true;
                }
            };
            const auto applyLongLong = [&](const QString &key, qlonglong &field)
            {
                const auto it = props.constFind(key);
                if (it != props.constEnd())
                {
                    field = it.value().toLongLong();
                    changed = true;
                }
            };

            applyInt(u"pieces_num"_s, m_data.piecesCount);
            applyLongLong(u"piece_size"_s, m_data.pieceLength);
            applyInt(u"pieces_have"_s, m_data.piecesHave);

            // Also backfill other fields missing from pre-5.2.0 maindata
            if (m_data.creator.isEmpty())
            {
                const auto it = props.constFind(u"created_by"_s);
                if (it != props.constEnd())
                    m_data.creator = it.value().toString();
            }
            if (!m_data.creationDate.isValid())
            {
                const auto it = props.constFind(u"creation_date"_s);
                if (it != props.constEnd())
                {
                    const qlonglong secs = it.value().toLongLong();
                    if (secs > 0)
                        m_data.creationDate = QDateTime::fromSecsSinceEpoch(secs);
                }
            }
            if (m_data.comment.isEmpty())
            {
                const auto it = props.constFind(u"comment"_s);
                if (it != props.constEnd())
                    m_data.comment = it.value().toString();
            }
            if (m_data.wastedSize == 0)
                applyLongLong(u"total_wasted"_s, m_data.wastedSize);

            if (changed)
                static_cast<RemoteSession *>(m_session)->emitTorrentMetadataReceived(this);
        });
    }

    QVariantMap RemoteTorrent::fileEntry(int index) const
    {
        if (index < 0 || index >= m_fileList.size())
            return {};
        return m_fileList.at(index).toMap();
    }
}
