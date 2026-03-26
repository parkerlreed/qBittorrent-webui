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

#include "remotesession.h"

#include <chrono>
#include <functional>

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include "base/global.h"
#include "base/net/apiclient.h"
#include "base/net/portforwarder.h"
#include "base/path.h"
#include "base/tag.h"
#include "base/utils/string.h"
#include "addtorrentparams.h"
#include "categoryoptions.h"
#include "downloadpathoption.h"
#include "remotetorrent.h"
#include "sharelimits.h"
#include "torrentcontentlayout.h"
#include "torrentcontentremoveoption.h"
#include "torrentdescriptor.h"

namespace
{
    // Minimal PortForwarder that delegates isEnabled/setEnabled to a RemoteSession's
    // cached preferences.  setPorts/removePorts are intentional no-ops — the remote
    // server handles actual UPnP mapping.
    class RemotePortForwarder final : public Net::PortForwarder
    {
    public:
        explicit RemotePortForwarder(QObject *parent = nullptr)
            : Net::PortForwarder(parent) {}

        bool isEnabled() const override { return m_enabled; }

        void setEnabled(bool enabled) override
        {
            if (m_enabled == enabled)
                return;
            m_enabled = enabled;
            if (m_onSet)
                m_onSet(enabled);
        }

        void setPorts(const QString &, QSet<quint16>) override {}
        void removePorts(const QString &) override {}

        void init(bool enabled, std::function<void(bool)> onSet)
        {
            m_enabled = enabled;
            m_onSet = std::move(onSet);
        }

    private:
        bool m_enabled = false;
        std::function<void(bool)> m_onSet;
    };
}

namespace BitTorrent
{
    namespace
    {
        // Milliseconds between maindata polls
        constexpr int POLL_INTERVAL_MS = 1500;
    }

    // ----- static factory -----

    void RemoteSession::initInstance(const QUrl &baseUrl,
                                     const QString &username,
                                     const QString &password)
    {
        Q_ASSERT(!instance());
        auto *s = new RemoteSession(baseUrl, username, password);
        Session::setInstance(s);
        s->m_client->login(username, password);
    }

    // ----- constructor / destructor -----

    RemoteSession::RemoteSession(const QUrl &baseUrl, const QString &username,
                                 const QString &password)
        : Session()
        , m_client(new Net::ApiClient(baseUrl, this))
        , m_pollTimer(new QTimer(this))
        , m_portForwarder(new RemotePortForwarder(this))
    {
        Q_UNUSED(username)
        Q_UNUSED(password)

        connect(m_client, &Net::ApiClient::loggedIn, this, &RemoteSession::onLoggedIn);

        m_pollTimer->setInterval(POLL_INTERVAL_MS);
        m_pollTimer->setSingleShot(false);
        connect(m_pollTimer, &QTimer::timeout, this, &RemoteSession::poll);
    }

    RemoteSession::~RemoteSession()
    {
        m_pollTimer->stop();
        qDeleteAll(m_torrents);
    }

    // ----- apiClient accessor -----

    Net::ApiClient *RemoteSession::apiClient() const
    {
        return m_client;
    }

    void RemoteSession::emitTorrentMetadataReceived(Torrent *torrent)
    {
        emit torrentMetadataReceived(torrent);
    }

    void RemoteSession::emitTorrentsUpdated(Torrent *torrent)
    {
        emit torrentsUpdated({torrent});
    }

    void RemoteSession::emitTrackersAdded(Torrent *torrent, const QList<TrackerEntry> &trackers)
    {
        emit trackersAdded(torrent, trackers);
    }

    QVariant RemoteSession::prefValue(const QString &key, const QVariant &defaultValue) const
    {
        return m_prefs.value(key, defaultValue);
    }

    void RemoteSession::setPrefValue(const QString &key, const QVariant &value)
    {
        setPref(key, value);
    }

    // ----- private slots -----

    void RemoteSession::onLoggedIn()
    {
        // Fetch preferences, then start polling
        m_client->appPreferences().then(this, [this](const QVariantMap &prefs)
        {
            onPreferencesFetched(prefs);
        });
    }

    void RemoteSession::onPreferencesFetched(const QVariantMap &prefs)
    {
        m_prefs = prefs;

        // Wire up RemotePortForwarder now that we have cached preferences.
        static_cast<RemotePortForwarder *>(m_portForwarder)->init(
            m_prefs.value(u"upnp"_s, false).toBool(),
            [this](bool enabled) { setPref(u"upnp"_s, enabled); });

        m_restored = true;
        emit restored();
        m_pollTimer->start();
        // Kick off the first poll immediately
        poll();
    }

    void RemoteSession::poll()
    {
        m_client->syncMaindata(m_rid).then(this, [this](const QVariantMap &data)
        {
            onMaindataReceived(data);
        });
    }

    void RemoteSession::onMaindataReceived(const QVariantMap &data)
    {
        m_rid = data.value(u"rid"_s).toInt();

        const bool fullUpdate = data.value(u"full_update"_s).toBool();

        // Apply category data
        if (data.contains(u"categories"_s))
        {
            applyCategoryData(data[u"categories"_s].toMap(),
                              data.value(u"categories_removed"_s).toStringList());
        }

        // Apply tag data
        if (data.contains(u"tags"_s))
        {
            applyTagData(data[u"tags"_s].toList(),
                         data.value(u"tags_removed"_s).toStringList());
        }

        // Apply torrent data
        if (data.contains(u"torrents"_s))
        {
            const QVariantMap torrentsMap = data[u"torrents"_s].toMap();
            const QStringList removed = data.value(u"torrents_removed"_s).toStringList();

            if (fullUpdate)
                applyFullSnapshot(torrentsMap);
            else
                applyTorrentDeltas(torrentsMap, removed);
        }

        // Apply server state
        if (data.contains(u"server_state"_s))
            applyServerState(data[u"server_state"_s].toMap());
    }

    void RemoteSession::applyFullSnapshot(const QVariantMap &torrents)
    {
        // Remove torrents that are no longer present
        QList<TorrentID> toRemove;
        for (auto it = m_torrents.constBegin(); it != m_torrents.constEnd(); ++it)
        {
            if (!torrents.contains(it.key().toString()))
                toRemove.append(it.key());
        }
        for (const TorrentID &id : asConst(toRemove))
        {
            RemoteTorrent *t = m_torrents.take(id);
            emit torrentAboutToBeRemoved(t);
            delete t;
        }

        QList<Torrent *> added;
        QList<Torrent *> updated;

        for (auto it = torrents.constBegin(); it != torrents.constEnd(); ++it)
        {
            const QString hashStr = it.key();
            const QVariantMap map = it.value().toMap();
            const TorrentID id = TorrentID::fromString(hashStr);

            if (m_torrents.contains(id))
            {
                m_torrents[id]->applyUpdate(map);
                updated.append(m_torrents[id]);
            }
            else
            {
                TorrentData data = TorrentData::fromVariantMap(hashStr, map);
                auto *t = new RemoteTorrent(this, data, m_client, this);
                m_torrents.insert(id, t);
                added.append(t);
            }
        }

        if (!added.isEmpty())
            emit torrentsLoaded(added);
        if (!updated.isEmpty())
            emit torrentsUpdated(updated);
    }

    void RemoteSession::applyTorrentDeltas(const QVariantMap &torrents, const QStringList &removed)
    {
        // Handle removals
        for (const QString &hashStr : removed)
        {
            const TorrentID id = TorrentID::fromString(hashStr);
            RemoteTorrent *t = m_torrents.take(id);
            if (t)
            {
                emit torrentAboutToBeRemoved(t);
                delete t;
            }
        }

        QList<Torrent *> added;
        QList<Torrent *> updated;

        for (auto it = torrents.constBegin(); it != torrents.constEnd(); ++it)
        {
            const QString hashStr = it.key();
            const QVariantMap map = it.value().toMap();
            const TorrentID id = TorrentID::fromString(hashStr);

            if (m_torrents.contains(id))
            {
                m_torrents[id]->applyUpdate(map);
                updated.append(m_torrents[id]);
            }
            else
            {
                TorrentData data = TorrentData::fromVariantMap(hashStr, map);
                auto *t = new RemoteTorrent(this, data, m_client, this);
                m_torrents.insert(id, t);
                added.append(t);
            }
        }

        if (!added.isEmpty())
            emit torrentsLoaded(added);
        if (!updated.isEmpty())
            emit torrentsUpdated(updated);
    }

    void RemoteSession::applyServerState(const QVariantMap &serverState)
    {
        if (serverState.contains(u"dl_info_speed"_s))
            m_status.payloadDownloadRate = serverState[u"dl_info_speed"_s].toLongLong();
        if (serverState.contains(u"up_info_speed"_s))
            m_status.payloadUploadRate = serverState[u"up_info_speed"_s].toLongLong();
        if (serverState.contains(u"dl_info_data"_s))
            m_status.totalPayloadDownload = serverState[u"dl_info_data"_s].toLongLong();
        if (serverState.contains(u"up_info_data"_s))
            m_status.totalPayloadUpload = serverState[u"up_info_data"_s].toLongLong();
        if (serverState.contains(u"alltime_dl"_s))
            m_status.allTimeDownload = serverState[u"alltime_dl"_s].toLongLong();
        if (serverState.contains(u"alltime_ul"_s))
            m_status.allTimeUpload = serverState[u"alltime_ul"_s].toLongLong();
        if (serverState.contains(u"total_wasted_session"_s))
            m_status.totalWasted = serverState[u"total_wasted_session"_s].toLongLong();
        if (serverState.contains(u"total_peer_connections"_s))
            m_status.peersCount = serverState[u"total_peer_connections"_s].toLongLong();
        if (serverState.contains(u"dht_nodes"_s))
            m_status.dhtNodes = serverState[u"dht_nodes"_s].toLongLong();
        if (serverState.contains(u"connection_status"_s))
        {
            const QString cs = serverState[u"connection_status"_s].toString();
            m_status.hasIncomingConnections = (cs == u"connected"_s);
        }
        if (serverState.contains(u"last_external_address_v4"_s))
            m_lastExternalIPv4 = serverState[u"last_external_address_v4"_s].toString();
        if (serverState.contains(u"last_external_address_v6"_s))
            m_lastExternalIPv6 = serverState[u"last_external_address_v6"_s].toString();
        if (serverState.contains(u"free_space_on_disk"_s))
            m_freeDiskSpace = serverState[u"free_space_on_disk"_s].toLongLong();

        emit statsUpdated();
    }

    void RemoteSession::applyCategoryData(const QVariantMap &categories, const QStringList &removed)
    {
        for (const QString &name : removed)
        {
            m_categories.remove(name);
            emit categoryRemoved(name);
        }

        for (auto it = categories.constBegin(); it != categories.constEnd(); ++it)
        {
            const QString name = it.key();
            const QVariantMap opts = it.value().toMap();
            const bool isNew = !m_categories.contains(name);
            m_categories[name] = opts;
            if (isNew)
                emit categoryAdded(name);
            else
                emit categoryOptionsChanged(name);
        }
    }

    void RemoteSession::applyTagData(const QVariantList &tags, const QStringList &removed)
    {
        for (const QString &name : removed)
        {
            m_tags.remove(Tag(name));
            emit tagRemoved(Tag(name));
        }

        for (const QVariant &v : tags)
        {
            const Tag tag(v.toString());
            if (!m_tags.contains(tag))
            {
                m_tags.insert(tag);
                emit tagAdded(tag);
            }
        }
    }

    void RemoteSession::setPref(const QString &key, const QVariant &value)
    {
        m_prefs[key] = value;
        m_client->setAppPreferences({{key, value}});
    }

    QStringList RemoteSession::torrentIDsToStrings(const QList<TorrentID> &ids)
    {
        QStringList result;
        result.reserve(ids.size());
        for (const TorrentID &id : ids)
            result.append(id.toString());
        return result;
    }

    // ----- Paths -----

    Path RemoteSession::savePath() const
    {
        return Path(pref<QString>(u"save_path"_s));
    }

    void RemoteSession::setSavePath(const Path &path)
    {
        setPref(u"save_path"_s, path.toString());
    }

    Path RemoteSession::downloadPath() const
    {
        return Path(pref<QString>(u"temp_path"_s));
    }

    void RemoteSession::setDownloadPath(const Path &path)
    {
        setPref(u"temp_path"_s, path.toString());
    }

    bool RemoteSession::isDownloadPathEnabled() const
    {
        return pref<bool>(u"temp_path_enabled"_s);
    }

    void RemoteSession::setDownloadPathEnabled(bool enabled)
    {
        setPref(u"temp_path_enabled"_s, enabled);
    }

    // ----- Categories -----

    QStringList RemoteSession::categories() const
    {
        return QStringList(m_categories.keys());
    }

    CategoryOptions RemoteSession::categoryOptions(const QString &categoryName) const
    {
        const QVariantMap opts = m_categories.value(categoryName);
        CategoryOptions result;
        result.savePath = Path(opts.value(u"savePath"_s).toString());
        const QVariant dlPath = opts.value(u"downloadPath"_s);
        if (dlPath.isValid() && !dlPath.isNull())
        {
            const QVariantMap dlPathMap = dlPath.toMap();
            DownloadPathOption dpOpt;
            dpOpt.enabled = dlPathMap.value(u"enabled"_s).toBool();
            dpOpt.path = Path(dlPathMap.value(u"path"_s).toString());
            result.downloadPath = dpOpt;
        }
        return result;
    }

    Path RemoteSession::categorySavePath(const QString &categoryName) const
    {
        return categoryOptions(categoryName).savePath;
    }

    Path RemoteSession::categorySavePath(const QString &categoryName, const CategoryOptions &options) const
    {
        Q_UNUSED(options)
        return categorySavePath(categoryName);
    }

    Path RemoteSession::categoryDownloadPath(const QString &categoryName) const
    {
        const auto opts = categoryOptions(categoryName);
        if (opts.downloadPath && opts.downloadPath->enabled)
            return opts.downloadPath->path;
        return {};
    }

    Path RemoteSession::categoryDownloadPath(const QString &categoryName, const CategoryOptions &options) const
    {
        Q_UNUSED(options)
        return categoryDownloadPath(categoryName);
    }

    qreal RemoteSession::categoryRatioLimit(const QString &categoryName) const
    {
        Q_UNUSED(categoryName)
        return DEFAULT_RATIO_LIMIT;
    }

    int RemoteSession::categorySeedingTimeLimit(const QString &categoryName) const
    {
        Q_UNUSED(categoryName)
        return DEFAULT_SEEDING_TIME_LIMIT;
    }

    int RemoteSession::categoryInactiveSeedingTimeLimit(const QString &categoryName) const
    {
        Q_UNUSED(categoryName)
        return DEFAULT_SEEDING_TIME_LIMIT;
    }

    ShareLimitAction RemoteSession::categoryShareLimitAction(const QString &categoryName) const
    {
        Q_UNUSED(categoryName)
        return ShareLimitAction::Default;
    }

    bool RemoteSession::addCategory(const QString &name, const CategoryOptions &options)
    {
        if (m_categories.contains(name))
            return false;
        m_client->createCategory(name, options.savePath.toString());
        // Optimistically update local state; maindata will confirm
        m_categories[name] = {};
        emit categoryAdded(name);
        return true;
    }

    bool RemoteSession::setCategoryOptions(const QString &name, const CategoryOptions &options)
    {
        if (!m_categories.contains(name))
            return false;
        m_client->editCategory(name, options.savePath.toString());
        return true;
    }

    bool RemoteSession::removeCategory(const QString &name)
    {
        if (!m_categories.contains(name))
            return false;
        m_client->removeCategories({name});
        m_categories.remove(name);
        emit categoryRemoved(name);
        return true;
    }

    bool RemoteSession::useCategoryPathsInManualMode() const
    {
        return pref<bool>(u"use_category_paths_in_manual_mode"_s);
    }

    void RemoteSession::setUseCategoryPathsInManualMode(bool value)
    {
        setPref(u"use_category_paths_in_manual_mode"_s, value);
    }

    Path RemoteSession::suggestedSavePath(const QString &categoryName, std::optional<bool> useAutoTMM) const
    {
        Q_UNUSED(categoryName)
        Q_UNUSED(useAutoTMM)
        return savePath();
    }

    Path RemoteSession::suggestedDownloadPath(const QString &categoryName, std::optional<bool> useAutoTMM) const
    {
        Q_UNUSED(categoryName)
        Q_UNUSED(useAutoTMM)
        return downloadPath();
    }

    // ----- Tags -----

    TagSet RemoteSession::tags() const
    {
        return m_tags;
    }

    bool RemoteSession::hasTag(const Tag &tag) const
    {
        return m_tags.contains(tag);
    }

    bool RemoteSession::addTag(const Tag &tag)
    {
        if (m_tags.contains(tag))
            return false;
        m_client->createTags({tag.toString()});
        m_tags.insert(tag);
        emit tagAdded(tag);
        return true;
    }

    bool RemoteSession::removeTag(const Tag &tag)
    {
        if (!m_tags.contains(tag))
            return false;
        m_client->deleteTags({tag.toString()});
        m_tags.remove(tag);
        emit tagRemoved(tag);
        return true;
    }

    // ----- TMM -----

    bool RemoteSession::isAutoTMMDisabledByDefault() const
    {
        return !pref<bool>(u"auto_tmm_enabled"_s);
    }

    void RemoteSession::setAutoTMMDisabledByDefault(bool value)
    {
        setPref(u"auto_tmm_enabled"_s, !value);
    }

    bool RemoteSession::isDisableAutoTMMWhenCategoryChanged() const
    {
        return !pref<bool>(u"torrent_changed_tmm_enabled"_s);
    }

    void RemoteSession::setDisableAutoTMMWhenCategoryChanged(bool value)
    {
        setPref(u"torrent_changed_tmm_enabled"_s, !value);
    }

    bool RemoteSession::isDisableAutoTMMWhenDefaultSavePathChanged() const
    {
        return !pref<bool>(u"save_path_changed_tmm_enabled"_s);
    }

    void RemoteSession::setDisableAutoTMMWhenDefaultSavePathChanged(bool value)
    {
        setPref(u"save_path_changed_tmm_enabled"_s, !value);
    }

    bool RemoteSession::isDisableAutoTMMWhenCategorySavePathChanged() const
    {
        return !pref<bool>(u"category_changed_tmm_enabled"_s);
    }

    void RemoteSession::setDisableAutoTMMWhenCategorySavePathChanged(bool value)
    {
        setPref(u"category_changed_tmm_enabled"_s, !value);
    }

    // ----- Global share limits -----

    qreal RemoteSession::globalMaxRatio() const
    {
        const QVariant v = m_prefs.value(u"max_ratio"_s);
        if (!v.isValid())
            return Torrent::MAX_RATIO;
        const qreal r = v.toDouble();
        return (r < 0) ? Torrent::MAX_RATIO : r;
    }

    void RemoteSession::setGlobalMaxRatio(qreal ratio)
    {
        setPref(u"max_ratio"_s, (ratio >= Torrent::MAX_RATIO) ? -1.0 : ratio);
    }

    int RemoteSession::globalMaxSeedingMinutes() const
    {
        return pref<int>(u"max_seeding_time"_s, -1);
    }

    void RemoteSession::setGlobalMaxSeedingMinutes(int minutes)
    {
        setPref(u"max_seeding_time"_s, minutes);
    }

    int RemoteSession::globalMaxInactiveSeedingMinutes() const
    {
        return pref<int>(u"max_inactive_seeding_time"_s, -1);
    }

    void RemoteSession::setGlobalMaxInactiveSeedingMinutes(int minutes)
    {
        setPref(u"max_inactive_seeding_time"_s, minutes);
    }

    ShareLimitAction RemoteSession::shareLimitAction() const
    {
        return static_cast<ShareLimitAction>(pref<int>(u"max_ratio_act"_s, 0));
    }

    void RemoteSession::setShareLimitAction(ShareLimitAction act)
    {
        setPref(u"max_ratio_act"_s, static_cast<int>(act));
    }

    // ----- Protocol / network settings -----

    QString RemoteSession::getDHTBootstrapNodes() const
    {
        return pref<QString>(u"dht_bootstrap_nodes"_s);
    }

    void RemoteSession::setDHTBootstrapNodes(const QString &nodes)
    {
        setPref(u"dht_bootstrap_nodes"_s, nodes);
    }

    bool RemoteSession::isDHTEnabled() const
    {
        return pref<bool>(u"dht"_s, true);
    }

    void RemoteSession::setDHTEnabled(bool enabled)
    {
        setPref(u"dht"_s, enabled);
    }

    bool RemoteSession::isLSDEnabled() const
    {
        return pref<bool>(u"lsd"_s, true);
    }

    void RemoteSession::setLSDEnabled(bool enabled)
    {
        setPref(u"lsd"_s, enabled);
    }

    bool RemoteSession::isPeXEnabled() const
    {
        return pref<bool>(u"pex"_s, true);
    }

    void RemoteSession::setPeXEnabled(bool enabled)
    {
        setPref(u"pex"_s, enabled);
    }

    bool RemoteSession::isAddTorrentToQueueTop() const
    {
        return pref<bool>(u"add_to_top_of_queue"_s);
    }

    void RemoteSession::setAddTorrentToQueueTop(bool value)
    {
        setPref(u"add_to_top_of_queue"_s, value);
    }

    bool RemoteSession::isAddTorrentStopped() const
    {
        return pref<bool>(u"add_stopped_enabled"_s);
    }

    void RemoteSession::setAddTorrentStopped(bool value)
    {
        setPref(u"add_stopped_enabled"_s, value);
    }

    Torrent::StopCondition RemoteSession::torrentStopCondition() const
    {
        return Utils::String::toEnum<Torrent::StopCondition>(
            pref<QString>(u"torrent_stop_condition"_s),
            Torrent::StopCondition::None);
    }

    void RemoteSession::setTorrentStopCondition(Torrent::StopCondition stopCondition)
    {
        setPref(u"torrent_stop_condition"_s, Utils::String::fromEnum(stopCondition));
    }

    TorrentContentLayout RemoteSession::torrentContentLayout() const
    {
        return Utils::String::toEnum<TorrentContentLayout>(
            pref<QString>(u"torrent_content_layout"_s),
            TorrentContentLayout::Original);
    }

    void RemoteSession::setTorrentContentLayout(TorrentContentLayout value)
    {
        setPref(u"torrent_content_layout"_s, Utils::String::fromEnum(value));
    }

    bool RemoteSession::isTrackerEnabled() const
    {
        return pref<bool>(u"enable_embedded_tracker"_s);
    }

    void RemoteSession::setTrackerEnabled(bool enabled)
    {
        setPref(u"enable_embedded_tracker"_s, enabled);
    }

    bool RemoteSession::isAppendExtensionEnabled() const
    {
        return pref<bool>(u"incomplete_files_ext"_s);
    }

    void RemoteSession::setAppendExtensionEnabled(bool enabled)
    {
        setPref(u"incomplete_files_ext"_s, enabled);
    }

    bool RemoteSession::isUnwantedFolderEnabled() const
    {
        return pref<bool>(u"use_unwanted_folder"_s);
    }

    void RemoteSession::setUnwantedFolderEnabled(bool enabled)
    {
        setPref(u"use_unwanted_folder"_s, enabled);
    }

    int RemoteSession::refreshInterval() const
    {
        return pref<int>(u"refresh_interval"_s, 1500);
    }

    void RemoteSession::setRefreshInterval(int value)
    {
        setPref(u"refresh_interval"_s, value);
    }

    bool RemoteSession::isPreallocationEnabled() const
    {
        return pref<bool>(u"preallocate_all"_s);
    }

    void RemoteSession::setPreallocationEnabled(bool enabled)
    {
        setPref(u"preallocate_all"_s, enabled);
    }

    Path RemoteSession::torrentExportDirectory() const
    {
        return Path(pref<QString>(u"export_dir"_s));
    }

    void RemoteSession::setTorrentExportDirectory(const Path &path)
    {
        setPref(u"export_dir"_s, path.toString());
    }

    Path RemoteSession::finishedTorrentExportDirectory() const
    {
        return Path(pref<QString>(u"export_dir_fin"_s));
    }

    void RemoteSession::setFinishedTorrentExportDirectory(const Path &path)
    {
        setPref(u"export_dir_fin"_s, path.toString());
    }

    bool RemoteSession::isAddTrackersFromURLEnabled() const
    {
        return pref<bool>(u"add_trackers_from_url_enabled"_s);
    }

    void RemoteSession::setAddTrackersFromURLEnabled(bool enabled)
    {
        setPref(u"add_trackers_from_url_enabled"_s, enabled);
    }

    QString RemoteSession::additionalTrackersURL() const
    {
        return pref<QString>(u"add_trackers_url"_s);
    }

    void RemoteSession::setAdditionalTrackersURL(const QString &url)
    {
        setPref(u"add_trackers_url"_s, url);
    }

    QString RemoteSession::additionalTrackersFromURL() const
    {
        return pref<QString>(u"add_trackers_url_list"_s);
    }

    // ----- Speed limits -----

    int RemoteSession::globalDownloadSpeedLimit() const
    {
        return pref<int>(u"dl_limit"_s, 0);
    }

    void RemoteSession::setGlobalDownloadSpeedLimit(int limit)
    {
        setPref(u"dl_limit"_s, limit);
    }

    int RemoteSession::globalUploadSpeedLimit() const
    {
        return pref<int>(u"up_limit"_s, 0);
    }

    void RemoteSession::setGlobalUploadSpeedLimit(int limit)
    {
        setPref(u"up_limit"_s, limit);
    }

    int RemoteSession::altGlobalDownloadSpeedLimit() const
    {
        return pref<int>(u"alt_dl_limit"_s, 0);
    }

    void RemoteSession::setAltGlobalDownloadSpeedLimit(int limit)
    {
        setPref(u"alt_dl_limit"_s, limit);
    }

    int RemoteSession::altGlobalUploadSpeedLimit() const
    {
        return pref<int>(u"alt_up_limit"_s, 0);
    }

    void RemoteSession::setAltGlobalUploadSpeedLimit(int limit)
    {
        setPref(u"alt_up_limit"_s, limit);
    }

    int RemoteSession::downloadSpeedLimit() const
    {
        // Current effective download limit (may be alt limit if alt enabled)
        return isAltGlobalSpeedLimitEnabled() ? altGlobalDownloadSpeedLimit() : globalDownloadSpeedLimit();
    }

    void RemoteSession::setDownloadSpeedLimit(int limit)
    {
        if (isAltGlobalSpeedLimitEnabled())
            setAltGlobalDownloadSpeedLimit(limit);
        else
            setGlobalDownloadSpeedLimit(limit);
    }

    int RemoteSession::uploadSpeedLimit() const
    {
        return isAltGlobalSpeedLimitEnabled() ? altGlobalUploadSpeedLimit() : globalUploadSpeedLimit();
    }

    void RemoteSession::setUploadSpeedLimit(int limit)
    {
        if (isAltGlobalSpeedLimitEnabled())
            setAltGlobalUploadSpeedLimit(limit);
        else
            setGlobalUploadSpeedLimit(limit);
    }

    bool RemoteSession::isAltGlobalSpeedLimitEnabled() const
    {
        return pref<bool>(u"alt_speed_enabled"_s);
    }

    void RemoteSession::setAltGlobalSpeedLimitEnabled(bool enabled)
    {
        setPref(u"alt_speed_enabled"_s, enabled);
    }

    bool RemoteSession::isBandwidthSchedulerEnabled() const
    {
        return pref<bool>(u"scheduler_enabled"_s);
    }

    void RemoteSession::setBandwidthSchedulerEnabled(bool enabled)
    {
        setPref(u"scheduler_enabled"_s, enabled);
    }

    // ----- Performance / misc settings -----

    bool RemoteSession::isPerformanceWarningEnabled() const { return false; }
    void RemoteSession::setPerformanceWarningEnabled(bool) {}

    int RemoteSession::saveResumeDataInterval() const
    {
        return pref<int>(u"save_resume_data_interval"_s, 60);
    }

    void RemoteSession::setSaveResumeDataInterval(int value)
    {
        setPref(u"save_resume_data_interval"_s, value);
    }

    std::chrono::minutes RemoteSession::saveStatisticsInterval() const
    {
        return std::chrono::minutes(pref<int>(u"save_statistics_interval"_s, 60));
    }

    void RemoteSession::setSaveStatisticsInterval(std::chrono::minutes value)
    {
        setPref(u"save_statistics_interval"_s, static_cast<int>(value.count()));
    }

    int RemoteSession::shutdownTimeout() const { return 0; }
    void RemoteSession::setShutdownTimeout(int) {}

    int RemoteSession::port() const
    {
        return pref<int>(u"listen_port"_s, 6881);
    }

    void RemoteSession::setPort(int port)
    {
        setPref(u"listen_port"_s, port);
    }

    bool RemoteSession::isSSLEnabled() const
    {
        return pref<bool>(u"ssl_enabled"_s);
    }

    void RemoteSession::setSSLEnabled(bool enabled)
    {
        setPref(u"ssl_enabled"_s, enabled);
    }

    int RemoteSession::sslPort() const
    {
        return pref<int>(u"ssl_listen_port"_s, 8443);
    }

    void RemoteSession::setSSLPort(int port)
    {
        setPref(u"ssl_listen_port"_s, port);
    }

    QString RemoteSession::networkInterface() const
    {
        return pref<QString>(u"current_network_interface"_s);
    }

    void RemoteSession::setNetworkInterface(const QString &iface)
    {
        setPref(u"current_network_interface"_s, iface);
    }

    QString RemoteSession::networkInterfaceName() const
    {
        return pref<QString>(u"current_interface_name"_s);
    }

    void RemoteSession::setNetworkInterfaceName(const QString &name)
    {
        setPref(u"current_interface_name"_s, name);
    }

    QString RemoteSession::networkInterfaceAddress() const
    {
        return pref<QString>(u"current_interface_address"_s);
    }

    void RemoteSession::setNetworkInterfaceAddress(const QString &address)
    {
        setPref(u"current_interface_address"_s, address);
    }

    int RemoteSession::encryption() const
    {
        return pref<int>(u"encryption"_s, 0);
    }

    void RemoteSession::setEncryption(int state)
    {
        setPref(u"encryption"_s, state);
    }

    int RemoteSession::maxActiveCheckingTorrents() const
    {
        return pref<int>(u"max_active_checking_torrents"_s, 1);
    }

    void RemoteSession::setMaxActiveCheckingTorrents(int val)
    {
        setPref(u"max_active_checking_torrents"_s, val);
    }

    bool RemoteSession::isI2PEnabled() const
    {
        return pref<bool>(u"i2p_enabled"_s);
    }

    void RemoteSession::setI2PEnabled(bool enabled)
    {
        setPref(u"i2p_enabled"_s, enabled);
    }

    QString RemoteSession::I2PAddress() const
    {
        return pref<QString>(u"i2p_address"_s);
    }

    void RemoteSession::setI2PAddress(const QString &address)
    {
        setPref(u"i2p_address"_s, address);
    }

    int RemoteSession::I2PPort() const
    {
        return pref<int>(u"i2p_port"_s, 7656);
    }

    void RemoteSession::setI2PPort(int port)
    {
        setPref(u"i2p_port"_s, port);
    }

    bool RemoteSession::I2PMixedMode() const
    {
        return pref<bool>(u"i2p_mixed_mode"_s);
    }

    void RemoteSession::setI2PMixedMode(bool enabled)
    {
        setPref(u"i2p_mixed_mode"_s, enabled);
    }

    int RemoteSession::I2PInboundQuantity() const
    {
        return pref<int>(u"i2p_inbound_quantity"_s, 3);
    }

    void RemoteSession::setI2PInboundQuantity(int value)
    {
        setPref(u"i2p_inbound_quantity"_s, value);
    }

    int RemoteSession::I2POutboundQuantity() const
    {
        return pref<int>(u"i2p_outbound_quantity"_s, 3);
    }

    void RemoteSession::setI2POutboundQuantity(int value)
    {
        setPref(u"i2p_outbound_quantity"_s, value);
    }

    int RemoteSession::I2PInboundLength() const
    {
        return pref<int>(u"i2p_inbound_length"_s, 3);
    }

    void RemoteSession::setI2PInboundLength(int value)
    {
        setPref(u"i2p_inbound_length"_s, value);
    }

    int RemoteSession::I2POutboundLength() const
    {
        return pref<int>(u"i2p_outbound_length"_s, 3);
    }

    void RemoteSession::setI2POutboundLength(int value)
    {
        setPref(u"i2p_outbound_length"_s, value);
    }

    bool RemoteSession::isProxyPeerConnectionsEnabled() const
    {
        return pref<bool>(u"proxy_peer_connections"_s);
    }

    void RemoteSession::setProxyPeerConnectionsEnabled(bool enabled)
    {
        setPref(u"proxy_peer_connections"_s, enabled);
    }

    ChokingAlgorithm RemoteSession::chokingAlgorithm() const
    {
        return static_cast<ChokingAlgorithm>(pref<int>(u"upload_slots_behavior"_s, 0));
    }

    void RemoteSession::setChokingAlgorithm(ChokingAlgorithm mode)
    {
        setPref(u"upload_slots_behavior"_s, static_cast<int>(mode));
    }

    SeedChokingAlgorithm RemoteSession::seedChokingAlgorithm() const
    {
        return static_cast<SeedChokingAlgorithm>(pref<int>(u"upload_choking_algorithm"_s, 1));
    }

    void RemoteSession::setSeedChokingAlgorithm(SeedChokingAlgorithm mode)
    {
        setPref(u"upload_choking_algorithm"_s, static_cast<int>(mode));
    }

    bool RemoteSession::isAddTrackersEnabled() const
    {
        return pref<bool>(u"add_trackers_enabled"_s);
    }

    void RemoteSession::setAddTrackersEnabled(bool enabled)
    {
        setPref(u"add_trackers_enabled"_s, enabled);
    }

    QString RemoteSession::additionalTrackers() const
    {
        return pref<QString>(u"add_trackers"_s);
    }

    void RemoteSession::setAdditionalTrackers(const QString &trackers)
    {
        setPref(u"add_trackers"_s, trackers);
    }

    bool RemoteSession::isIPFilteringEnabled() const
    {
        return pref<bool>(u"ip_filter_enabled"_s);
    }

    void RemoteSession::setIPFilteringEnabled(bool enabled)
    {
        setPref(u"ip_filter_enabled"_s, enabled);
    }

    Path RemoteSession::IPFilterFile() const
    {
        return Path(pref<QString>(u"ip_filter_path"_s));
    }

    void RemoteSession::setIPFilterFile(const Path &path)
    {
        setPref(u"ip_filter_path"_s, path.toString());
    }

    bool RemoteSession::announceToAllTrackers() const
    {
        return pref<bool>(u"announce_to_all_trackers"_s);
    }

    void RemoteSession::setAnnounceToAllTrackers(bool val)
    {
        setPref(u"announce_to_all_trackers"_s, val);
    }

    bool RemoteSession::announceToAllTiers() const
    {
        return pref<bool>(u"announce_to_all_tiers"_s);
    }

    void RemoteSession::setAnnounceToAllTiers(bool val)
    {
        setPref(u"announce_to_all_tiers"_s, val);
    }

    int RemoteSession::peerTurnover() const
    {
        return pref<int>(u"peer_turnover"_s, 4);
    }

    void RemoteSession::setPeerTurnover(int val)
    {
        setPref(u"peer_turnover"_s, val);
    }

    int RemoteSession::peerTurnoverCutoff() const
    {
        return pref<int>(u"peer_turnover_cutoff"_s, 90);
    }

    void RemoteSession::setPeerTurnoverCutoff(int val)
    {
        setPref(u"peer_turnover_cutoff"_s, val);
    }

    int RemoteSession::peerTurnoverInterval() const
    {
        return pref<int>(u"peer_turnover_interval"_s, 300);
    }

    void RemoteSession::setPeerTurnoverInterval(int val)
    {
        setPref(u"peer_turnover_interval"_s, val);
    }

    int RemoteSession::requestQueueSize() const
    {
        return pref<int>(u"request_queue_size"_s, 500);
    }

    void RemoteSession::setRequestQueueSize(int val)
    {
        setPref(u"request_queue_size"_s, val);
    }

    int RemoteSession::asyncIOThreads() const
    {
        return pref<int>(u"async_io_threads"_s, 10);
    }

    void RemoteSession::setAsyncIOThreads(int num)
    {
        setPref(u"async_io_threads"_s, num);
    }

    int RemoteSession::hashingThreads() const
    {
        return pref<int>(u"hashing_threads"_s, 1);
    }

    void RemoteSession::setHashingThreads(int num)
    {
        setPref(u"hashing_threads"_s, num);
    }

    int RemoteSession::filePoolSize() const
    {
        return pref<int>(u"file_pool_size"_s, 500);
    }

    void RemoteSession::setFilePoolSize(int size)
    {
        setPref(u"file_pool_size"_s, size);
    }

    int RemoteSession::checkingMemUsage() const
    {
        return pref<int>(u"checking_memory_use"_s, 32);
    }

    void RemoteSession::setCheckingMemUsage(int size)
    {
        setPref(u"checking_memory_use"_s, size);
    }

    int RemoteSession::diskCacheSize() const
    {
        return pref<int>(u"disk_cache"_s, -1);
    }

    void RemoteSession::setDiskCacheSize(int size)
    {
        setPref(u"disk_cache"_s, size);
    }

    int RemoteSession::diskCacheTTL() const
    {
        return pref<int>(u"disk_cache_ttl"_s, 60);
    }

    void RemoteSession::setDiskCacheTTL(int ttl)
    {
        setPref(u"disk_cache_ttl"_s, ttl);
    }

    qint64 RemoteSession::diskQueueSize() const
    {
        return pref<qint64>(u"disk_queue_size"_s, 1024 * 1024);
    }

    void RemoteSession::setDiskQueueSize(qint64 size)
    {
        setPref(u"disk_queue_size"_s, size);
    }

    DiskIOType RemoteSession::diskIOType() const
    {
        return static_cast<DiskIOType>(pref<int>(u"disk_io_type"_s, 0));
    }

    void RemoteSession::setDiskIOType(DiskIOType type)
    {
        setPref(u"disk_io_type"_s, static_cast<int>(type));
    }

    DiskIOReadMode RemoteSession::diskIOReadMode() const
    {
        return static_cast<DiskIOReadMode>(pref<int>(u"disk_io_read_mode"_s, 0));
    }

    void RemoteSession::setDiskIOReadMode(DiskIOReadMode mode)
    {
        setPref(u"disk_io_read_mode"_s, static_cast<int>(mode));
    }

    DiskIOWriteMode RemoteSession::diskIOWriteMode() const
    {
        return static_cast<DiskIOWriteMode>(pref<int>(u"disk_io_write_mode"_s, 0));
    }

    void RemoteSession::setDiskIOWriteMode(DiskIOWriteMode mode)
    {
        setPref(u"disk_io_write_mode"_s, static_cast<int>(mode));
    }

    bool RemoteSession::isCoalesceReadWriteEnabled() const
    {
        return pref<bool>(u"enable_coalesce_read_write"_s);
    }

    void RemoteSession::setCoalesceReadWriteEnabled(bool enabled)
    {
        setPref(u"enable_coalesce_read_write"_s, enabled);
    }

    bool RemoteSession::usePieceExtentAffinity() const
    {
        return pref<bool>(u"enable_piece_extent_affinity"_s);
    }

    void RemoteSession::setPieceExtentAffinity(bool enabled)
    {
        setPref(u"enable_piece_extent_affinity"_s, enabled);
    }

    bool RemoteSession::isSuggestModeEnabled() const
    {
        return pref<bool>(u"enable_upload_suggestions"_s);
    }

    void RemoteSession::setSuggestMode(bool mode)
    {
        setPref(u"enable_upload_suggestions"_s, mode);
    }

    int RemoteSession::sendBufferWatermark() const
    {
        return pref<int>(u"send_buffer_watermark"_s, 500);
    }

    void RemoteSession::setSendBufferWatermark(int value)
    {
        setPref(u"send_buffer_watermark"_s, value);
    }

    int RemoteSession::sendBufferLowWatermark() const
    {
        return pref<int>(u"send_buffer_low_watermark"_s, 10);
    }

    void RemoteSession::setSendBufferLowWatermark(int value)
    {
        setPref(u"send_buffer_low_watermark"_s, value);
    }

    int RemoteSession::sendBufferWatermarkFactor() const
    {
        return pref<int>(u"send_buffer_watermark_factor"_s, 50);
    }

    void RemoteSession::setSendBufferWatermarkFactor(int value)
    {
        setPref(u"send_buffer_watermark_factor"_s, value);
    }

    int RemoteSession::connectionSpeed() const
    {
        return pref<int>(u"connection_speed"_s, 30);
    }

    void RemoteSession::setConnectionSpeed(int value)
    {
        setPref(u"connection_speed"_s, value);
    }

    int RemoteSession::socketSendBufferSize() const
    {
        return pref<int>(u"socket_send_buffer_size"_s, 0);
    }

    void RemoteSession::setSocketSendBufferSize(int value)
    {
        setPref(u"socket_send_buffer_size"_s, value);
    }

    int RemoteSession::socketReceiveBufferSize() const
    {
        return pref<int>(u"socket_receive_buffer_size"_s, 0);
    }

    void RemoteSession::setSocketReceiveBufferSize(int value)
    {
        setPref(u"socket_receive_buffer_size"_s, value);
    }

    int RemoteSession::socketBacklogSize() const
    {
        return pref<int>(u"socket_backlog_size"_s, 30);
    }

    void RemoteSession::setSocketBacklogSize(int value)
    {
        setPref(u"socket_backlog_size"_s, value);
    }

    bool RemoteSession::isAnonymousModeEnabled() const
    {
        return pref<bool>(u"anonymous_mode"_s);
    }

    void RemoteSession::setAnonymousModeEnabled(bool enabled)
    {
        setPref(u"anonymous_mode"_s, enabled);
    }

    bool RemoteSession::isQueueingSystemEnabled() const
    {
        return pref<bool>(u"queueing_enabled"_s, true);
    }

    void RemoteSession::setQueueingSystemEnabled(bool enabled)
    {
        setPref(u"queueing_enabled"_s, enabled);
    }

    bool RemoteSession::ignoreSlowTorrentsForQueueing() const
    {
        return pref<bool>(u"dont_count_slow_torrents"_s);
    }

    void RemoteSession::setIgnoreSlowTorrentsForQueueing(bool ignore)
    {
        setPref(u"dont_count_slow_torrents"_s, ignore);
    }

    int RemoteSession::downloadRateForSlowTorrents() const
    {
        return pref<int>(u"slow_torrent_dl_rate_threshold"_s, 2);
    }

    void RemoteSession::setDownloadRateForSlowTorrents(int rateInKibiBytes)
    {
        setPref(u"slow_torrent_dl_rate_threshold"_s, rateInKibiBytes);
    }

    int RemoteSession::uploadRateForSlowTorrents() const
    {
        return pref<int>(u"slow_torrent_ul_rate_threshold"_s, 2);
    }

    void RemoteSession::setUploadRateForSlowTorrents(int rateInKibiBytes)
    {
        setPref(u"slow_torrent_ul_rate_threshold"_s, rateInKibiBytes);
    }

    int RemoteSession::slowTorrentsInactivityTimer() const
    {
        return pref<int>(u"slow_torrent_inactive_timer"_s, 60);
    }

    void RemoteSession::setSlowTorrentsInactivityTimer(int timeInSeconds)
    {
        setPref(u"slow_torrent_inactive_timer"_s, timeInSeconds);
    }

    int RemoteSession::outgoingPortsMin() const
    {
        return pref<int>(u"outgoing_ports_min"_s, 0);
    }

    void RemoteSession::setOutgoingPortsMin(int min)
    {
        setPref(u"outgoing_ports_min"_s, min);
    }

    int RemoteSession::outgoingPortsMax() const
    {
        return pref<int>(u"outgoing_ports_max"_s, 0);
    }

    void RemoteSession::setOutgoingPortsMax(int max)
    {
        setPref(u"outgoing_ports_max"_s, max);
    }

    int RemoteSession::UPnPLeaseDuration() const
    {
        return pref<int>(u"upnp_lease_duration"_s, 0);
    }

    void RemoteSession::setUPnPLeaseDuration(int duration)
    {
        setPref(u"upnp_lease_duration"_s, duration);
    }

    int RemoteSession::peerDSCP() const
    {
        return pref<int>(u"peer_tos"_s, 4);
    }

    void RemoteSession::setPeerDSCP(int value)
    {
        setPref(u"peer_tos"_s, value);
    }

    bool RemoteSession::ignoreLimitsOnLAN() const
    {
        return !pref<bool>(u"limit_lan_peers"_s, true);
    }

    void RemoteSession::setIgnoreLimitsOnLAN(bool ignore)
    {
        setPref(u"limit_lan_peers"_s, !ignore);
    }

    bool RemoteSession::includeOverheadInLimits() const
    {
        return pref<bool>(u"limit_tcp_overhead"_s);
    }

    void RemoteSession::setIncludeOverheadInLimits(bool include)
    {
        setPref(u"limit_tcp_overhead"_s, include);
    }

    QString RemoteSession::announceIP() const
    {
        return pref<QString>(u"announce_ip"_s);
    }

    void RemoteSession::setAnnounceIP(const QString &ip)
    {
        setPref(u"announce_ip"_s, ip);
    }

    int RemoteSession::announcePort() const
    {
        return pref<int>(u"announce_port"_s, 0);
    }

    void RemoteSession::setAnnouncePort(int port)
    {
        setPref(u"announce_port"_s, port);
    }

    int RemoteSession::maxConcurrentHTTPAnnounces() const
    {
        return pref<int>(u"max_concurrent_http_announces"_s, 50);
    }

    void RemoteSession::setMaxConcurrentHTTPAnnounces(int value)
    {
        setPref(u"max_concurrent_http_announces"_s, value);
    }

    bool RemoteSession::isReannounceWhenAddressChangedEnabled() const
    {
        return pref<bool>(u"reannounce_when_address_changed"_s);
    }

    void RemoteSession::setReannounceWhenAddressChangedEnabled(bool enabled)
    {
        setPref(u"reannounce_when_address_changed"_s, enabled);
    }

    void RemoteSession::reannounceToAllTrackers() const
    {
        // No direct API endpoint; no-op
    }

    int RemoteSession::stopTrackerTimeout() const
    {
        return pref<int>(u"stop_tracker_timeout"_s, 5);
    }

    void RemoteSession::setStopTrackerTimeout(int value)
    {
        setPref(u"stop_tracker_timeout"_s, value);
    }

    int RemoteSession::maxConnections() const
    {
        return pref<int>(u"max_connec"_s, -1);
    }

    void RemoteSession::setMaxConnections(int max)
    {
        setPref(u"max_connec"_s, max);
    }

    int RemoteSession::maxConnectionsPerTorrent() const
    {
        return pref<int>(u"max_connec_per_torrent"_s, -1);
    }

    void RemoteSession::setMaxConnectionsPerTorrent(int max)
    {
        setPref(u"max_connec_per_torrent"_s, max);
    }

    int RemoteSession::maxUploads() const
    {
        return pref<int>(u"max_uploads"_s, -1);
    }

    void RemoteSession::setMaxUploads(int max)
    {
        setPref(u"max_uploads"_s, max);
    }

    int RemoteSession::maxUploadsPerTorrent() const
    {
        return pref<int>(u"max_uploads_per_torrent"_s, -1);
    }

    void RemoteSession::setMaxUploadsPerTorrent(int max)
    {
        setPref(u"max_uploads_per_torrent"_s, max);
    }

    int RemoteSession::maxActiveDownloads() const
    {
        return pref<int>(u"max_active_downloads"_s, 3);
    }

    void RemoteSession::setMaxActiveDownloads(int max)
    {
        setPref(u"max_active_downloads"_s, max);
    }

    int RemoteSession::maxActiveUploads() const
    {
        return pref<int>(u"max_active_uploads"_s, 3);
    }

    void RemoteSession::setMaxActiveUploads(int max)
    {
        setPref(u"max_active_uploads"_s, max);
    }

    int RemoteSession::maxActiveTorrents() const
    {
        return pref<int>(u"max_active_torrents"_s, 5);
    }

    void RemoteSession::setMaxActiveTorrents(int max)
    {
        setPref(u"max_active_torrents"_s, max);
    }

    BTProtocol RemoteSession::btProtocol() const
    {
        return static_cast<BTProtocol>(pref<int>(u"bittorrent_protocol"_s, 0));
    }

    void RemoteSession::setBTProtocol(BTProtocol protocol)
    {
        setPref(u"bittorrent_protocol"_s, static_cast<int>(protocol));
    }

    bool RemoteSession::isUTPRateLimited() const
    {
        return pref<bool>(u"limit_utp_rate"_s, true);
    }

    void RemoteSession::setUTPRateLimited(bool limited)
    {
        setPref(u"limit_utp_rate"_s, limited);
    }

    MixedModeAlgorithm RemoteSession::utpMixedMode() const
    {
        return static_cast<MixedModeAlgorithm>(pref<int>(u"utp_tcp_mixed_mode"_s, 0));
    }

    void RemoteSession::setUtpMixedMode(MixedModeAlgorithm mode)
    {
        setPref(u"utp_tcp_mixed_mode"_s, static_cast<int>(mode));
    }

    int RemoteSession::hostnameCacheTTL() const
    {
        return pref<int>(u"hostname_cache_ttl"_s, 300);
    }

    void RemoteSession::setHostnameCacheTTL(int value)
    {
        setPref(u"hostname_cache_ttl"_s, value);
    }

    bool RemoteSession::isIDNSupportEnabled() const
    {
        return pref<bool>(u"idn_support_enabled"_s);
    }

    void RemoteSession::setIDNSupportEnabled(bool enabled)
    {
        setPref(u"idn_support_enabled"_s, enabled);
    }

    bool RemoteSession::multiConnectionsPerIpEnabled() const
    {
        return pref<bool>(u"enable_multi_connections_from_same_ip"_s);
    }

    void RemoteSession::setMultiConnectionsPerIpEnabled(bool enabled)
    {
        setPref(u"enable_multi_connections_from_same_ip"_s, enabled);
    }

    bool RemoteSession::validateHTTPSTrackerCertificate() const
    {
        return pref<bool>(u"validate_https_tracker_certificate"_s, true);
    }

    void RemoteSession::setValidateHTTPSTrackerCertificate(bool enabled)
    {
        setPref(u"validate_https_tracker_certificate"_s, enabled);
    }

    bool RemoteSession::isSSRFMitigationEnabled() const
    {
        return pref<bool>(u"ssrf_mitigation"_s, true);
    }

    void RemoteSession::setSSRFMitigationEnabled(bool enabled)
    {
        setPref(u"ssrf_mitigation"_s, enabled);
    }

    bool RemoteSession::blockPeersOnPrivilegedPorts() const
    {
        return pref<bool>(u"block_peers_on_privileged_ports"_s);
    }

    void RemoteSession::setBlockPeersOnPrivilegedPorts(bool enabled)
    {
        setPref(u"block_peers_on_privileged_ports"_s, enabled);
    }

    bool RemoteSession::isTrackerFilteringEnabled() const
    {
        return pref<bool>(u"ip_filter_trackers"_s);
    }

    void RemoteSession::setTrackerFilteringEnabled(bool enabled)
    {
        setPref(u"ip_filter_trackers"_s, enabled);
    }

    bool RemoteSession::isExcludedFileNamesEnabled() const
    {
        return pref<bool>(u"excluded_file_names_enabled"_s);
    }

    void RemoteSession::setExcludedFileNamesEnabled(bool enabled)
    {
        setPref(u"excluded_file_names_enabled"_s, enabled);
    }

    QStringList RemoteSession::excludedFileNames() const
    {
        return pref<QString>(u"excluded_file_names"_s).split(u'\n', Qt::SkipEmptyParts);
    }

    void RemoteSession::setExcludedFileNames(const QStringList &newList)
    {
        setPref(u"excluded_file_names"_s, newList.join(u'\n'));
    }

    void RemoteSession::applyFilenameFilter(const PathList &files, QList<BitTorrent::DownloadPriority> &priorities)
    {
        Q_UNUSED(files)
        Q_UNUSED(priorities)
        // Not meaningful for remote session
    }

    QStringList RemoteSession::bannedIPs() const
    {
        return pref<QString>(u"banned_IPs"_s).split(u'\n', Qt::SkipEmptyParts);
    }

    void RemoteSession::setBannedIPs(const QStringList &newList)
    {
        setPref(u"banned_IPs"_s, newList.join(u'\n'));
    }

    ResumeDataStorageType RemoteSession::resumeDataStorageType() const
    {
        return Utils::String::toEnum<ResumeDataStorageType>(
            pref<QString>(u"resume_data_storage_type"_s),
            ResumeDataStorageType::Legacy);
    }

    void RemoteSession::setResumeDataStorageType(ResumeDataStorageType type)
    {
        setPref(u"resume_data_storage_type"_s, Utils::String::fromEnum(type));
    }

    bool RemoteSession::isMergeTrackersEnabled() const
    {
        return pref<bool>(u"merge_trackers"_s);
    }

    void RemoteSession::setMergeTrackersEnabled(bool enabled)
    {
        setPref(u"merge_trackers"_s, enabled);
    }

    bool RemoteSession::isStartPaused() const
    {
        return m_paused;
    }

    void RemoteSession::setStartPaused(bool value)
    {
        Q_UNUSED(value)
        // Not applicable for remote session
    }

    TorrentContentRemoveOption RemoteSession::torrentContentRemoveOption() const
    {
        return Utils::String::toEnum<TorrentContentRemoveOption>(
            pref<QString>(u"torrent_content_remove_option"_s),
            TorrentContentRemoveOption::MoveToTrash);
    }

    void RemoteSession::setTorrentContentRemoveOption(TorrentContentRemoveOption option)
    {
        setPref(u"torrent_content_remove_option"_s, Utils::String::fromEnum(option));
    }

    // ----- Session state -----

    bool RemoteSession::isRestored() const
    {
        return m_restored;
    }

    bool RemoteSession::isPaused() const
    {
        return m_paused;
    }

    void RemoteSession::pause()
    {
        m_paused = true;
        m_client->appPause();
        emit paused();
    }

    void RemoteSession::resume()
    {
        m_paused = false;
        m_client->appResume();
        emit resumed();
    }

    // ----- Torrent access -----

    Torrent *RemoteSession::getTorrent(const TorrentID &id) const
    {
        return m_torrents.value(id, nullptr);
    }

    Torrent *RemoteSession::findTorrent(const InfoHash &infoHash) const
    {
        for (RemoteTorrent *t : m_torrents)
        {
            if (t->infoHash() == infoHash)
                return t;
        }
        return nullptr;
    }

    QList<Torrent *> RemoteSession::torrents() const
    {
        QList<Torrent *> result;
        result.reserve(m_torrents.size());
        for (RemoteTorrent *t : m_torrents)
            result.append(t);
        return result;
    }

    qsizetype RemoteSession::torrentsCount() const
    {
        return m_torrents.size();
    }

    const SessionStatus &RemoteSession::status() const
    {
        return m_status;
    }

    const CacheStatus &RemoteSession::cacheStatus() const
    {
        return m_cacheStatus;
    }

    bool RemoteSession::isListening() const
    {
        return m_status.hasIncomingConnections || m_restored;
    }

    void RemoteSession::banIP(const QString &ip)
    {
        QStringList ips = bannedIPs();
        if (!ips.contains(ip))
        {
            ips.append(ip);
            setBannedIPs(ips);
        }
    }

    bool RemoteSession::isKnownTorrent(const InfoHash &infoHash) const
    {
        return findTorrent(infoHash) != nullptr;
    }

    bool RemoteSession::addTorrent(const TorrentDescriptor &torrentDescr, const AddTorrentParams &params)
    {
        QVariantMap apiParams;
        if (!params.savePath.isEmpty())
            apiParams[u"savepath"_s] = params.savePath.toString();
        if (!params.category.isEmpty())
            apiParams[u"category"_s] = params.category;
        if (!params.tags.isEmpty())
        {
            QStringList tagList;
            for (const Tag &tag : params.tags)
                tagList.append(tag.toString());
            apiParams[u"tags"_s] = tagList.join(u',');
        }
        if (params.addStopped.has_value())
            apiParams[u"stopped"_s] = *params.addStopped;
        if (params.addForced)
            apiParams[u"forceStart"_s] = true;
        if (params.sequential)
            apiParams[u"sequentialDownload"_s] = true;
        if (params.firstLastPiecePriority)
            apiParams[u"firstLastPiecePrio"_s] = true;
        if (params.uploadLimit >= 0)
            apiParams[u"upLimit"_s] = params.uploadLimit;
        if (params.downloadLimit >= 0)
            apiParams[u"dlLimit"_s] = params.downloadLimit;

        // Try to get raw buffer from descriptor; fall back to magnet URI
        const auto buf = torrentDescr.saveToBuffer();
        if (buf)
        {
            m_client->torrentsAdd(*buf, apiParams);
        }
        else
        {
            // Magnet URI path: encode as URLs in params
            // The torrentDescr may be a magnet URI — extract the source from the info hash
            QString magnetUrl;
            magnetUrl += u"magnet:?xt=urn:btih:"_s;
            magnetUrl += torrentDescr.infoHash().toString();
            apiParams[u"urls"_s] = magnetUrl;
            m_client->torrentsAdd({}, apiParams);
        }
        return true;
    }

    bool RemoteSession::removeTorrent(const TorrentID &id, TorrentRemoveOption deleteOption)
    {
        if (!m_torrents.contains(id))
            return false;
        m_client->torrentsDelete({id.toString()},
                                 deleteOption == TorrentRemoveOption::RemoveContent);
        return true;
    }

    bool RemoteSession::downloadMetadata(const TorrentDescriptor &torrentDescr)
    {
        Q_UNUSED(torrentDescr)
        return false;
    }

    bool RemoteSession::cancelDownloadMetadata(const TorrentID &id)
    {
        Q_UNUSED(id)
        return false;
    }

    void RemoteSession::increaseTorrentsQueuePos(const QList<TorrentID> &ids)
    {
        m_client->torrentsIncreasePrio(torrentIDsToStrings(ids));
    }

    void RemoteSession::decreaseTorrentsQueuePos(const QList<TorrentID> &ids)
    {
        m_client->torrentsDecreasePrio(torrentIDsToStrings(ids));
    }

    void RemoteSession::topTorrentsQueuePos(const QList<TorrentID> &ids)
    {
        m_client->torrentsTopPrio(torrentIDsToStrings(ids));
    }

    void RemoteSession::bottomTorrentsQueuePos(const QList<TorrentID> &ids)
    {
        m_client->torrentsBottomPrio(torrentIDsToStrings(ids));
    }

    QString RemoteSession::lastExternalIPv4Address() const
    {
        return m_lastExternalIPv4;
    }

    QString RemoteSession::lastExternalIPv6Address() const
    {
        return m_lastExternalIPv6;
    }

    qint64 RemoteSession::freeDiskSpace() const
    {
        return m_freeDiskSpace;
    }
}
