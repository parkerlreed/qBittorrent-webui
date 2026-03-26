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

#include "remoteaddtorrentdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/bittorrent/torrentcontentlayout.h"
#include "base/global.h"
#include "base/path.h"

RemoteAddTorrentDialog::RemoteAddTorrentDialog(const QString &source, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Add Torrent"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Source info
    auto *sourceLabel = new QLabel(source.length() > 90
        ? source.left(87) + u"..."_s : source, this);
    sourceLabel->setWordWrap(true);
    form->addRow(tr("Source:"), sourceLabel);

    // Torrent Management Mode
    m_autoTMMCombo = new QComboBox(this);
    m_autoTMMCombo->addItem(tr("Manual"), false);
    m_autoTMMCombo->addItem(tr("Automatic"), true);
    m_autoTMMCombo->setCurrentIndex(1); // Default: Automatic
    form->addRow(tr("Torrent Management Mode:"), m_autoTMMCombo);

    // Save path
    m_savePathEdit = new QLineEdit(this);
    m_savePathEdit->setText(BitTorrent::Session::instance()->savePath().toString());
    form->addRow(tr("Save files to:"), m_savePathEdit);

    // Rename
    m_renameEdit = new QLineEdit(this);
    form->addRow(tr("Rename torrent:"), m_renameEdit);

    // Category
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->addItem(QString());
    const QStringList cats = BitTorrent::Session::instance()->categories();
    for (const QString &cat : cats)
        m_categoryCombo->addItem(cat);
    form->addRow(tr("Category:"), m_categoryCombo);

    // Start torrent
    m_startCheckbox = new QCheckBox(tr("Start torrent"), this);
    m_startCheckbox->setChecked(true);
    form->addRow(QString(), m_startCheckbox);

    // Add to top of queue
    m_topQueueCheckbox = new QCheckBox(tr("Add to top of queue"), this);
    form->addRow(QString(), m_topQueueCheckbox);

    // Stop condition
    m_stopConditionCombo = new QComboBox(this);
    m_stopConditionCombo->addItem(tr("None"), u"None"_s);
    m_stopConditionCombo->addItem(tr("Metadata received"), u"MetadataReceived"_s);
    m_stopConditionCombo->addItem(tr("Files checked"), u"FilesChecked"_s);
    form->addRow(tr("Stop condition:"), m_stopConditionCombo);

    // Skip hash check
    m_skipHashCheckbox = new QCheckBox(tr("Skip hash check"), this);
    form->addRow(QString(), m_skipHashCheckbox);

    // Content layout
    m_contentLayoutCombo = new QComboBox(this);
    m_contentLayoutCombo->addItem(tr("Original"), u"Original"_s);
    m_contentLayoutCombo->addItem(tr("Create subfolder"), u"Subfolder"_s);
    m_contentLayoutCombo->addItem(tr("Don't create subfolder"), u"NoSubfolder"_s);
    form->addRow(tr("Content layout:"), m_contentLayoutCombo);

    // Sequential
    m_sequentialCheckbox = new QCheckBox(tr("Download in sequential order"), this);
    form->addRow(QString(), m_sequentialCheckbox);

    // First/last piece
    m_firstLastCheckbox = new QCheckBox(tr("Download first and last pieces first"), this);
    form->addRow(QString(), m_firstLastCheckbox);

    // Rate limits
    m_dlLimitEdit = new QLineEdit(this);
    m_dlLimitEdit->setPlaceholderText(tr("KiB/s (0 = unlimited)"));
    form->addRow(tr("Limit download rate:"), m_dlLimitEdit);

    m_ulLimitEdit = new QLineEdit(this);
    m_ulLimitEdit->setPlaceholderText(tr("KiB/s (0 = unlimited)"));
    form->addRow(tr("Limit upload rate:"), m_ulLimitEdit);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add Torrent"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    resize(520, sizeHint().height());
}

BitTorrent::AddTorrentParams RemoteAddTorrentDialog::params() const
{
    BitTorrent::AddTorrentParams p;

    // Torrent Management Mode
    p.useAutoTMM = m_autoTMMCombo->currentData().toBool();

    // Save path (only meaningful when not using autoTMM)
    const QString savePathStr = m_savePathEdit->text().trimmed();
    if (!savePathStr.isEmpty())
        p.savePath = Path(savePathStr);

    // Rename
    const QString rename = m_renameEdit->text().trimmed();
    if (!rename.isEmpty())
        p.name = rename;

    // Category
    p.category = m_categoryCombo->currentText();

    // Start / stopped
    p.addStopped = !m_startCheckbox->isChecked();

    // Queue position
    if (m_topQueueCheckbox->isChecked())
        p.addToQueueTop = true;

    // Stop condition
    const QString stopCond = m_stopConditionCombo->currentData().toString();
    if (stopCond == u"MetadataReceived"_s)
        p.stopCondition = BitTorrent::Torrent::StopCondition::MetadataReceived;
    else if (stopCond == u"FilesChecked"_s)
        p.stopCondition = BitTorrent::Torrent::StopCondition::FilesChecked;
    else
        p.stopCondition = BitTorrent::Torrent::StopCondition::None;

    // Skip hash check
    p.skipChecking = m_skipHashCheckbox->isChecked();

    // Content layout
    const QString layout = m_contentLayoutCombo->currentData().toString();
    if (layout == u"Subfolder"_s)
        p.contentLayout = BitTorrent::TorrentContentLayout::Subfolder;
    else if (layout == u"NoSubfolder"_s)
        p.contentLayout = BitTorrent::TorrentContentLayout::NoSubfolder;
    else
        p.contentLayout = BitTorrent::TorrentContentLayout::Original;

    // Sequential / first-last
    p.sequential = m_sequentialCheckbox->isChecked();
    p.firstLastPiecePriority = m_firstLastCheckbox->isChecked();

    // Rate limits (convert KiB/s → bytes/s; 0 or empty = unlimited)
    const int dlKibs = m_dlLimitEdit->text().trimmed().toInt();
    p.downloadLimit = dlKibs > 0 ? dlKibs * 1024 : -1;
    const int ulKibs = m_ulLimitEdit->text().trimmed().toInt();
    p.uploadLimit = ulKibs > 0 ? ulKibs * 1024 : -1;

    return p;
}
