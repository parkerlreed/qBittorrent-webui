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
#include <QList>
#include <QObject>
#include <QString>

struct SearchResult
{
    QString fileName;
    QString fileUrl;
    qlonglong fileSize = 0;
    qlonglong nbSeeders = 0;
    qlonglong nbLeechers = 0;
    QString engineName;
    QString siteUrl;
    QString descrLink;
    QDateTime pubDate;
};

// Abstract interface shared by SearchHandler (local Python) and RemoteSearchHandler (API).
// SearchJobWidget depends only on this class.
class AbstractSearchHandler : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(AbstractSearchHandler)

public:
    using QObject::QObject;

    virtual bool isActive() const = 0;
    virtual QString pattern() const = 0;
    virtual void cancelSearch() = 0;

signals:
    void newSearchResults(const QList<SearchResult> &results);
    void searchFinished(bool cancelled = false);
    void searchFailed(const QString &errorMessage);
};
