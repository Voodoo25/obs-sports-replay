/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "sr-dock.h"
#include "sr-config.h"
#include "sr-thumb.h"
#include "sr-capture.h"
#include "sr-credit.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <cstring>

#include <QWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QListWidget>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QIcon>
#include <QShowEvent>

#define THUMB_W 112
#define THUMB_H 63
#define MAX_ITEMS 10

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* A small "Sports Replay (version) by Systec" clickable credit label, shown
 * at the bottom of the dock and its dialogs, matching the footer convention
 * used by other OBS plugins (e.g. Exeldro's). */
static QLabel *makeCreditLabel(QWidget *parent)
{
	char buf[256];
	auto *label = new QLabel(QString::fromUtf8(sr_plugin_credit_html(buf, sizeof(buf))), parent);
	label->setTextFormat(Qt::RichText);
	label->setTextInteractionFlags(Qt::TextBrowserInteraction);
	label->setOpenExternalLinks(true);
	label->setStyleSheet("color: gray; font-size: 10px;");
	return label;
}

namespace {

bool enum_replay_sources(void *param, obs_source_t *source)
{
	auto *names = static_cast<QStringList *>(param);
	if (strcmp(obs_source_get_unversioned_id(source), SR_PLAYBACK_ID) == 0)
		names->append(QString::fromUtf8(obs_source_get_name(source)));
	return true;
}

/* Name of the first Sports Replay source in the current scene collection. */
QByteArray firstReplaySource()
{
	QStringList names;
	obs_enum_sources(enum_replay_sources, &names);
	return names.isEmpty() ? QByteArray() : names.first().toUtf8();
}

/* The scene source that contains the given source, or nullptr (ref'd). */
obs_source_t *sceneContaining(const char *sourceName)
{
	if (!sourceName || !*sourceName)
		return nullptr;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	obs_source_t *result = nullptr;
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sceneSrc = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		if (scene && obs_scene_find_source(scene, sourceName)) {
			result = obs_source_get_ref(sceneSrc);
			break;
		}
	}
	obs_frontend_source_list_free(&scenes);
	return result;
}

class SrDock : public QWidget {
public:
	explicit SrDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(4, 4, 4, 4);

		// slim top bar: just a settings gear on the right
		auto *bar = new QHBoxLayout();
		bar->addStretch(1);
		auto *gear = new QToolButton(this);
		gear->setText(QString::fromUtf8("\xE2\x9A\x99")); // gear glyph
		gear->setToolTip(T("Dock.Settings"));
		gear->setAutoRaise(true);
		bar->addWidget(gear);
		root->addLayout(bar);

		list = new QListWidget(this);
		list->setViewMode(QListView::IconMode);
		list->setIconSize(QSize(THUMB_W, THUMB_H));
		list->setGridSize(QSize(THUMB_W + 20, THUMB_H + 40));
		list->setResizeMode(QListView::Adjust);
		list->setMovement(QListView::Static);
		list->setWordWrap(true);
		list->setSpacing(4);
		root->addWidget(list, 1);

		auto *hint = new QLabel(T("Dock.Hint"), this);
		hint->setWordWrap(true);
		hint->setStyleSheet("color: gray;");
		root->addWidget(hint);

		root->addWidget(makeCreditLabel(this));

		watcher = new QFileSystemWatcher(this);

		/* a new file appears before its mp4 index (moov) is finished, so
		 * debounce the refresh to let the file become readable */
		refreshTimer = new QTimer(this);
		refreshTimer->setSingleShot(true);
		connect(refreshTimer, &QTimer::timeout, this, [this]() { refreshList(); });

		char *dir = sr_config_get_save_dir();
		currentFolder = QString::fromUtf8(dir);
		bfree(dir);

		watchFolder();
		refreshList();

		connect(gear, &QToolButton::clicked, this, [this]() { openSettings(); });
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { launch(item); });
		connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { refreshTimer->start(700); });
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		refreshList();
	}

private:
	void openSettings()
	{
		QDialog dlg(this);
		dlg.setWindowTitle(T("Dock.SettingsTitle"));
		auto *lay = new QVBoxLayout(&dlg);

		lay->addWidget(new QLabel(T("Dock.Folder"), &dlg));
		auto *row = new QHBoxLayout();
		auto *edit = new QLineEdit(currentFolder, &dlg);
		edit->setReadOnly(true);
		edit->setMinimumWidth(320);
		auto *browse = new QPushButton(QStringLiteral("..."), &dlg);
		browse->setMaximumWidth(36);
		row->addWidget(edit, 1);
		row->addWidget(browse);
		lay->addLayout(row);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);

		auto *footer = new QHBoxLayout();
		footer->addWidget(makeCreditLabel(&dlg));
		footer->addStretch(1);
		footer->addWidget(buttons);
		lay->addLayout(footer);

		connect(browse, &QPushButton::clicked, &dlg, [&]() {
			QString picked = QFileDialog::getExistingDirectory(&dlg, T("Dock.PickFolder"), edit->text());
			if (!picked.isEmpty())
				edit->setText(picked);
		});
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		if (dlg.exec() == QDialog::Accepted) {
			currentFolder = edit->text();
			QByteArray f = currentFolder.toUtf8();
			sr_config_set_save_dir(f.constData());
			watchFolder();
			refreshList();
		}
	}

	void watchFolder()
	{
		if (!watcher->directories().isEmpty())
			watcher->removePaths(watcher->directories());
		if (!currentFolder.isEmpty() && QDir(currentFolder).exists())
			watcher->addPath(currentFolder);
	}

	void refreshList()
	{
		list->clear();

		if (currentFolder.isEmpty())
			return;
		QDir dir(currentFolder);
		if (!dir.exists())
			return;

		QStringList filters;
		filters << "*.mp4";
		QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

		int count = 0;
		for (const QFileInfo &fi : files) {
			if (count >= MAX_ITEMS)
				break;
			QByteArray path = fi.absoluteFilePath().toUtf8();

			QIcon icon;
			uint8_t *rgba = nullptr;
			if (sr_thumbnail_rgba(path.constData(), THUMB_W, THUMB_H, &rgba) && rgba) {
				QImage img(rgba, THUMB_W, THUMB_H, THUMB_W * 4, QImage::Format_RGBA8888);
				icon = QIcon(QPixmap::fromImage(img.copy()));
				bfree(rgba);
			}

			auto *item = new QListWidgetItem(icon, fi.completeBaseName());
			item->setData(Qt::UserRole, fi.absoluteFilePath());
			item->setToolTip(fi.fileName());
			list->addItem(item);
			count++;
		}
	}

	void launch(QListWidgetItem *item)
	{
		if (!item)
			return;
		QByteArray path = item->data(Qt::UserRole).toString().toUtf8();

		QByteArray srcName = firstReplaySource();
		if (srcName.isEmpty())
			return;

		obs_source_t *source = obs_get_source_by_name(srcName.constData());
		if (source) {
			sr_playback_play_file(source, path.constData());
			obs_source_release(source);
		}

		obs_source_t *scene = sceneContaining(srcName.constData());
		if (scene) {
			obs_frontend_set_current_scene(scene);
			obs_source_release(scene);
		}
	}

	QString currentFolder;
	QListWidget *list = nullptr;
	QFileSystemWatcher *watcher = nullptr;
	QTimer *refreshTimer = nullptr;
};

} // namespace

void sr_dock_register(void)
{
	auto *dock = new SrDock();
	dock->setObjectName("SportsReplayDock");
	if (!obs_frontend_add_dock_by_id("sports_replay_dock", obs_module_text("Dock.Title"), dock))
		delete dock;
}
