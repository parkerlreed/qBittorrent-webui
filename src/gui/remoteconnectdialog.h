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

#include <QDialog>
#include <QUrl>

namespace Ui { class RemoteConnectDialog; }
namespace Net { class ApiClient; }

// Dialog for connecting to a remote qBittorrent WebUI instance.
// On successful login, emits connected() with the base URL, username, and password.
class RemoteConnectDialog final : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RemoteConnectDialog)

public:
    explicit RemoteConnectDialog(QWidget *parent = nullptr);
    ~RemoteConnectDialog() override;

signals:
    void connected(const QUrl &baseUrl, const QString &username, const QString &password);

private slots:
    void onConnectClicked();
    void onLoggedIn();
    void onLoginFailed(const QString &reason);

private:
    void setUIEnabled(bool enabled);

    Ui::RemoteConnectDialog *m_ui = nullptr;
    Net::ApiClient *m_client = nullptr;
    QString m_pendingUsername;
    QString m_pendingPassword;
};
