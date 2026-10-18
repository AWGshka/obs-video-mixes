#include "ui/dock.hpp"
#include "core/manager.hpp"
#include "core/visibility.hpp"
#include "ui/common.hpp"
#include "ui/mix-settings-dialog.hpp"
#include "ui/preview.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <vector>

namespace vmix::ui {

namespace {

constexpr int kMaxTreeDepth = 8;

QString format_duration(uint64_t ns)
{
	const uint64_t s = ns / 1000000000ULL;
	return QStringLiteral("%1:%2:%3")
		.arg(s / 3600, 2, 10, QLatin1Char('0'))
		.arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(s % 60, 2, 10, QLatin1Char('0'));
}

/* OBS 32 themes style these utility classes (text-muted, text-danger, ...) for light and dark variants. */
void set_class(QWidget *w, const QString &cls)
{
	if (w->property("class").toString() == cls)
		return;
	w->setProperty("class", cls);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

const char *trigger_key(Trigger t)
{
	switch (t) {
	case Trigger::Recording:
		return "Dock.Trigger.Recording";
	case Trigger::Streaming:
		return "Dock.Trigger.Streaming";
	default:
		return "Dock.Trigger.Manual";
	}
}

class MixesDock : public QWidget {
public:
	MixesDock();
	~MixesDock() override;

private:
	struct Row {
		uint32_t id;
		QLabel *dot;
		QLabel *status;
		QToolButton *rec;
		QToolButton *preview;
	};

	void on_manager_changed();
	void rebuild_rows(const std::vector<MixState> &states);
	void update_status();
	void schedule_tree();
	void rebuild_tree();
	void add_scene(obs_scene_t *scene, QTreeWidgetItem *parent, int depth);
	void release_obs_refs();
	void toggle_preview(uint32_t id);
	void open_settings(uint32_t id);
	void add_mix(MixKind kind);

	static void frontend_event(enum obs_frontend_event event, void *data);
	static void scene_signal(void *data, calldata_t *);

	QVBoxLayout *rows_layout_ = nullptr;
	QLabel *empty_label_ = nullptr;
	QLabel *scene_label_ = nullptr;
	QTreeWidget *tree_ = nullptr;
	MixPreview *preview_ = nullptr;
	QTimer status_timer_;

	std::vector<Row> rows_;
	std::vector<uint32_t> columns_; /* mix id per tree column (column index - 1) */
	std::vector<std::string> column_names_;
	std::vector<OBSSceneItem> tree_items_;
	std::vector<OBSSignal> scene_signals_;
	OBSSignal rename_signal_;
	std::atomic<bool> tree_pending_{false};
	int listener_ = 0;
};

MixesDock::MixesDock()
{
	setMinimumWidth(260);

	auto *title = new QLabel(T("Dock.Mixes"));
	set_class(title, QStringLiteral("text-bold"));
	auto *add = new QToolButton();
	add->setText(QStringLiteral("+"));
	add->setToolTip(T("Dock.AddMix"));
	add->setAutoRaise(true);
	add->setPopupMode(QToolButton::InstantPopup);
	add->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
	auto *add_menu = new QMenu(add);
	add_menu->addAction(T("Dock.AddProgramMix"), this, [this] { add_mix(MixKind::Program); })
		->setToolTip(T("Dock.AddProgramMix.Hint"));
	add_menu->addAction(T("Dock.AddSourceMix"), this, [this] { add_mix(MixKind::Source); })
		->setToolTip(T("Dock.AddSourceMix.Hint"));
	add_menu->setToolTipsVisible(true);
	add->setMenu(add_menu);

	auto *header = new QHBoxLayout();
	header->addWidget(title, 1);
	header->addWidget(add);

	rows_layout_ = new QVBoxLayout();
	rows_layout_->setSpacing(2);

	empty_label_ = new QLabel(T("Dock.NoMixes"));
	empty_label_->setWordWrap(true);
	set_class(empty_label_, QStringLiteral("text-muted"));

	scene_label_ = new QLabel();
	scene_label_->setToolTip(T("Dock.ItemHint"));

	tree_ = new QTreeWidget();
	tree_->setRootIsDecorated(true);
	tree_->setUniformRowHeights(true);
	tree_->setSelectionMode(QAbstractItemView::NoSelection);
	tree_->setToolTip(T("Dock.ItemHint"));
	tree_->header()->setStretchLastSection(false);
	connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
		if (col < 1 || (size_t)col > columns_.size())
			return;
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx < 0 || (size_t)idx >= tree_items_.size())
			return;
		set_hidden(tree_items_[idx], 1u << columns_[col - 1], it->checkState(col) != Qt::Checked);
	});

	preview_ = new MixPreview();
	preview_->hide();

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->addLayout(header);
	layout->addLayout(rows_layout_);
	layout->addWidget(empty_label_);
	layout->addSpacing(4);
	layout->addWidget(scene_label_);
	layout->addWidget(tree_, 1);
	layout->addWidget(preview_, 1);
	layout->addStretch(0); /* keeps content top-aligned while the tree/preview are hidden */

	status_timer_.setInterval(1000);
	connect(&status_timer_, &QTimer::timeout, this, [this] { update_status(); });

	listener_ = Manager::get().subscribe([this] { on_manager_changed(); });
	obs_frontend_add_event_callback(&MixesDock::frontend_event, this);
	rename_signal_.Connect(obs_get_signal_handler(), "source_rename", &MixesDock::scene_signal, this);

	on_manager_changed();
	schedule_tree();
}

MixesDock::~MixesDock()
{
	obs_frontend_remove_event_callback(&MixesDock::frontend_event, this);
	Manager::get().unsubscribe(listener_);
	rename_signal_.Disconnect();
	release_obs_refs();
}

void MixesDock::frontend_event(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<MixesDock *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		self->schedule_tree();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		self->release_obs_refs();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		self->preview_->unbind();
		self->rename_signal_.Disconnect();
		self->release_obs_refs();
		break;
	default:
		break;
	}
}

/* Any thread. Coalesces bursts of scene changes into one rebuild on the UI thread. */
void MixesDock::scene_signal(void *data, calldata_t *)
{
	static_cast<MixesDock *>(data)->schedule_tree();
}

void MixesDock::schedule_tree()
{
	if (tree_pending_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			tree_pending_ = false;
			rebuild_tree();
		},
		Qt::QueuedConnection);
}

void MixesDock::release_obs_refs()
{
	scene_signals_.clear();
	tree_->clear();
	tree_items_.clear();
}

void MixesDock::on_manager_changed()
{
	const std::vector<MixState> states = Manager::get().states();

	std::vector<uint32_t> ids;
	std::vector<std::string> names;
	for (const MixState &s : states) {
		if (s.cfg.kind != MixKind::Program)
			continue; /* only program mixes hide items */
		ids.push_back(s.cfg.id);
		names.push_back(s.cfg.name);
	}
	if (ids != columns_ || names != column_names_) {
		columns_ = std::move(ids);
		column_names_ = std::move(names);
		schedule_tree();
	}

	rebuild_rows(states);
	preview_->sync();
	preview_->setVisible(preview_->mix_id() != 0);
}

void MixesDock::rebuild_rows(const std::vector<MixState> &states)
{
	/* deleteLater: the clicked button that caused this rebuild may still be on the call stack. */
	for (Row &row : rows_) {
		QWidget *w = row.dot->parentWidget();
		rows_layout_->removeWidget(w);
		w->hide();
		w->deleteLater();
	}
	rows_.clear();

	for (const MixState &s : states) {
		const uint32_t id = s.cfg.id;
		auto *w = new QWidget();
		auto *h = new QHBoxLayout(w);
		h->setContentsMargins(0, 0, 0, 0);

		auto *dot = new QLabel(QStringLiteral("●"));
		auto *name = new QLabel(Q(s.cfg.name));
		name->setTextFormat(Qt::PlainText);
		set_class(name, QStringLiteral("text-bold"));
		auto *status = new QLabel();
		status->setTextFormat(Qt::PlainText);

		auto *text = new QVBoxLayout();
		text->setSpacing(0);
		text->addWidget(name);
		text->addWidget(status);

		auto *rec = new QToolButton();
		rec->setAutoRaise(true);
		connect(rec, &QToolButton::clicked, this, [id] {
			if (Manager::get().recording(id))
				Manager::get().stop_recording(id);
			else
				Manager::get().start_recording(id, Trigger::Manual);
		});

		auto *pv = new QToolButton();
		pv->setText(T("Dock.Preview"));
		pv->setCheckable(true);
		pv->setAutoRaise(true);
		pv->setChecked(preview_->mix_id() == id);
		connect(pv, &QToolButton::clicked, this, [this, id] { toggle_preview(id); });

		auto *gear = new QToolButton();
		gear->setText(QStringLiteral("⚙"));
		gear->setToolTip(T("Dock.Settings"));
		gear->setAutoRaise(true);
		connect(gear, &QToolButton::clicked, this, [this, id] { open_settings(id); });

		h->addWidget(dot);
		h->addLayout(text, 1);
		h->addWidget(rec);
		h->addWidget(pv);
		h->addWidget(gear);
		rows_layout_->addWidget(w);
		rows_.push_back({id, dot, status, rec, pv});
	}

	empty_label_->setVisible(states.empty());
	scene_label_->setVisible(!columns_.empty());
	tree_->setVisible(!columns_.empty());
	update_status();
}

void MixesDock::update_status()
{
	const std::vector<MixState> states = Manager::get().states();
	const uint64_t now = os_gettime_ns();
	bool any_recording = false;

	for (Row &row : rows_) {
		auto it = std::find_if(states.begin(), states.end(), [&](const MixState &s) { return s.cfg.id == row.id; });
		if (it == states.end())
			continue;
		const MixState &s = *it;
		QString text = T(trigger_key(s.cfg.trigger));
		if (s.cfg.kind == MixKind::Source)
			text = T("Dock.SourceOf", Q(s.cfg.source_name)) + QStringLiteral(" · ") + text;
		const char *tone = "text-muted";

		if (s.recording) {
			any_recording = true;
			tone = s.paused ? "text-warning" : "text-danger";
			text = s.paused ? T("Dock.Paused") : format_duration(now - s.started_ns);
			if (s.fps > 0.0)
				text += QStringLiteral(" · ") + T("Dock.Fps", QString::number(s.fps, 'g', 4));
			if (s.dropped_frames > 0)
				text += QStringLiteral(" · ") + T("Dock.Dropped", QString::number(s.dropped_frames));
		} else if (!s.last_error.empty()) {
			tone = "text-danger";
			text = T("Dock.Error", Q(s.last_error));
		} else {
			text += QStringLiteral(" · ") + T("Dock.Idle");
		}

		set_class(row.dot, tone);
		set_class(row.status, QStringLiteral("text-small %1").arg(QLatin1String(tone)));
		row.status->setText(text);
		row.status->setToolTip(s.recording ? Q(s.path) : QString());
		row.rec->setText(s.recording ? T("Dock.StopRecord") : T("Dock.Record"));
	}

	if (any_recording && !status_timer_.isActive())
		status_timer_.start();
	else if (!any_recording)
		status_timer_.stop();
}

void MixesDock::rebuild_tree()
{
	const QSignalBlocker block(tree_);
	release_obs_refs();

	QStringList headers{T("Dock.Source")};
	for (const std::string &n : column_names_)
		headers << Q(n);
	tree_->setColumnCount((int)headers.size());
	tree_->setHeaderLabels(headers);
	tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int c = 1; c < headers.size(); c++)
		tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);

	OBSSourceAutoRelease scene_source = obs_frontend_preview_program_mode_active()
						    ? obs_frontend_get_current_preview_scene()
						    : obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene) {
		scene_label_->setText(T("Dock.NoScene"));
		return;
	}
	scene_label_->setText(T("Dock.Visibility", QString::fromUtf8(obs_source_get_name(scene_source))));
	add_scene(scene, nullptr, 0);
	tree_->expandAll();

	/* Only reserve the expand-arrow gutter when something can be expanded. */
	bool nested = false;
	for (int i = 0; i < tree_->topLevelItemCount() && !nested; i++)
		nested = tree_->topLevelItem(i)->childCount() > 0;
	tree_->setRootIsDecorated(nested);
}

/* Lists items top-most first like the OBS Sources dock; groups and nested scenes become subtrees. */
void MixesDock::add_scene(obs_scene_t *scene, QTreeWidgetItem *parent, int depth)
{
	signal_handler_t *sh = obs_source_get_signal_handler(obs_scene_get_source(scene));
	for (const char *sig : {"item_add", "item_remove", "reorder", "refresh"})
		scene_signals_.emplace_back(sh, sig, &MixesDock::scene_signal, this);

	std::vector<OBSSceneItem> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		},
		&items);

	for (auto it = items.rbegin(); it != items.rend(); ++it) {
		obs_sceneitem_t *item = *it;
		obs_source_t *source = obs_sceneitem_get_source(item);
		const uint32_t mask = hidden_mask(item);

		auto *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
		node->setText(0, QString::fromUtf8(obs_source_get_name(source)));
		node->setData(0, Qt::UserRole, (int)tree_items_.size());
		node->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		for (size_t c = 0; c < columns_.size(); c++)
			node->setCheckState((int)c + 1, (mask & (1u << columns_[c])) ? Qt::Unchecked : Qt::Checked);
		tree_items_.push_back(*it);

		if (depth < kMaxTreeDepth) {
			obs_scene_t *child = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
									  : obs_scene_from_source(source);
			if (child)
				add_scene(child, node, depth + 1);
		}
	}
}

void MixesDock::toggle_preview(uint32_t id)
{
	if (preview_->mix_id() == id)
		preview_->unbind();
	else
		preview_->bind(id);
	preview_->setVisible(preview_->mix_id() != 0);
	for (Row &row : rows_)
		row.preview->setChecked(preview_->mix_id() == row.id);
}

void MixesDock::open_settings(uint32_t id)
{
	const MixConfig *current = Manager::get().find(id);
	if (!current)
		return;
	MixConfig cfg = *current;
	switch (edit_mix_settings(this, cfg, Manager::get().recording(id))) {
	case SettingsResult::Saved:
		Manager::get().update_mix(cfg);
		break;
	case SettingsResult::Deleted:
		if (preview_->mix_id() == id)
			preview_->unbind();
		Manager::get().remove_mix(id);
		break;
	case SettingsResult::Cancelled:
		break;
	}
}

void MixesDock::add_mix(MixKind kind)
{
	const char *name = kind == MixKind::Source ? "Dock.NewSourceMixName" : "Dock.NewMixName";
	const uint32_t id = Manager::get().add_mix(T(name).toStdString(), kind);
	if (!id) {
		QMessageBox::information(this, T("Dock.AddMix"), T("Dock.LimitReached"));
		return;
	}
	open_settings(id);
}

} // namespace

void create_dock()
{
	auto *dock = new MixesDock();
	if (!obs_frontend_add_dock_by_id("video-mixes", obs_module_text("Dock.Title"), dock))
		delete dock;
}

} // namespace vmix::ui
