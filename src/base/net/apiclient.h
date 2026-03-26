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

#include <QFuture>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

class QByteArray;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace Net
{
    // HTTP client for the qBittorrent WebUI API (/api/v2/).
    // All request methods return QFuture<T> and are non-blocking.
    // Login is asynchronous: connect to loggedIn() / loginFailed() before calling login().
    class ApiClient : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(ApiClient)

    public:
        explicit ApiClient(const QUrl &baseUrl, QObject *parent = nullptr);

        QUrl baseUrl() const;

        // Authentication
        void login(const QString &username, const QString &password);
        void logout();
        bool isLoggedIn() const;

        // Sync
        QFuture<QVariantMap> syncMaindata(int rid = 0);
        QFuture<QVariantMap> syncTorrentPeers(const QString &hash, int rid = 0);

        // App / preferences
        QFuture<QVariantMap> appPreferences();
        QFuture<void> setAppPreferences(const QVariantMap &prefs);
        QFuture<QString> appDefaultSavePath();
        QFuture<void> appPause();
        QFuture<void> appResume();

        // Torrent list/info
        QFuture<QVariantList> torrentsInfo(const QStringList &hashes = {});
        QFuture<QVariantMap>  torrentsProperties(const QString &hash);
        QFuture<QVariantList> torrentsTrackers(const QString &hash);
        QFuture<QVariantList> torrentsWebSeeds(const QString &hash);
        QFuture<QVariantList> torrentsFiles(const QString &hash);
        QFuture<QVariantList> torrentsPieceStates(const QString &hash);
        QFuture<QVariantList> torrentsPieceHashes(const QString &hash);
        QFuture<QVariantMap>  torrentsPeers(const QString &hash, int rid = 0);

        // Torrent actions (hashes is "|"-separated or "all")
        QFuture<void> torrentsStart(const QStringList &hashes);
        QFuture<void> torrentsStop(const QStringList &hashes);
        QFuture<void> torrentsDelete(const QStringList &hashes, bool deleteFiles);
        QFuture<void> torrentsRecheck(const QStringList &hashes);
        QFuture<void> torrentsReannounce(const QStringList &hashes);
        QFuture<void> torrentsSetCategory(const QStringList &hashes, const QString &category);
        QFuture<void> torrentsAddTags(const QStringList &hashes, const QString &tags);
        QFuture<void> torrentsRemoveTags(const QStringList &hashes, const QString &tags);
        QFuture<void> torrentsSetTags(const QStringList &hashes, const QString &tags);
        QFuture<void> torrentsRename(const QString &hash, const QString &name);
        QFuture<void> torrentsSetUploadLimit(const QStringList &hashes, int limit);
        QFuture<void> torrentsSetDownloadLimit(const QStringList &hashes, int limit);
        QFuture<void> torrentsSetShareLimits(const QStringList &hashes, qreal ratioLimit,
                                              int seedingTimeLimit, int inactiveSeedingTimeLimit);
        QFuture<void> torrentsSetSavePath(const QStringList &hashes, const QString &path);
        QFuture<void> torrentsSetDownloadPath(const QStringList &hashes, const QString &path);
        QFuture<void> torrentsSetForceStart(const QStringList &hashes, bool force);
        QFuture<void> torrentsSetSuperSeeding(const QStringList &hashes, bool enabled);
        QFuture<void> torrentsSetAutoManagement(const QStringList &hashes, bool enabled);
        QFuture<void> torrentsSetSequentialDownload(const QStringList &hashes); // toggle
        QFuture<void> torrentsSetFirstLastPiecePrio(const QStringList &hashes); // toggle
        QFuture<void> torrentsIncreasePrio(const QStringList &hashes);
        QFuture<void> torrentsDecreasePrio(const QStringList &hashes);
        QFuture<void> torrentsTopPrio(const QStringList &hashes);
        QFuture<void> torrentsBottomPrio(const QStringList &hashes);
        // Sets `priority` for the given file indices on torrent `hash`.
        // Corresponds to POST /api/v2/torrents/filePrio with hash, id (pipe-separated), priority.
        QFuture<void> torrentsSetFilePrio(const QString &hash, const QList<int> &fileIds, int priority);
        QFuture<void> torrentsAdd(const QByteArray &torrentData, const QVariantMap &params);
        QFuture<void> torrentsAddTrackers(const QString &hash, const QString &urlsNewlineSeparated);
        QFuture<void> torrentsEditTracker(const QString &hash, const QString &origUrl,
                                          const QString &newUrl);
        QFuture<void> torrentsRemoveTrackers(const QString &hash, const QStringList &urls);
        QFuture<void> torrentsAddWebSeeds(const QString &hash, const QStringList &urls);
        QFuture<void> torrentsRemoveWebSeeds(const QString &hash, const QStringList &urls);
        QFuture<void> torrentsRenameFile(const QString &hash, const QString &oldPath,
                                         const QString &newPath);
        QFuture<void> torrentsRenameFolder(const QString &hash, const QString &oldPath,
                                           const QString &newPath);
        QFuture<void> torrentsSetSSLParameters(const QString &hash, const QVariantMap &sslParams);
        QFuture<void> torrentsSetComment(const QString &hash, const QString &comment);

        // Search
        QFuture<QVariantMap> searchStart(const QString &pattern, const QString &category, const QString &plugins);
        QFuture<QVariantMap> searchResults(int id, int offset = 0);
        QFuture<void> searchStop(int id);
        QFuture<QVariantList> searchPlugins();

        // Categories / tags
        QFuture<QVariantMap> categories();
        QFuture<void> createCategory(const QString &name, const QString &savePath,
                                     const QString &downloadPath = {});
        QFuture<void> editCategory(const QString &name, const QString &savePath,
                                   const QString &downloadPath = {});
        QFuture<void> removeCategories(const QStringList &names);
        QFuture<QStringList> tags();
        QFuture<void> createTags(const QStringList &tagNames);
        QFuture<void> deleteTags(const QStringList &tagNames);

    signals:
        void loggedIn();
        void loginFailed(const QString &reason);
        void requestError(const QString &endpoint, const QString &reason);

    private:
        QNetworkAccessManager *m_nam;
        QUrl m_baseUrl;
        QString m_sid;
        bool m_loggedIn = false;

        QNetworkRequest makeRequest(const QString &apiPath) const;
        QFuture<QByteArray> getRaw(const QString &apiPath, const QUrlQuery &query = {});
        QFuture<QByteArray> postRaw(const QString &apiPath, const QByteArray &body,
                                    const QString &contentType);
        QFuture<QByteArray> postForm(const QString &apiPath, const QUrlQuery &form);
        static QString joinHashes(const QStringList &hashes);
    };
}
