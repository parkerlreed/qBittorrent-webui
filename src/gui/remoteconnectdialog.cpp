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

#include "remoteconnectdialog.h"

#include <QPushButton>
#include <QUrl>

#include "base/global.h"
#include "base/net/apiclient.h"
#include "ui_remoteconnectdialog.h"

RemoteConnectDialog::RemoteConnectDialog(QWidget *parent)
    : QDialog(parent)
    , m_ui(new Ui::RemoteConnectDialog)
{
    m_ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    // Wire the OK button to our connect logic (not the dialog accept)
    QPushButton *okBtn = m_ui->buttonBox->button(QDialogButtonBox::Ok);
    okBtn->setText(tr("Connect"));
    disconnect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &RemoteConnectDialog::onConnectClicked);
}

RemoteConnectDialog::~RemoteConnectDialog()
{
    delete m_ui;
}

void RemoteConnectDialog::onConnectClicked()
{
    const QUrl baseUrl = QUrl(m_ui->hostEdit->text().trimmed());
    if (!baseUrl.isValid() || baseUrl.host().isEmpty())
    {
        m_ui->statusLabel->setText(tr("Invalid URL."));
        return;
    }

    m_pendingUsername = m_ui->usernameEdit->text();
    m_pendingPassword = m_ui->passwordEdit->text();

    setUIEnabled(false);
    m_ui->statusLabel->setText(tr("Connecting..."));

    // Create a temporary client just for login
    delete m_client;
    m_client = new Net::ApiClient(baseUrl, this);
    connect(m_client, &Net::ApiClient::loggedIn, this, &RemoteConnectDialog::onLoggedIn);
    connect(m_client, &Net::ApiClient::loginFailed, this, &RemoteConnectDialog::onLoginFailed);
    m_client->login(m_pendingUsername, m_pendingPassword);
}

void RemoteConnectDialog::onLoggedIn()
{
    const QUrl baseUrl = QUrl(m_ui->hostEdit->text().trimmed());
    // Accept first so the dialog is closed/scheduled for deletion before
    // connectToRemote() tears down the session and recreates MainWindow.
    accept();
    emit connected(baseUrl, m_pendingUsername, m_pendingPassword);
}

void RemoteConnectDialog::onLoginFailed(const QString &reason)
{
    m_ui->statusLabel->setText(tr("Login failed: %1").arg(reason));
    setUIEnabled(true);
}

void RemoteConnectDialog::setUIEnabled(bool enabled)
{
    m_ui->hostEdit->setEnabled(enabled);
    m_ui->usernameEdit->setEnabled(enabled);
    m_ui->passwordEdit->setEnabled(enabled);
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(enabled);
}
