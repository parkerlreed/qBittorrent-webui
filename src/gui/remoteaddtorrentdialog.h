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

#include "base/bittorrent/addtorrentparams.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

class RemoteAddTorrentDialog : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(RemoteAddTorrentDialog)

public:
    explicit RemoteAddTorrentDialog(const QString &source, QWidget *parent = nullptr);

    BitTorrent::AddTorrentParams params() const;

private:
    QComboBox *m_autoTMMCombo = nullptr;
    QLineEdit *m_savePathEdit = nullptr;
    QLineEdit *m_renameEdit = nullptr;
    QComboBox *m_categoryCombo = nullptr;
    QCheckBox *m_startCheckbox = nullptr;
    QCheckBox *m_topQueueCheckbox = nullptr;
    QComboBox *m_stopConditionCombo = nullptr;
    QCheckBox *m_skipHashCheckbox = nullptr;
    QComboBox *m_contentLayoutCombo = nullptr;
    QCheckBox *m_sequentialCheckbox = nullptr;
    QCheckBox *m_firstLastCheckbox = nullptr;
    QLineEdit *m_dlLimitEdit = nullptr;
    QLineEdit *m_ulLimitEdit = nullptr;
};
