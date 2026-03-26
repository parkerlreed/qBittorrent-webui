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

#include "apiclient.h"

#include <memory>
#include <stdexcept>

#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPromise>
#include <QUrlQuery>

#include "base/global.h"

namespace
{
    // Bridge a QNetworkReply to a QPromise<QByteArray>.
    // The promise and reply lifetimes are tied together via the lambda capture.
    void bridgeReplyToPromise(QNetworkReply *reply,
                              std::shared_ptr<QPromise<QByteArray>> promise)
    {
        QObject::connect(reply, &QNetworkReply::finished, reply,
            [reply, promise]() mutable
            {
                if (reply->error() == QNetworkReply::NoError)
                {
                    promise->addResult(reply->readAll());
                }
                else
                {
                    promise->setException(std::make_exception_ptr(
                        std::runtime_error(reply->errorString().toStdString())));
                }
                promise->finish();
                reply->deleteLater();
            });
    }
}

namespace Net
{
    ApiClient::ApiClient(const QUrl &baseUrl, QObject *parent)
        : QObject(parent)
        , m_nam(new QNetworkAccessManager(this))
        , m_baseUrl(baseUrl)
    {
    }

    QUrl ApiClient::baseUrl() const
    {
        return m_baseUrl;
    }

    void ApiClient::login(const QString &username, const QString &password)
    {
        QUrlQuery form;
        form.addQueryItem(u"username"_s, username);
        form.addQueryItem(u"password"_s, password);

        postForm(u"/api/v2/auth/login"_s, form).then(this,
            [this](const QByteArray &response)
            {
                const QString text = QString::fromUtf8(response).trimmed();
                if (text == u"Ok."_s)
                {
                    m_loggedIn = true;
                    emit loggedIn();
                }
                else if (text == u"Banned."_s)
                {
                    emit loginFailed(tr("IP has been banned due to too many failed login attempts."));
                }
                else
                {
                    emit loginFailed(tr("Invalid username or password."));
                }
            }).onFailed(this,
            [this](const std::exception &e)
            {
                emit loginFailed(QString::fromStdString(e.what()));
            });
    }

    void ApiClient::logout()
    {
        postForm(u"/api/v2/auth/logout"_s, {});
        m_loggedIn = false;
        m_sid.clear();
    }

    bool ApiClient::isLoggedIn() const
    {
        return m_loggedIn;
    }

    QFuture<QVariantMap> ApiClient::syncMaindata(int rid)
    {
        QUrlQuery query;
        query.addQueryItem(u"rid"_s, QString::number(rid));
        return getRaw(u"/api/v2/sync/maindata"_s, query).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<QVariantMap> ApiClient::syncTorrentPeers(const QString &hash, int rid)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        query.addQueryItem(u"rid"_s, QString::number(rid));
        return getRaw(u"/api/v2/sync/torrentPeers"_s, query).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<QVariantMap> ApiClient::appPreferences()
    {
        return getRaw(u"/api/v2/app/preferences"_s).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<void> ApiClient::setAppPreferences(const QVariantMap &prefs)
    {
        const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(prefs)).toJson(QJsonDocument::Compact);
        QUrlQuery form;
        form.addQueryItem(u"json"_s, QString::fromUtf8(json));
        return postForm(u"/api/v2/app/setPreferences"_s, form).then([](QByteArray) {});
    }

    QFuture<QString> ApiClient::appDefaultSavePath()
    {
        return getRaw(u"/api/v2/app/defaultSavePath"_s).then(
            [](const QByteArray &data) -> QString
            {
                return QString::fromUtf8(data).trimmed();
            });
    }

    QFuture<void> ApiClient::appPause()
    {
        return postForm(u"/api/v2/app/pause"_s, {}).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::appResume()
    {
        return postForm(u"/api/v2/app/resume"_s, {}).then([](QByteArray) {});
    }

    QFuture<QVariantList> ApiClient::torrentsInfo(const QStringList &hashes)
    {
        QUrlQuery query;
        if (!hashes.isEmpty())
            query.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return getRaw(u"/api/v2/torrents/info"_s, query).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    QFuture<QVariantMap> ApiClient::torrentsProperties(const QString &hash)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        return getRaw(u"/api/v2/torrents/properties"_s, query).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<QVariantList> ApiClient::torrentsTrackers(const QString &hash)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        return getRaw(u"/api/v2/torrents/trackers"_s, query).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    QFuture<QVariantList> ApiClient::torrentsWebSeeds(const QString &hash)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        return getRaw(u"/api/v2/torrents/webseeds"_s, query).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    QFuture<QVariantList> ApiClient::torrentsFiles(const QString &hash)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        return getRaw(u"/api/v2/torrents/files"_s, query).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    QFuture<QVariantList> ApiClient::torrentsPieceStates(const QString &hash)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        return getRaw(u"/api/v2/torrents/pieceStates"_s, query).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    QFuture<QVariantList> ApiClient::torrentsPieceHashes(const QString &hash)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        return getRaw(u"/api/v2/torrents/pieceHashes"_s, query).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    QFuture<QVariantMap> ApiClient::torrentsPeers(const QString &hash, int rid)
    {
        QUrlQuery query;
        query.addQueryItem(u"hash"_s, hash);
        query.addQueryItem(u"rid"_s, QString::number(rid));
        return getRaw(u"/api/v2/sync/torrentPeers"_s, query).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    // --- Torrent actions ---

    QFuture<void> ApiClient::torrentsStart(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/start"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsStop(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/stop"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsDelete(const QStringList &hashes, bool deleteFiles)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"deleteFiles"_s, deleteFiles ? u"true"_s : u"false"_s);
        return postForm(u"/api/v2/torrents/delete"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRecheck(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/recheck"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsReannounce(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/reannounce"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetCategory(const QStringList &hashes, const QString &category)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"category"_s, category);
        return postForm(u"/api/v2/torrents/setCategory"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsAddTags(const QStringList &hashes, const QString &tags)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"tags"_s, tags);
        return postForm(u"/api/v2/torrents/addTags"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRemoveTags(const QStringList &hashes, const QString &tags)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"tags"_s, tags);
        return postForm(u"/api/v2/torrents/removeTags"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetTags(const QStringList &hashes, const QString &tags)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"tags"_s, tags);
        return postForm(u"/api/v2/torrents/setTags"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRename(const QString &hash, const QString &name)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"name"_s, name);
        return postForm(u"/api/v2/torrents/rename"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetUploadLimit(const QStringList &hashes, int limit)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"limit"_s, QString::number(limit));
        return postForm(u"/api/v2/torrents/setUploadLimit"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetDownloadLimit(const QStringList &hashes, int limit)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"limit"_s, QString::number(limit));
        return postForm(u"/api/v2/torrents/setDownloadLimit"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetShareLimits(const QStringList &hashes,
        qreal ratioLimit, int seedingTimeLimit, int inactiveSeedingTimeLimit)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"ratioLimit"_s, QString::number(ratioLimit));
        form.addQueryItem(u"seedingTimeLimit"_s, QString::number(seedingTimeLimit));
        form.addQueryItem(u"inactiveSeedingTimeLimit"_s, QString::number(inactiveSeedingTimeLimit));
        return postForm(u"/api/v2/torrents/setShareLimits"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetSavePath(const QStringList &hashes, const QString &path)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"savePath"_s, path);
        return postForm(u"/api/v2/torrents/setSavePath"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetDownloadPath(const QStringList &hashes, const QString &path)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"downloadPath"_s, path);
        return postForm(u"/api/v2/torrents/setDownloadPath"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetForceStart(const QStringList &hashes, bool force)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"value"_s, force ? u"true"_s : u"false"_s);
        return postForm(u"/api/v2/torrents/setForceStart"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetSuperSeeding(const QStringList &hashes, bool enabled)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"value"_s, enabled ? u"true"_s : u"false"_s);
        return postForm(u"/api/v2/torrents/setSuperSeeding"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetAutoManagement(const QStringList &hashes, bool enabled)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        form.addQueryItem(u"enable"_s, enabled ? u"true"_s : u"false"_s);
        return postForm(u"/api/v2/torrents/setAutoManagement"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetSequentialDownload(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/toggleSequentialDownload"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetFirstLastPiecePrio(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/toggleFirstLastPiecePrio"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsIncreasePrio(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/increasePrio"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsDecreasePrio(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/decreasePrio"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsTopPrio(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/topPrio"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsBottomPrio(const QStringList &hashes)
    {
        QUrlQuery form;
        form.addQueryItem(u"hashes"_s, joinHashes(hashes));
        return postForm(u"/api/v2/torrents/bottomPrio"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetFilePrio(const QString &hash, const QList<int> &fileIds, int priority)
    {
        QStringList ids;
        ids.reserve(fileIds.size());
        for (int id : fileIds)
            ids.append(QString::number(id));
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"id"_s, ids.join(u'|'));
        form.addQueryItem(u"priority"_s, QString::number(priority));
        return postForm(u"/api/v2/torrents/filePrio"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsAdd(const QByteArray &torrentData, const QVariantMap &params)
    {
        auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

        // Torrent file part
        if (!torrentData.isEmpty())
        {
            QHttpPart filePart;
            filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                u"form-data; name=\"torrents\"; filename=\"upload.torrent\""_s);
            filePart.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-bittorrent"_s);
            filePart.setBody(torrentData);
            multiPart->append(filePart);
        }

        // Additional parameters
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           u"form-data; name=\"%1\""_s.arg(it.key()));
            part.setBody(it.value().toString().toUtf8());
            multiPart->append(part);
        }

        // Use a plain request without the form-encoded Content-Type (multipart sets its own)
        QUrl url = m_baseUrl;
        url.setPath(u"/api/v2/torrents/add"_s);
        QNetworkRequest req(url);
        if (!m_sid.isEmpty())
            req.setRawHeader("Cookie", (u"SID="_s + m_sid).toUtf8());
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_nam->post(req, multiPart);
        multiPart->setParent(reply);

        auto promise = std::make_shared<QPromise<QByteArray>>();
        promise->start();
        QFuture<QByteArray> future = promise->future();
        bridgeReplyToPromise(reply, promise);

        return future.then([](QByteArray) {}).onFailed([](const std::exception &) {});
    }

    QFuture<void> ApiClient::torrentsAddTrackers(const QString &hash, const QString &urlsNewlineSeparated)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"urls"_s, urlsNewlineSeparated);
        return postForm(u"/api/v2/torrents/addTrackers"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsEditTracker(const QString &hash, const QString &origUrl,
                                                  const QString &newUrl)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"origUrl"_s, origUrl);
        form.addQueryItem(u"newUrl"_s, newUrl);
        return postForm(u"/api/v2/torrents/editTracker"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRemoveTrackers(const QString &hash, const QStringList &urls)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"urls"_s, urls.join(u'\n'));
        return postForm(u"/api/v2/torrents/removeTrackers"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsAddWebSeeds(const QString &hash, const QStringList &urls)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"urls"_s, urls.join(u'\n'));
        return postForm(u"/api/v2/torrents/addWebSeeds"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRemoveWebSeeds(const QString &hash, const QStringList &urls)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"urls"_s, urls.join(u'\n'));
        return postForm(u"/api/v2/torrents/removeWebSeeds"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRenameFile(const QString &hash, const QString &oldPath,
                                                 const QString &newPath)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"oldPath"_s, oldPath);
        form.addQueryItem(u"newPath"_s, newPath);
        return postForm(u"/api/v2/torrents/renameFile"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsRenameFolder(const QString &hash, const QString &oldPath,
                                                   const QString &newPath)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"oldPath"_s, oldPath);
        form.addQueryItem(u"newPath"_s, newPath);
        return postForm(u"/api/v2/torrents/renameFolder"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetSSLParameters(const QString &hash, const QVariantMap &sslParams)
    {
        const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(sslParams)).toJson(QJsonDocument::Compact);
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"params"_s, QString::fromUtf8(json));
        return postForm(u"/api/v2/torrents/setSSLParameters"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::torrentsSetComment(const QString &hash, const QString &comment)
    {
        QUrlQuery form;
        form.addQueryItem(u"hash"_s, hash);
        form.addQueryItem(u"comment"_s, comment);
        return postForm(u"/api/v2/torrents/setComment"_s, form).then([](QByteArray) {});
    }

    // --- Categories / tags ---

    QFuture<QVariantMap> ApiClient::categories()
    {
        return getRaw(u"/api/v2/torrents/categories"_s).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<void> ApiClient::createCategory(const QString &name, const QString &savePath,
                                             const QString &downloadPath)
    {
        QUrlQuery form;
        form.addQueryItem(u"category"_s, name);
        form.addQueryItem(u"savePath"_s, savePath);
        if (!downloadPath.isEmpty())
            form.addQueryItem(u"downloadPath"_s, downloadPath);
        return postForm(u"/api/v2/torrents/createCategory"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::editCategory(const QString &name, const QString &savePath,
                                           const QString &downloadPath)
    {
        QUrlQuery form;
        form.addQueryItem(u"category"_s, name);
        form.addQueryItem(u"savePath"_s, savePath);
        if (!downloadPath.isEmpty())
            form.addQueryItem(u"downloadPath"_s, downloadPath);
        return postForm(u"/api/v2/torrents/editCategory"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::removeCategories(const QStringList &names)
    {
        QUrlQuery form;
        form.addQueryItem(u"categories"_s, names.join(u'\n'));
        return postForm(u"/api/v2/torrents/removeCategories"_s, form).then([](QByteArray) {});
    }

    QFuture<QStringList> ApiClient::tags()
    {
        return getRaw(u"/api/v2/torrents/tags"_s).then(
            [](const QByteArray &data) -> QStringList
            {
                QStringList result;
                const QJsonArray arr = QJsonDocument::fromJson(data).array();
                result.reserve(arr.size());
                for (const QJsonValue &v : arr)
                    result.append(v.toString());
                return result;
            });
    }

    QFuture<void> ApiClient::createTags(const QStringList &tagNames)
    {
        QUrlQuery form;
        form.addQueryItem(u"tags"_s, tagNames.join(u','));
        return postForm(u"/api/v2/torrents/createTags"_s, form).then([](QByteArray) {});
    }

    QFuture<void> ApiClient::deleteTags(const QStringList &tagNames)
    {
        QUrlQuery form;
        form.addQueryItem(u"tags"_s, tagNames.join(u','));
        return postForm(u"/api/v2/torrents/deleteTags"_s, form).then([](QByteArray) {});
    }

    // --- Search ---

    QFuture<QVariantMap> ApiClient::searchStart(const QString &pattern, const QString &category, const QString &plugins)
    {
        QUrlQuery form;
        form.addQueryItem(u"pattern"_s, pattern);
        form.addQueryItem(u"category"_s, category);
        form.addQueryItem(u"plugins"_s, plugins);
        return postForm(u"/api/v2/search/start"_s, form).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<QVariantMap> ApiClient::searchResults(int id, int offset)
    {
        QUrlQuery query;
        query.addQueryItem(u"id"_s, QString::number(id));
        query.addQueryItem(u"offset"_s, QString::number(offset));
        return getRaw(u"/api/v2/search/results"_s, query).then(
            [](const QByteArray &data) -> QVariantMap
            {
                return QJsonDocument::fromJson(data).object().toVariantMap();
            });
    }

    QFuture<void> ApiClient::searchStop(int id)
    {
        QUrlQuery form;
        form.addQueryItem(u"id"_s, QString::number(id));
        return postForm(u"/api/v2/search/stop"_s, form).then([](QByteArray) {});
    }

    QFuture<QVariantList> ApiClient::searchPlugins()
    {
        return getRaw(u"/api/v2/search/plugins"_s).then(
            [](const QByteArray &data) -> QVariantList
            {
                return QJsonDocument::fromJson(data).array().toVariantList();
            });
    }

    // --- Private helpers ---

    QNetworkRequest ApiClient::makeRequest(const QString &apiPath) const
    {
        QUrl url = m_baseUrl;
        url.setPath(apiPath);
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
        if (!m_sid.isEmpty())
            req.setRawHeader("Cookie", (u"SID="_s + m_sid).toUtf8());
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        return req;
    }

    QFuture<QByteArray> ApiClient::getRaw(const QString &apiPath, const QUrlQuery &query)
    {
        QNetworkRequest req = makeRequest(apiPath);
        if (!query.isEmpty())
        {
            QUrl url = req.url();
            url.setQuery(query);
            req.setUrl(url);
        }

        QNetworkReply *reply = m_nam->get(req);

        // Capture SID from response cookies (only on login endpoint, but safe to always check)
        connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            const QList<QNetworkReply::RawHeaderPair> headers = reply->rawHeaderPairs();
            for (const auto &[name, value] : headers)
            {
                if (name.toLower() == "set-cookie")
                {
                    const QString cookieStr = QString::fromUtf8(value);
                    for (const QString &part : cookieStr.split(u';'))
                    {
                        const QString trimmed = part.trimmed();
                        if (trimmed.startsWith(u"SID="_s))
                        {
                            m_sid = trimmed.mid(4);
                            break;
                        }
                    }
                    break;
                }
            }
        });

        auto promise = std::make_shared<QPromise<QByteArray>>();
        promise->start();
        QFuture<QByteArray> future = promise->future();
        bridgeReplyToPromise(reply, promise);
        return future;
    }

    QFuture<QByteArray> ApiClient::postRaw(const QString &apiPath, const QByteArray &body,
                                            const QString &contentType)
    {
        QNetworkRequest req = makeRequest(apiPath);
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        QNetworkReply *reply = m_nam->post(req, body);

        // Capture SID from login response (Set-Cookie header)
        connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            const QList<QNetworkReply::RawHeaderPair> headers = reply->rawHeaderPairs();
            for (const auto &[name, value] : headers)
            {
                if (name.toLower() == "set-cookie")
                {
                    const QString cookieStr = QString::fromUtf8(value);
                    for (const QString &part : cookieStr.split(u';'))
                    {
                        const QString trimmed = part.trimmed();
                        if (trimmed.startsWith(u"SID="_s))
                        {
                            m_sid = trimmed.mid(4);
                            break;
                        }
                    }
                    break;
                }
            }
        });

        auto promise = std::make_shared<QPromise<QByteArray>>();
        promise->start();
        QFuture<QByteArray> future = promise->future();
        bridgeReplyToPromise(reply, promise);
        return future;
    }

    QFuture<QByteArray> ApiClient::postForm(const QString &apiPath, const QUrlQuery &form)
    {
        return postRaw(apiPath, form.toString(QUrl::FullyEncoded).toUtf8(),
                       u"application/x-www-form-urlencoded"_s);
    }

    QString ApiClient::joinHashes(const QStringList &hashes)
    {
        return hashes.join(u'|');
    }
}
