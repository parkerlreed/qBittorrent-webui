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

#include <QHash>
#include <QUrl>
#include <QVariant>

#include "cachestatus.h"
#include "session.h"
#include "sessionstatus.h"
#include "trackerentry.h"

class QTimer;
namespace Net { class ApiClient; class PortForwarder; }

namespace BitTorrent
{
    class RemoteTorrent;

    // Session implementation backed by the WebUI HTTP API.
    // Polls /api/v2/sync/maindata on a timer, maintains RemoteTorrent objects,
    // and emits the same signals as SessionImpl.
    // Settings are backed by a cached preferences map from /api/v2/app/preferences.
    class RemoteSession final : public Session
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(RemoteSession)

    public:
        // Creates and registers the RemoteSession as the Session singleton.
        // login() is called automatically. On success, the `restored()` signal is emitted.
        static void initInstance(const QUrl &baseUrl,
                                 const QString &username,
                                 const QString &password);

        ~RemoteSession() override;

        Net::ApiClient *apiClient() const;

        // Read/write a raw preference value from the cached prefs map.
        // Used by the Options dialog to access app-level settings (e.g. file logger)
        // that live in /api/v2/app/preferences but not in the Session interface.
        QVariant prefValue(const QString &key, const QVariant &defaultValue = {}) const;
        void setPrefValue(const QString &key, const QVariant &value);

        // Called by RemoteTorrent after backfilling piece info from the properties API.
        void emitTorrentMetadataReceived(Torrent *torrent);
        // Called by RemoteTorrent after its file list is refreshed, so the UI re-reads file priorities/progress.
        void emitTorrentsUpdated(Torrent *torrent);
        // Called by RemoteTorrent after its tracker list is fetched, so the sidebar filter populates.
        void emitTrackersAdded(Torrent *torrent, const QList<TrackerEntry> &trackers);

        // ----- Paths -----
        Path savePath() const override;
        void setSavePath(const Path &path) override;
        Path downloadPath() const override;
        void setDownloadPath(const Path &path) override;
        bool isDownloadPathEnabled() const override;
        void setDownloadPathEnabled(bool enabled) override;

        // ----- Categories -----
        QStringList categories() const override;
        CategoryOptions categoryOptions(const QString &categoryName) const override;
        Path categorySavePath(const QString &categoryName) const override;
        Path categorySavePath(const QString &categoryName, const CategoryOptions &options) const override;
        Path categoryDownloadPath(const QString &categoryName) const override;
        Path categoryDownloadPath(const QString &categoryName, const CategoryOptions &options) const override;
        qreal categoryRatioLimit(const QString &categoryName) const override;
        int categorySeedingTimeLimit(const QString &categoryName) const override;
        int categoryInactiveSeedingTimeLimit(const QString &categoryName) const override;
        ShareLimitAction categoryShareLimitAction(const QString &categoryName) const override;
        bool addCategory(const QString &name, const CategoryOptions &options = {}) override;
        bool setCategoryOptions(const QString &name, const CategoryOptions &options) override;
        bool removeCategory(const QString &name) override;
        bool useCategoryPathsInManualMode() const override;
        void setUseCategoryPathsInManualMode(bool value) override;
        Path suggestedSavePath(const QString &categoryName, std::optional<bool> useAutoTMM) const override;
        Path suggestedDownloadPath(const QString &categoryName, std::optional<bool> useAutoTMM) const override;

        // ----- Tags -----
        TagSet tags() const override;
        bool hasTag(const Tag &tag) const override;
        bool addTag(const Tag &tag) override;
        bool removeTag(const Tag &tag) override;

        // ----- TMM -----
        bool isAutoTMMDisabledByDefault() const override;
        void setAutoTMMDisabledByDefault(bool value) override;
        bool isDisableAutoTMMWhenCategoryChanged() const override;
        void setDisableAutoTMMWhenCategoryChanged(bool value) override;
        bool isDisableAutoTMMWhenDefaultSavePathChanged() const override;
        void setDisableAutoTMMWhenDefaultSavePathChanged(bool value) override;
        bool isDisableAutoTMMWhenCategorySavePathChanged() const override;
        void setDisableAutoTMMWhenCategorySavePathChanged(bool value) override;

        // ----- Global share limits -----
        qreal globalMaxRatio() const override;
        void setGlobalMaxRatio(qreal ratio) override;
        int globalMaxSeedingMinutes() const override;
        void setGlobalMaxSeedingMinutes(int minutes) override;
        int globalMaxInactiveSeedingMinutes() const override;
        void setGlobalMaxInactiveSeedingMinutes(int minutes) override;
        ShareLimitAction shareLimitAction() const override;
        void setShareLimitAction(ShareLimitAction act) override;

        // ----- Protocol / network settings -----
        QString getDHTBootstrapNodes() const override;
        void setDHTBootstrapNodes(const QString &nodes) override;
        bool isDHTEnabled() const override;
        void setDHTEnabled(bool enabled) override;
        bool isLSDEnabled() const override;
        void setLSDEnabled(bool enabled) override;
        bool isPeXEnabled() const override;
        void setPeXEnabled(bool enabled) override;
        bool isAddTorrentToQueueTop() const override;
        void setAddTorrentToQueueTop(bool value) override;
        bool isAddTorrentStopped() const override;
        void setAddTorrentStopped(bool value) override;
        Torrent::StopCondition torrentStopCondition() const override;
        void setTorrentStopCondition(Torrent::StopCondition stopCondition) override;
        TorrentContentLayout torrentContentLayout() const override;
        void setTorrentContentLayout(TorrentContentLayout value) override;
        bool isTrackerEnabled() const override;
        void setTrackerEnabled(bool enabled) override;
        bool isAppendExtensionEnabled() const override;
        void setAppendExtensionEnabled(bool enabled) override;
        bool isUnwantedFolderEnabled() const override;
        void setUnwantedFolderEnabled(bool enabled) override;
        int refreshInterval() const override;
        void setRefreshInterval(int value) override;
        bool isPreallocationEnabled() const override;
        void setPreallocationEnabled(bool enabled) override;
        Path torrentExportDirectory() const override;
        void setTorrentExportDirectory(const Path &path) override;
        Path finishedTorrentExportDirectory() const override;
        void setFinishedTorrentExportDirectory(const Path &path) override;
        bool isAddTrackersFromURLEnabled() const override;
        void setAddTrackersFromURLEnabled(bool enabled) override;
        QString additionalTrackersURL() const override;
        void setAdditionalTrackersURL(const QString &url) override;
        QString additionalTrackersFromURL() const override;

        // ----- Speed limits -----
        int globalDownloadSpeedLimit() const override;
        void setGlobalDownloadSpeedLimit(int limit) override;
        int globalUploadSpeedLimit() const override;
        void setGlobalUploadSpeedLimit(int limit) override;
        int altGlobalDownloadSpeedLimit() const override;
        void setAltGlobalDownloadSpeedLimit(int limit) override;
        int altGlobalUploadSpeedLimit() const override;
        void setAltGlobalUploadSpeedLimit(int limit) override;
        int downloadSpeedLimit() const override;
        void setDownloadSpeedLimit(int limit) override;
        int uploadSpeedLimit() const override;
        void setUploadSpeedLimit(int limit) override;
        bool isAltGlobalSpeedLimitEnabled() const override;
        void setAltGlobalSpeedLimitEnabled(bool enabled) override;
        bool isBandwidthSchedulerEnabled() const override;
        void setBandwidthSchedulerEnabled(bool enabled) override;

        // ----- Performance / misc settings -----
        bool isPerformanceWarningEnabled() const override;
        void setPerformanceWarningEnabled(bool enable) override;
        int saveResumeDataInterval() const override;
        void setSaveResumeDataInterval(int value) override;
        std::chrono::minutes saveStatisticsInterval() const override;
        void setSaveStatisticsInterval(std::chrono::minutes value) override;
        int shutdownTimeout() const override;
        void setShutdownTimeout(int value) override;
        int port() const override;
        void setPort(int port) override;
        bool isSSLEnabled() const override;
        void setSSLEnabled(bool enabled) override;
        int sslPort() const override;
        void setSSLPort(int port) override;
        QString networkInterface() const override;
        void setNetworkInterface(const QString &iface) override;
        QString networkInterfaceName() const override;
        void setNetworkInterfaceName(const QString &name) override;
        QString networkInterfaceAddress() const override;
        void setNetworkInterfaceAddress(const QString &address) override;
        int encryption() const override;
        void setEncryption(int state) override;
        int maxActiveCheckingTorrents() const override;
        void setMaxActiveCheckingTorrents(int val) override;
        bool isI2PEnabled() const override;
        void setI2PEnabled(bool enabled) override;
        QString I2PAddress() const override;
        void setI2PAddress(const QString &address) override;
        int I2PPort() const override;
        void setI2PPort(int port) override;
        bool I2PMixedMode() const override;
        void setI2PMixedMode(bool enabled) override;
        int I2PInboundQuantity() const override;
        void setI2PInboundQuantity(int value) override;
        int I2POutboundQuantity() const override;
        void setI2POutboundQuantity(int value) override;
        int I2PInboundLength() const override;
        void setI2PInboundLength(int value) override;
        int I2POutboundLength() const override;
        void setI2POutboundLength(int value) override;
        bool isProxyPeerConnectionsEnabled() const override;
        void setProxyPeerConnectionsEnabled(bool enabled) override;
        ChokingAlgorithm chokingAlgorithm() const override;
        void setChokingAlgorithm(ChokingAlgorithm mode) override;
        SeedChokingAlgorithm seedChokingAlgorithm() const override;
        void setSeedChokingAlgorithm(SeedChokingAlgorithm mode) override;
        bool isAddTrackersEnabled() const override;
        void setAddTrackersEnabled(bool enabled) override;
        QString additionalTrackers() const override;
        void setAdditionalTrackers(const QString &trackers) override;
        bool isIPFilteringEnabled() const override;
        void setIPFilteringEnabled(bool enabled) override;
        Path IPFilterFile() const override;
        void setIPFilterFile(const Path &path) override;
        bool announceToAllTrackers() const override;
        void setAnnounceToAllTrackers(bool val) override;
        bool announceToAllTiers() const override;
        void setAnnounceToAllTiers(bool val) override;
        int peerTurnover() const override;
        void setPeerTurnover(int val) override;
        int peerTurnoverCutoff() const override;
        void setPeerTurnoverCutoff(int val) override;
        int peerTurnoverInterval() const override;
        void setPeerTurnoverInterval(int val) override;
        int requestQueueSize() const override;
        void setRequestQueueSize(int val) override;
        int asyncIOThreads() const override;
        void setAsyncIOThreads(int num) override;
        int hashingThreads() const override;
        void setHashingThreads(int num) override;
        int filePoolSize() const override;
        void setFilePoolSize(int size) override;
        int checkingMemUsage() const override;
        void setCheckingMemUsage(int size) override;
        int diskCacheSize() const override;
        void setDiskCacheSize(int size) override;
        int diskCacheTTL() const override;
        void setDiskCacheTTL(int ttl) override;
        qint64 diskQueueSize() const override;
        void setDiskQueueSize(qint64 size) override;
        DiskIOType diskIOType() const override;
        void setDiskIOType(DiskIOType type) override;
        DiskIOReadMode diskIOReadMode() const override;
        void setDiskIOReadMode(DiskIOReadMode mode) override;
        DiskIOWriteMode diskIOWriteMode() const override;
        void setDiskIOWriteMode(DiskIOWriteMode mode) override;
        bool isCoalesceReadWriteEnabled() const override;
        void setCoalesceReadWriteEnabled(bool enabled) override;
        bool usePieceExtentAffinity() const override;
        void setPieceExtentAffinity(bool enabled) override;
        bool isSuggestModeEnabled() const override;
        void setSuggestMode(bool mode) override;
        int sendBufferWatermark() const override;
        void setSendBufferWatermark(int value) override;
        int sendBufferLowWatermark() const override;
        void setSendBufferLowWatermark(int value) override;
        int sendBufferWatermarkFactor() const override;
        void setSendBufferWatermarkFactor(int value) override;
        int connectionSpeed() const override;
        void setConnectionSpeed(int value) override;
        int socketSendBufferSize() const override;
        void setSocketSendBufferSize(int value) override;
        int socketReceiveBufferSize() const override;
        void setSocketReceiveBufferSize(int value) override;
        int socketBacklogSize() const override;
        void setSocketBacklogSize(int value) override;
        bool isAnonymousModeEnabled() const override;
        void setAnonymousModeEnabled(bool enabled) override;
        bool isQueueingSystemEnabled() const override;
        void setQueueingSystemEnabled(bool enabled) override;
        bool ignoreSlowTorrentsForQueueing() const override;
        void setIgnoreSlowTorrentsForQueueing(bool ignore) override;
        int downloadRateForSlowTorrents() const override;
        void setDownloadRateForSlowTorrents(int rateInKibiBytes) override;
        int uploadRateForSlowTorrents() const override;
        void setUploadRateForSlowTorrents(int rateInKibiBytes) override;
        int slowTorrentsInactivityTimer() const override;
        void setSlowTorrentsInactivityTimer(int timeInSeconds) override;
        int outgoingPortsMin() const override;
        void setOutgoingPortsMin(int min) override;
        int outgoingPortsMax() const override;
        void setOutgoingPortsMax(int max) override;
        int UPnPLeaseDuration() const override;
        void setUPnPLeaseDuration(int duration) override;
        int peerDSCP() const override;
        void setPeerDSCP(int value) override;
        bool ignoreLimitsOnLAN() const override;
        void setIgnoreLimitsOnLAN(bool ignore) override;
        bool includeOverheadInLimits() const override;
        void setIncludeOverheadInLimits(bool include) override;
        QString announceIP() const override;
        void setAnnounceIP(const QString &ip) override;
        int announcePort() const override;
        void setAnnouncePort(int port) override;
        int maxConcurrentHTTPAnnounces() const override;
        void setMaxConcurrentHTTPAnnounces(int value) override;
        bool isReannounceWhenAddressChangedEnabled() const override;
        void setReannounceWhenAddressChangedEnabled(bool enabled) override;
        void reannounceToAllTrackers() const override;
        int stopTrackerTimeout() const override;
        void setStopTrackerTimeout(int value) override;
        int maxConnections() const override;
        void setMaxConnections(int max) override;
        int maxConnectionsPerTorrent() const override;
        void setMaxConnectionsPerTorrent(int max) override;
        int maxUploads() const override;
        void setMaxUploads(int max) override;
        int maxUploadsPerTorrent() const override;
        void setMaxUploadsPerTorrent(int max) override;
        int maxActiveDownloads() const override;
        void setMaxActiveDownloads(int max) override;
        int maxActiveUploads() const override;
        void setMaxActiveUploads(int max) override;
        int maxActiveTorrents() const override;
        void setMaxActiveTorrents(int max) override;
        BTProtocol btProtocol() const override;
        void setBTProtocol(BTProtocol protocol) override;
        bool isUTPRateLimited() const override;
        void setUTPRateLimited(bool limited) override;
        MixedModeAlgorithm utpMixedMode() const override;
        void setUtpMixedMode(MixedModeAlgorithm mode) override;
        int hostnameCacheTTL() const override;
        void setHostnameCacheTTL(int value) override;
        bool isIDNSupportEnabled() const override;
        void setIDNSupportEnabled(bool enabled) override;
        bool multiConnectionsPerIpEnabled() const override;
        void setMultiConnectionsPerIpEnabled(bool enabled) override;
        bool validateHTTPSTrackerCertificate() const override;
        void setValidateHTTPSTrackerCertificate(bool enabled) override;
        bool isSSRFMitigationEnabled() const override;
        void setSSRFMitigationEnabled(bool enabled) override;
        bool blockPeersOnPrivilegedPorts() const override;
        void setBlockPeersOnPrivilegedPorts(bool enabled) override;
        bool isTrackerFilteringEnabled() const override;
        void setTrackerFilteringEnabled(bool enabled) override;
        bool isExcludedFileNamesEnabled() const override;
        void setExcludedFileNamesEnabled(bool enabled) override;
        QStringList excludedFileNames() const override;
        void setExcludedFileNames(const QStringList &newList) override;
        void applyFilenameFilter(const PathList &files, QList<BitTorrent::DownloadPriority> &priorities) override;
        QStringList bannedIPs() const override;
        void setBannedIPs(const QStringList &newList) override;
        ResumeDataStorageType resumeDataStorageType() const override;
        void setResumeDataStorageType(ResumeDataStorageType type) override;
        bool isMergeTrackersEnabled() const override;
        void setMergeTrackersEnabled(bool enabled) override;
        bool isStartPaused() const override;
        void setStartPaused(bool value) override;
        TorrentContentRemoveOption torrentContentRemoveOption() const override;
        void setTorrentContentRemoveOption(TorrentContentRemoveOption option) override;

        // ----- Session state -----
        bool isRestored() const override;
        bool isPaused() const override;
        void pause() override;
        void resume() override;

        // ----- Torrent access -----
        Torrent *getTorrent(const TorrentID &id) const override;
        Torrent *findTorrent(const InfoHash &infoHash) const override;
        QList<Torrent *> torrents() const override;
        qsizetype torrentsCount() const override;
        const SessionStatus &status() const override;
        const CacheStatus &cacheStatus() const override;
        bool isListening() const override;

        void banIP(const QString &ip) override;

        bool isKnownTorrent(const InfoHash &infoHash) const override;
        bool addTorrent(const TorrentDescriptor &torrentDescr, const AddTorrentParams &params = {}) override;
        bool removeTorrent(const TorrentID &id, TorrentRemoveOption deleteOption = TorrentRemoveOption::KeepContent) override;
        bool downloadMetadata(const TorrentDescriptor &torrentDescr) override;
        bool cancelDownloadMetadata(const TorrentID &id) override;

        void increaseTorrentsQueuePos(const QList<TorrentID> &ids) override;
        void decreaseTorrentsQueuePos(const QList<TorrentID> &ids) override;
        void topTorrentsQueuePos(const QList<TorrentID> &ids) override;
        void bottomTorrentsQueuePos(const QList<TorrentID> &ids) override;

        QString lastExternalIPv4Address() const override;
        QString lastExternalIPv6Address() const override;
        qint64 freeDiskSpace() const override;

    private:
        explicit RemoteSession(const QUrl &baseUrl, const QString &username,
                               const QString &password);

        void onLoggedIn();
        void onPreferencesFetched(const QVariantMap &prefs);
        void poll();
        void onMaindataReceived(const QVariantMap &data);
        void applyFullSnapshot(const QVariantMap &torrents);
        void applyTorrentDeltas(const QVariantMap &torrents, const QStringList &removed);
        void applyServerState(const QVariantMap &serverState);
        void applyCategoryData(const QVariantMap &categories, const QStringList &removed);
        void applyTagData(const QVariantList &tags, const QStringList &removed);

        // Preferences helper: read/write with a key name and default
        template<typename T>
        T pref(const QString &key, const T &def = T{}) const
        {
            const QVariant v = m_prefs.value(key);
            if (!v.isValid())
                return def;
            return v.value<T>();
        }

        void setPref(const QString &key, const QVariant &value);

        static QStringList torrentIDsToStrings(const QList<TorrentID> &ids);

        Net::ApiClient *m_client;
        QTimer *m_pollTimer;
        Net::PortForwarder *m_portForwarder;
        int m_rid = 0;
        bool m_restored = false;
        bool m_paused = false;

        QHash<TorrentID, RemoteTorrent *> m_torrents;
        SessionStatus m_status;
        CacheStatus m_cacheStatus;
        QVariantMap m_prefs;

        // Category data from maindata
        QHash<QString, QVariantMap> m_categories;

        // Tag set from maindata
        TagSet m_tags;

        // Cached from server_state
        QString m_lastExternalIPv4;
        QString m_lastExternalIPv6;
        qint64  m_freeDiskSpace = 0;
    };
}
