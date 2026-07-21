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
#include <util/platform.h>

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
#include <QPainter>
#include <QSet>
#include <QRegularExpression>

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

/* Name of the Sports Replay source whose "capture_source" setting matches
 * cameraName, or empty if none do. */
QByteArray replaySourceForCamera(const QString &cameraName)
{
	QStringList names;
	obs_enum_sources(enum_replay_sources, &names);

	QByteArray cameraUtf8 = cameraName.toUtf8();
	for (const QString &name : names) {
		QByteArray nameUtf8 = name.toUtf8();
		obs_source_t *source = obs_get_source_by_name(nameUtf8.constData());
		if (!source)
			continue;
		obs_data_t *settings = obs_source_get_settings(source);
		const char *capture = obs_data_get_string(settings, S_CAPTURE_SOURCE);
		bool match = capture && cameraUtf8 == capture;
		obs_data_release(settings);
		obs_source_release(source);
		if (match)
			return nameUtf8;
	}
	return QByteArray();
}

/* Extracts the camera name from a saved replay's base filename, of the form
 * "<camera>_<YYYYMMDD-HHMMSS>" (see sr_playback_capture_replay). Empty if the
 * name doesn't match that pattern (e.g. a file dropped in by hand). */
QString cameraNameFromFile(const QString &baseName)
{
	static const QRegularExpression re(QStringLiteral("^(.*)_\\d{8}-\\d{6}$"));
	QRegularExpressionMatch m = re.match(baseName);
	return m.hasMatch() ? m.captured(1) : QString();
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

/* Stamps a green checkmark badge on the bottom-right corner of a thumbnail
 * pixmap, marking a replay that's already been sent to program so it's not
 * confused with one still waiting to be used. */
void drawPlayedBadge(QPixmap &pixmap)
{
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);

	const int badgeSize = 18;
	const int margin = 3;
	QRect badgeRect(pixmap.width() - badgeSize - margin, pixmap.height() - badgeSize - margin, badgeSize, badgeSize);

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(46, 204, 64));
	painter.drawRoundedRect(badgeRect, 4, 4);

	QPen checkPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(checkPen);
	painter.drawLine(badgeRect.left() + 4, badgeRect.center().y() + 1, badgeRect.left() + 7, badgeRect.bottom() - 4);
	painter.drawLine(badgeRect.left() + 7, badgeRect.bottom() - 4, badgeRect.right() - 3, badgeRect.top() + 4);
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

		loadPlayedPaths();
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

	static QString playedConfigPath()
	{
		char *path = obs_module_config_path("played.json");
		QString result = path ? QString::fromUtf8(path) : QString();
		bfree(path);
		return result;
	}

	/* Persisted across OBS restarts so a crash mid-broadcast doesn't lose
	 * track of which replays already went to air. */
	void loadPlayedPaths()
	{
		QString cfgPath = playedConfigPath();
		if (cfgPath.isEmpty())
			return;
		QByteArray cfgUtf8 = cfgPath.toUtf8();
		obs_data_t *data = obs_data_create_from_json_file(cfgUtf8.constData());
		if (!data)
			return;

		obs_data_array_t *arr = obs_data_get_array(data, "played");
		if (arr) {
			const size_t count = obs_data_array_count(arr);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *entry = obs_data_array_item(arr, i);
				const char *p = obs_data_get_string(entry, "path");
				if (p && *p)
					playedPaths.insert(QString::fromUtf8(p));
				obs_data_release(entry);
			}
			obs_data_array_release(arr);
		}
		obs_data_release(data);
	}

	void savePlayedPaths()
	{
		QString cfgPath = playedConfigPath();
		if (cfgPath.isEmpty())
			return;

		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}

		obs_data_array_t *arr = obs_data_array_create();
		for (const QString &p : playedPaths) {
			obs_data_t *entry = obs_data_create();
			QByteArray pUtf8 = p.toUtf8();
			obs_data_set_string(entry, "path", pUtf8.constData());
			obs_data_array_push_back(arr, entry);
			obs_data_release(entry);
		}

		obs_data_t *data = obs_data_create();
		obs_data_set_array(data, "played", arr);
		QByteArray cfgUtf8 = cfgPath.toUtf8();
		obs_data_save_json(data, cfgUtf8.constData());
		obs_data_array_release(arr);
		obs_data_release(data);
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

		/* drop played-markers for replays that no longer exist (deleted
		 * from the folder), so the persisted list doesn't grow stale */
		QSet<QString> existing;
		for (const QFileInfo &fi : files)
			existing.insert(fi.absoluteFilePath());
		bool pruned = false;
		for (auto it = playedPaths.begin(); it != playedPaths.end();) {
			if (!existing.contains(*it)) {
				it = playedPaths.erase(it);
				pruned = true;
			} else {
				++it;
			}
		}
		if (pruned)
			savePlayedPaths();

		int count = 0;
		for (const QFileInfo &fi : files) {
			if (count >= MAX_ITEMS)
				break;
			QByteArray path = fi.absoluteFilePath().toUtf8();

			QIcon icon;
			uint8_t *rgba = nullptr;
			if (sr_thumbnail_rgba(path.constData(), THUMB_W, THUMB_H, &rgba) && rgba) {
				QImage img(rgba, THUMB_W, THUMB_H, THUMB_W * 4, QImage::Format_RGBA8888);
				QPixmap pixmap = QPixmap::fromImage(img.copy());
				if (playedPaths.contains(fi.absoluteFilePath()))
					drawPlayedBadge(pixmap);
				icon = QIcon(pixmap);
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
		QString filePath = item->data(Qt::UserRole).toString();
		QByteArray path = filePath.toUtf8();

		/* route to the playback source for the camera this replay was
		 * captured from, falling back to the first one found if the
		 * filename doesn't match the expected pattern or no source
		 * claims that camera anymore */
		QString cameraName = cameraNameFromFile(QFileInfo(filePath).completeBaseName());
		QByteArray srcName = cameraName.isEmpty() ? QByteArray() : replaySourceForCamera(cameraName);
		if (srcName.isEmpty())
			srcName = firstReplaySource();
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

		if (!playedPaths.contains(filePath)) {
			playedPaths.insert(filePath);
			savePlayedPaths();
			QPixmap pixmap = item->icon().pixmap(THUMB_W, THUMB_H);
			drawPlayedBadge(pixmap);
			item->setIcon(QIcon(pixmap));
		}
	}

	QString currentFolder;
	QListWidget *list = nullptr;
	QFileSystemWatcher *watcher = nullptr;
	QTimer *refreshTimer = nullptr;
	QSet<QString> playedPaths;
};

} // namespace

void sr_dock_register(void)
{
	auto *dock = new SrDock();
	dock->setObjectName("SportsReplayDock");
	if (!obs_frontend_add_dock_by_id("sports_replay_dock", obs_module_text("Dock.Title"), dock))
		delete dock;
}
