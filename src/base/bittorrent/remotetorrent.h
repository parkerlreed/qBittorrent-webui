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

#include <QList>
#include <QVariant>

#include "torrent.h"
#include "torrentdata.h"

namespace Net { class ApiClient; }

namespace BitTorrent
{
    class Session;

    // Concrete implementation of Torrent backed by the WebUI API.
    // Display-mode methods read from the cached TorrentData snapshot.
    // Mutation methods issue asynchronous API calls.
    // applyUpdate() is called by RemoteSession when a maindata delta arrives.
    class RemoteTorrent final : public Torrent
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(RemoteTorrent)

    public:
        explicit RemoteTorrent(Session *session, const TorrentData &data,
                               Net::ApiClient *client, QObject *parent = nullptr);

        // Called by RemoteSession on each maindata update cycle.
        void applyUpdate(const QVariantMap &delta);

        // ----- AbstractFileStorage -----
        int filesCount() const override;
        Path filePath(int index) const override;
        qlonglong fileSize(int index) const override;
        void renameFile(int index, const Path &newPath) override;

        // ----- TorrentContentHandler -----
        bool hasMetadata() const override;
        Path actualStorageLocation() const override;
        Path actualFilePath(int fileIndex) const override;
        QList<DownloadPriority> filePriorities() const override;
        QList<qreal> filesProgress() const override;
        QFuture<QList<qreal>> fetchAvailableFileFractions() const override;
        void prioritizeFiles(const QList<DownloadPriority> &priorities) override;
        void flushCache() const override;
        void scheduleContentRefresh() override;

        // ----- Torrent: identity -----
        Session *session() const override;
        InfoHash infoHash() const override;
        TorrentID id() const;  // non-virtual in Torrent base
        QString name() const override;
        QDateTime creationDate() const override;
        QString creator() const override;
        QString comment() const override;
        void setComment(const QString &comment) override;
        bool isPrivate() const override;

        // ----- Torrent: sizes -----
        qlonglong totalSize() const override;
        qlonglong wantedSize() const override;
        qlonglong completedSize() const override;
        qlonglong pieceLength() const override;
        qlonglong wastedSize() const override;
        QString currentTracker() const override;
        int piecesCount() const override;
        int piecesHave() const override;

        // ----- Torrent: paths -----
        bool isAutoTMMEnabled() const override;
        void setAutoTMMEnabled(bool enabled) override;
        Path savePath() const override;
        void setSavePath(const Path &savePath) override;
        Path downloadPath() const override;
        void setDownloadPath(const Path &downloadPath) override;
        Path rootPath() const override;
        Path contentPath() const override;
        PathList filePaths() const override;
        PathList actualFilePaths() const override;

        // ----- Torrent: category / tags -----
        QString category() const override;
        bool belongsToCategory(const QString &category) const override;
        bool setCategory(const QString &category) override;
        TagSet tags() const override;
        bool hasTag(const Tag &tag) const override;
        bool addTag(const Tag &tag) override;
        bool removeTag(const Tag &tag) override;
        void removeAllTags() override;

        // ----- Torrent: progress / state -----
        qreal progress() const override;
        TorrentState state() const override;
        bool isFinished() const override;
        bool isStopped() const override;
        bool isQueued() const override;
        bool isForced() const override;
        bool isChecking() const override;
        bool isDownloading() const override;
        bool isMoving() const override;
        bool isUploading() const override;
        bool isCompleted() const override;
        bool isActive() const override;
        bool isInactive() const override;
        bool isErrored() const override;
        bool hasMissingFiles() const override;
        bool hasError() const override;

        // ----- Torrent: times -----
        QDateTime addedTime() const override;
        QDateTime completedTime() const override;
        QDateTime lastSeenComplete() const override;
        qlonglong activeTime() const override;
        qlonglong finishedTime() const override;
        qlonglong timeSinceUpload() const override;
        qlonglong timeSinceDownload() const override;
        qlonglong timeSinceActivity() const override;

        // ----- Torrent: share limits -----
        qreal ratioLimit() const override;
        void setRatioLimit(qreal limit) override;
        int seedingTimeLimit() const override;
        void setSeedingTimeLimit(int limit) override;
        int inactiveSeedingTimeLimit() const override;
        void setInactiveSeedingTimeLimit(int limit) override;
        ShareLimitAction shareLimitAction() const override;
        void setShareLimitAction(ShareLimitAction action) override;
        qreal effectiveRatioLimit() const override;
        int effectiveSeedingTimeLimit() const override;
        int effectiveInactiveSeedingTimeLimit() const override;
        ShareLimitAction effectiveShareLimitAction() const override;

        // ----- Torrent: torrent info -----
        TorrentInfo info() const override;
        bool isSequentialDownload() const override;
        bool hasFirstLastPiecePriority() const override;
        int queuePosition() const override;
        QList<TrackerEntryStatus> trackers() const override;
        QList<QUrl> urlSeeds() const override;
        QString error() const override;
        qlonglong totalDownload() const override;
        qlonglong totalUpload() const override;
        qlonglong eta() const override;
        int seedsCount() const override;
        int peersCount() const override;
        int leechsCount() const override;
        int totalSeedsCount() const override;
        int totalPeersCount() const override;
        int totalLeechersCount() const override;
        int downloadLimit() const override;
        int uploadLimit() const override;
        bool superSeeding() const override;
        bool isDHTDisabled() const override;
        bool isPEXDisabled() const override;
        bool isLSDDisabled() const override;
        QBitArray pieces() const override;
        qreal distributedCopies() const override;
        qreal realRatio() const override;
        qreal popularity() const override;
        int uploadPayloadRate() const override;
        int downloadPayloadRate() const override;
        qlonglong totalPayloadUpload() const override;
        qlonglong totalPayloadDownload() const override;
        int connectionsCount() const override;
        int connectionsLimit() const override;
        qlonglong nextAnnounce() const override;
        TorrentAnnounceStatus announceStatus() const override;

        // ----- Torrent: mutations -----
        void setName(const QString &name) override;
        void setSequentialDownload(bool enable) override;
        void setFirstLastPiecePriority(bool enabled) override;
        void stop() override;
        void start(TorrentOperatingMode mode = TorrentOperatingMode::AutoManaged) override;
        void forceReannounce(int index = -1) override;
        void forceDHTAnnounce() override;
        void forceRecheck() override;
        void setUploadLimit(int limit) override;
        void setDownloadLimit(int limit) override;
        void setSuperSeeding(bool enable) override;
        void setDHTDisabled(bool disable) override;
        void setPEXDisabled(bool disable) override;
        void setLSDDisabled(bool disable) override;
        void addTrackers(QList<TrackerEntry> trackers) override;
        void removeTrackers(const QStringList &trackers) override;
        void replaceTrackers(QList<TrackerEntry> trackers) override;
        void addUrlSeeds(const QList<QUrl> &urlSeeds) override;
        void removeUrlSeeds(const QList<QUrl> &urlSeeds) override;
        bool connectPeer(const PeerAddress &peerAddress) override;
        void clearPeers() override;
        void setMetadata(const TorrentInfo &torrentInfo) override;

        StopCondition stopCondition() const override;
        void setStopCondition(StopCondition stopCondition) override;
        SSLParameters getSSLParameters() const override;
        void setSSLParameters(const SSLParameters &sslParams) override;

        QString createMagnetURI() const override;
        nonstd::expected<QByteArray, QString> exportToBuffer() const override;
        nonstd::expected<void, QString> exportToFile(const Path &path) const override;

        // ----- Torrent: async reads -----
        QFuture<QList<PeerInfo>> fetchPeerInfo() const override;
        QFuture<QList<QUrl>> fetchURLSeeds() const override;
        QFuture<QList<int>> fetchPieceAvailability() const override;
        QFuture<QBitArray> fetchDownloadingPieces() const override;

    private:
        Session *m_session;
        Net::ApiClient *m_client;
        TorrentData m_data;

        // Cached from /api/v2/torrents/files — refreshed when metadata arrives.
        QVariantList m_fileList;
        bool m_fileListRefreshPending = false;
        QString m_magnetUri;

        QString hashStr() const;
        void refreshFileList();
        void applyFileList(const QVariantList &files);
        // Fetches pieces_num/piece_size/pieces_have (and other fields missing from
        // v5.1.0 maindata) from /api/v2/torrents/properties, then triggers a UI refresh.
        void fetchPieceInfo();

        // Helpers for file list access
        QVariantMap fileEntry(int index) const;
    };
}
