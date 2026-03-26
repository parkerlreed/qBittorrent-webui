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

#include "remotesearchhandler.h"

#include <QDateTime>
#include <QTimer>
#include <QVariant>

#include "base/global.h"
#include "base/net/apiclient.h"
#include "base/search/searchhandler.h"  // for SearchResult

RemoteSearchHandler::RemoteSearchHandler(const QString &pattern, const QString &category,
                                         const QString &plugins, Net::ApiClient *client,
                                         QObject *parent)
    : AbstractSearchHandler(parent)
    , m_client(client)
    , m_pattern(pattern)
    , m_category(category)
    , m_plugins(plugins)
    , m_pollTimer(new QTimer(this))
{
    m_pollTimer->setInterval(1500);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &RemoteSearchHandler::poll);

    m_active = true;
    m_client->searchStart(m_pattern, m_category, m_plugins).then(this,
        [this](const QVariantMap &result)
        {
            m_searchId = result.value(u"id"_s).toInt();
            if (m_searchId <= 0)
            {
                m_active = false;
                emit searchFailed(tr("Remote search failed to start."));
                return;
            }
            if (m_cancelled)
            {
                // cancelSearch() was called before we got the ID
                m_client->searchStop(m_searchId);
                m_active = false;
                emit searchFinished(true);
                return;
            }
            m_pollTimer->start();
        });
}

RemoteSearchHandler::~RemoteSearchHandler() = default;

bool RemoteSearchHandler::isActive() const
{
    return m_active;
}

QString RemoteSearchHandler::pattern() const
{
    return m_pattern;
}

void RemoteSearchHandler::cancelSearch()
{
    if (!m_active)
        return;

    m_cancelled = true;
    m_pollTimer->stop();

    if (m_searchId > 0)
    {
        m_client->searchStop(m_searchId);
        m_active = false;
        emit searchFinished(true);
    }
    // else: start hasn't returned yet; handled in the start callback
}

void RemoteSearchHandler::poll()
{
    if (!m_active || m_searchId <= 0)
        return;

    m_client->searchResults(m_searchId, m_offset).then(this,
        [this](const QVariantMap &data)
        {
            onResults(data);
        });
}

void RemoteSearchHandler::onResults(const QVariantMap &data)
{
    const QVariantList rawResults = data.value(u"results"_s).toList();
    const QString status = data.value(u"status"_s).toString();

    if (!rawResults.isEmpty())
    {
        QList<SearchResult> results;
        results.reserve(rawResults.size());

        for (const QVariant &v : rawResults)
        {
            const QVariantMap m = v.toMap();
            SearchResult r;
            r.fileName   = m.value(u"fileName"_s).toString();
            r.fileUrl    = m.value(u"fileUrl"_s).toString();
            r.fileSize   = m.value(u"fileSize"_s).toLongLong();
            r.nbSeeders  = m.value(u"nbSeeders"_s).toLongLong();
            r.nbLeechers = m.value(u"nbLeechers"_s).toLongLong();
            r.engineName = m.value(u"engineName"_s).toString();
            r.siteUrl    = m.value(u"siteUrl"_s).toString();
            r.descrLink  = m.value(u"descrLink"_s).toString();
            const auto epoch = m.value(u"pubDate"_s).toLongLong();
            if (epoch > 0)
                r.pubDate = QDateTime::fromSecsSinceEpoch(epoch);
            results.append(r);
        }

        m_offset += results.size();
        emit newSearchResults(results);
    }

    if (status == u"Stopped"_s)
    {
        m_pollTimer->stop();
        m_active = false;
        emit searchFinished(false);
    }
}
