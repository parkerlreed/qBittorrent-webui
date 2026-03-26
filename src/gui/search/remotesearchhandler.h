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

#include <QString>

#include "base/search/abstractsearchhandler.h"

class QTimer;

namespace Net { class ApiClient; }

// Implements AbstractSearchHandler by driving the remote search API:
//   POST /api/v2/search/start  →  polls /api/v2/search/results  →  emits newSearchResults
// Emits searchFinished() when the remote search stops.
class RemoteSearchHandler final : public AbstractSearchHandler
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RemoteSearchHandler)

public:
    explicit RemoteSearchHandler(const QString &pattern, const QString &category,
                                 const QString &plugins, Net::ApiClient *client,
                                 QObject *parent = nullptr);
    ~RemoteSearchHandler() override;

    bool isActive() const override;
    QString pattern() const override;
    void cancelSearch() override;

private:
    void poll();
    void onResults(const QVariantMap &data);

    Net::ApiClient *m_client;
    const QString m_pattern;
    const QString m_category;
    const QString m_plugins;

    int m_searchId = -1;
    int m_offset = 0;
    bool m_active = false;
    bool m_cancelled = false;

    QTimer *m_pollTimer;
};
