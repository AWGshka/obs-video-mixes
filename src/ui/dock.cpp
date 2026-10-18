#include "ui/dock.hpp"
#include "core/manager.hpp"
#include "ui/common.hpp"
#include "ui/mix-settings-dialog.hpp"
#include "ui/preview.hpp"
#include "ui/visibility-tree.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <QDesktopServices>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <vector>

namespace vmix::ui {

namespace {

constexpr std::array kKinds = {OutputKind::Record, OutputKind::Replay, OutputKind::Stream};

QString format_duration(uint64_t ns)
{
	const uint64_t s = ns / 1000000000ULL;
	return QStringLiteral("%1:%2:%3")
		.arg(s / 3600, 2, 10, QLatin1Char('0'))
		.arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString format_bitrate(double kbps)
{
	return kbps >= 1000.0 ? T("Dock.Mbps", QString::number(kbps / 1000.0, 'f', 1))
			      : T("Dock.Kbps", QString::number((int)kbps));
}

bool output_enabled(const MixConfig &cfg, OutputKind kind)
{
	return kind == OutputKind::Record ? cfg.record : kind == OutputKind::Replay ? cfg.replay : cfg.stream;
}

Trigger output_trigger(const MixConfig &cfg, OutputKind kind)
{
	return kind == OutputKind::Record ? cfg.trigger : kind == OutputKind::Replay ? cfg.replay_trigger : cfg.stream_trigger;
}

const char *output_title_key(OutputKind kind)
{
	return kind == OutputKind::Record ? "Output.Record" : kind == OutputKind::Replay ? "Output.Replay" : "Output.Stream";
}

QString mix_folder(const MixConfig &cfg)
{
	if (!cfg.folder.empty())
		return Q(cfg.folder);
	BPtr<char> dir = obs_frontend_get_current_record_output_path();
	return dir ? QString::fromUtf8(dir.Get()) : QString();
}

/* One output of a mix: status dot, name, live status, start/stop (and save for the replay buffer). */
class OutputRow : public QWidget {
public:
	OutputRow(uint32_t id, OutputKind kind) : id_(id), kind_(kind)
	{
		dot_ = label(QStringLiteral("●"), QStringLiteral("text-muted"));
		title_ = label(T(output_title_key(kind)));
		status_ = label(QString(), QStringLiteral("text-muted text-small"));
		status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

		toggle_ = icon_button("icon-media-play", QString());
		connect(toggle_, &QToolButton::clicked, this, [this] {
			Manager &m = Manager::get();
			if (m.output_active(id_, kind_))
				m.stop_output(id_, kind_);
			else
				m.start_output(id_, kind_);
		});

		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(6);
		layout->addWidget(dot_);
		layout->addWidget(title_);
		layout->addWidget(status_, 1);
		if (kind == OutputKind::Replay) {
			save_ = icon_button("icon-save", T("Dock.SaveReplay"));
			connect(save_, &QToolButton::clicked, this, [this] { Manager::get().save_replay(id_); });
			layout->addWidget(save_);
		}
		layout->addWidget(toggle_);
	}

	/* kbps: measured stream bitrate, < 0 if unknown. */
	void update(const MixState &s, uint64_t now, double kbps)
	{
		const OutputState &o = s.out(kind_);
		QString text;
		QString tip;
		const char *dot = "text-muted";    /* idle */
		const char *status = "text-muted"; /* normal; warning/danger when something needs attention */

		if (o.active) {
			dot = kind_ == OutputKind::Record ? "text-danger" : kind_ == OutputKind::Stream ? "text-success" : "text-bright";
			const QString elapsed = format_duration(now - o.started_ns);
			switch (kind_) {
			case OutputKind::Record:
				text = o.paused ? T("Dock.Paused") : elapsed;
				if (o.paused)
					dot = status = "text-warning";
				break;
			case OutputKind::Replay:
				text = T("Dock.Buffering", QString::number(s.cfg.replay_seconds));
				break;
			case OutputKind::Stream:
				text = elapsed;
				if (kbps >= 0.0)
					text += QStringLiteral(" · ") + format_bitrate(kbps);
				if (o.reconnecting) {
					text = T("Dock.Reconnecting");
					dot = status = "text-warning";
				} else if (o.congestion > 0.5f) {
					status = "text-warning";
				}
				break;
			}
			if (kind_ != OutputKind::Replay && s.fps > 0.0)
				text += QStringLiteral(" · ") + T("Dock.Fps", QString::number(s.fps, 'g', 4));
			if (o.dropped_frames > 0) {
				text += QStringLiteral(" · ") + T("Dock.Dropped", QString::number(o.dropped_frames));
				status = "text-warning";
			}
			tip = Q(o.path);
		} else if (!o.error.empty()) {
			dot = status = "text-danger";
			text = Q(o.error);
			tip = text;
		} else if (kind_ == OutputKind::Stream && s.cfg.stream_server.empty()) {
			text = T("Dock.NoServer");
		} else {
			text = T(trigger_key(output_trigger(s.cfg, kind_)));
		}
		if (kind_ == OutputKind::Replay && !o.path.empty())
			tip = T("Dock.LastReplay", Q(o.path));

		set_class(dot_, QLatin1String(dot));
		set_class(status_, QStringLiteral("text-small %1").arg(QLatin1String(status)));
		status_->setText(text);
		status_->setToolTip(tip);

		const bool active = o.active;
		set_class(toggle_, QStringLiteral("btn-tool ") +
					   (active ? QStringLiteral("icon-media-stop") : QStringLiteral("icon-media-play")));
		const QString action = T(active ? "Dock.Stop" : "Dock.Start", T(output_title_key(kind_)));
		toggle_->setToolTip(action);
		toggle_->setAccessibleName(action);
		if (save_)
			save_->setVisible(active);
	}

private:
	uint32_t id_;
	OutputKind kind_;
	QLabel *dot_ = nullptr;
	QLabel *title_ = nullptr;
	QLabel *status_ = nullptr;
	QToolButton *toggle_ = nullptr;
	QToolButton *save_ = nullptr;
};

class MixesDock;

/* A mix: name and kind, preview/settings/menu buttons, and one row per enabled output. */
class MixCard : public QFrame {
public:
	MixCard(MixesDock *dock, const MixState &s);

	uint32_t id() const { return id_; }
	void update(const MixState &s, uint64_t now, const std::array<double, kOutputKinds> &kbps);
	void set_previewing(bool on) { preview_->setChecked(on); }

protected:
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
	void build_rows(const MixConfig &cfg);
	void fill_menu(QMenu *menu);

	MixesDock *dock_;
	uint32_t id_;
	QLabel *name_ = nullptr;
	QLabel *subtitle_ = nullptr;
	QLabel *error_ = nullptr;
	QToolButton *preview_ = nullptr;
	QVBoxLayout *rows_layout_ = nullptr;
	std::map<OutputKind, OutputRow *> rows_;
	std::array<bool, kOutputKinds> enabled_{};
};

class MixesDock : public QWidget {
public:
	MixesDock();
	~MixesDock() override;

	void toggle_preview(uint32_t id);
	void open_settings(uint32_t id);
	void delete_mix(uint32_t id);
	void duplicate_mix(uint32_t id);

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		fit_cards();
	}

private:
	void on_manager_changed();
	void sync_cards(const std::vector<MixState> &states);
	void update_status();
	void update_tree_title();
	void fit_cards();
	void add_mix(MixKind kind);
	QMenu *add_menu(QWidget *parent);

	static void frontend_event(enum obs_frontend_event event, void *data);

	QWidget *cards_ = nullptr;
	QVBoxLayout *cards_layout_ = nullptr;
	QWidget *empty_ = nullptr;
	QWidget *tree_section_ = nullptr;
	QLabel *tree_title_ = nullptr;
	VisibilityTree *tree_ = nullptr;
	QWidget *preview_section_ = nullptr;
	QLabel *preview_title_ = nullptr;
	MixPreview *preview_ = nullptr;
	QTimer status_timer_;
	QSplitter *splitter_ = nullptr;
	QWidget *scroll_content_ = nullptr;
	bool user_sized_ = false;

	std::vector<MixCard *> cards_list_;
	struct Rate {
		uint64_t bytes = 0;
		uint64_t at = 0;
		double kbps = -1.0;
	};
	std::map<uint32_t, Rate> stream_rates_;
	int listener_ = 0;
};

/* ---- MixCard ---- */

MixCard::MixCard(MixesDock *dock, const MixState &s) : dock_(dock), id_(s.cfg.id)
{
	setProperty("class", QStringLiteral("dialog-frame"));
	setFrameShape(QFrame::NoFrame);

	name_ = label(QString(), QStringLiteral("text-bold"));
	name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	subtitle_ = label(QString(), QStringLiteral("text-muted text-small"));
	subtitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	error_ = label(QString(), QStringLiteral("text-danger text-small"));
	error_->setWordWrap(true);
	error_->hide();

	preview_ = new QToolButton();
	preview_->setText(T("Dock.Preview"));
	preview_->setToolTip(T("Dock.Preview.Hint"));
	preview_->setCheckable(true);
	preview_->setProperty("class", QStringLiteral("btn-tool"));
	connect(preview_, &QToolButton::clicked, this, [this] { dock_->toggle_preview(id_); });

	auto *settings = icon_button("icon-gear", T("Dock.Settings"));
	connect(settings, &QToolButton::clicked, this, [this] { dock_->open_settings(id_); });

	auto *more = icon_button("icon-dots-vert", T("Dock.More"));
	more->setPopupMode(QToolButton::InstantPopup);
	more->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
	auto *menu = new QMenu(more);
	/* Filled on open so the entries match the current state. */
	connect(menu, &QMenu::aboutToShow, this, [this, menu] { fill_menu(menu); });
	more->setMenu(menu);

	auto *text = new QVBoxLayout();
	text->setSpacing(0);
	text->addWidget(name_);
	text->addWidget(subtitle_);

	auto *header = new QHBoxLayout();
	header->setSpacing(4);
	header->addLayout(text, 1);
	header->addWidget(preview_);
	header->addWidget(settings);
	header->addWidget(more);

	rows_layout_ = new QVBoxLayout();
	rows_layout_->setSpacing(2);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 6, 6);
	layout->setSpacing(4);
	layout->addLayout(header);
	layout->addWidget(error_);
	layout->addLayout(rows_layout_);

	setToolTip(T("Dock.Card.Hint"));
	build_rows(s.cfg);
}

void MixCard::build_rows(const MixConfig &cfg)
{
	for (auto &[kind, row] : rows_) {
		rows_layout_->removeWidget(row);
		row->deleteLater(); /* its button may be what triggered this */
	}
	rows_.clear();
	for (OutputKind kind : kKinds) {
		enabled_[(size_t)kind] = output_enabled(cfg, kind);
		if (!enabled_[(size_t)kind])
			continue;
		auto *row = new OutputRow(id_, kind);
		rows_layout_->addWidget(row);
		rows_[kind] = row;
	}
}

void MixCard::update(const MixState &s, uint64_t now, const std::array<double, kOutputKinds> &kbps)
{
	std::array<bool, kOutputKinds> enabled{};
	for (OutputKind kind : kKinds)
		enabled[(size_t)kind] = output_enabled(s.cfg, kind);
	if (enabled != enabled_)
		build_rows(s.cfg);

	name_->setText(Q(s.cfg.name));
	name_->setToolTip(Q(s.cfg.name));
	QString subtitle;
	if (s.cfg.kind == MixKind::Source)
		subtitle = T("Dock.SourceMix", Q(s.cfg.source_name));
	else
		subtitle = T("Dock.ProgramMix");
	if (rows_.empty())
		subtitle += QStringLiteral(" · ") + T("Dock.NoOutputs");
	subtitle_->setText(subtitle);

	error_->setText(Q(s.error));
	error_->setVisible(!s.error.empty());

	for (auto &[kind, row] : rows_)
		row->update(s, now, kbps[(size_t)kind]);
}

void MixCard::mouseDoubleClickEvent(QMouseEvent *event)
{
	QFrame::mouseDoubleClickEvent(event);
	dock_->open_settings(id_);
}

void MixCard::fill_menu(QMenu *menu)
{
	menu->clear();
	Manager &m = Manager::get();
	const MixConfig *cfg = m.find(id_);
	if (!cfg)
		return;

	for (OutputKind kind : kKinds) {
		if (!output_enabled(*cfg, kind))
			continue;
		const bool active = m.output_active(id_, kind);
		const uint32_t id = id_;
		menu->addAction(T(active ? "Dock.Stop" : "Dock.Start", T(output_title_key(kind))), this, [id, kind, active] {
			if (active)
				Manager::get().stop_output(id, kind);
			else
				Manager::get().start_output(id, kind);
		});
		if (kind == OutputKind::Replay && active)
			menu->addAction(T("Dock.SaveReplay"), this, [id] { Manager::get().save_replay(id); });
	}
	if (!menu->isEmpty())
		menu->addSeparator();

	const QString folder = mix_folder(*cfg);
	QAction *open = menu->addAction(T("Dock.OpenFolder"), this,
					[folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
	open->setEnabled(!folder.isEmpty());
	menu->addAction(T("Dock.Duplicate"), this, [this] { dock_->duplicate_mix(id_); });
	menu->addSeparator();
	menu->addAction(T("Dock.Settings"), this, [this] { dock_->open_settings(id_); });
	QAction *del = menu->addAction(T("Dock.Delete"), this, [this] { dock_->delete_mix(id_); });
	del->setEnabled(!m.any_active(id_));
}

/* ---- MixesDock ---- */

MixesDock::MixesDock()
{
	setMinimumWidth(240);

	/* Mixes */
	auto *title = label(T("Dock.Mixes"), QStringLiteral("text-bold"));
	auto *add = icon_button("icon-plus", T("Dock.AddMix"));
	add->setPopupMode(QToolButton::InstantPopup);
	add->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
	add->setMenu(add_menu(add));

	auto *header = new QHBoxLayout();
	header->addWidget(title, 1);
	header->addWidget(add);

	cards_ = new QWidget();
	cards_layout_ = new QVBoxLayout(cards_);
	cards_layout_->setContentsMargins(0, 0, 0, 0);
	cards_layout_->setSpacing(0);

	/* Empty state: what a mix is, and the two ways to start. */
	empty_ = new QFrame();
	empty_->setProperty("class", QStringLiteral("dialog-frame"));
	{
		auto *v = new QVBoxLayout(empty_);
		v->setContentsMargins(10, 10, 10, 10);
		v->addWidget(label(T("Dock.Empty.Title"), QStringLiteral("text-bold")));
		v->addWidget(hint(T("Dock.Empty.Text")));
		auto *program = new QPushButton(T("Dock.AddProgramMix"));
		program->setToolTip(T("Dock.AddProgramMix.Hint"));
		connect(program, &QPushButton::clicked, this, [this] { add_mix(MixKind::Program); });
		auto *source = new QPushButton(T("Dock.AddSourceMix"));
		source->setToolTip(T("Dock.AddSourceMix.Hint"));
		connect(source, &QPushButton::clicked, this, [this] { add_mix(MixKind::Source); });
		v->addSpacing(4);
		v->addWidget(program);
		v->addWidget(source);
	}

	auto *top = new QWidget();
	auto *top_layout = new QVBoxLayout(top);
	top_layout->setContentsMargins(6, 6, 6, 4);
	top_layout->setSpacing(4);
	top_layout->addLayout(header);
	top_layout->addWidget(cards_);
	top_layout->addWidget(empty_);
	top_layout->addStretch(1);

	auto *scroll = new QScrollArea();
	scroll->setWidget(top);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	/* Visibility */
	tree_section_ = new QWidget();
	{
		tree_title_ = label(QString(), QStringLiteral("text-bold"));
		tree_title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		tree_ = new VisibilityTree();
		tree_->set_scene_changed_callback([this] { update_tree_title(); });
		auto *v = new QVBoxLayout(tree_section_);
		v->setContentsMargins(6, 4, 6, 4);
		v->setSpacing(4);
		v->addWidget(tree_title_);
		v->addWidget(hint(T("Visibility.Hint")));
		v->addWidget(tree_, 1);
	}

	/* Preview */
	preview_section_ = new QWidget();
	{
		preview_title_ = label(QString(), QStringLiteral("text-bold"));
		preview_title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		auto *close = icon_button("icon-close", T("Dock.ClosePreview"));
		connect(close, &QToolButton::clicked, this, [this] {
			if (preview_->mix_id())
				toggle_preview(preview_->mix_id());
		});
		auto *h = new QHBoxLayout();
		h->addWidget(preview_title_, 1);
		h->addWidget(close);
		preview_ = new MixPreview();
		auto *v = new QVBoxLayout(preview_section_);
		v->setContentsMargins(6, 4, 6, 6);
		v->setSpacing(4);
		v->addLayout(h);
		v->addWidget(preview_, 1);
	}
	preview_section_->hide();

	splitter_ = new QSplitter(Qt::Vertical);
	splitter_->setChildrenCollapsible(false);
	splitter_->addWidget(scroll);
	splitter_->addWidget(tree_section_);
	splitter_->addWidget(preview_section_);
	splitter_->setStretchFactor(0, 0);
	splitter_->setStretchFactor(1, 1);
	splitter_->setStretchFactor(2, 1);
	connect(splitter_, &QSplitter::splitterMoved, this, [this] { user_sized_ = true; });
	scroll_content_ = top;

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(splitter_);

	status_timer_.setInterval(1000);
	connect(&status_timer_, &QTimer::timeout, this, [this] { update_status(); });

	listener_ = Manager::get().subscribe([this] { on_manager_changed(); });
	obs_frontend_add_event_callback(&MixesDock::frontend_event, this);
	on_manager_changed();

#ifdef VMIX_DEV_SNAPSHOTS
	/* Developer aid: VMIX_SNAPSHOT_DIR=<dir> renders the dock and all settings pages once OBS has settled. */
	if (const char *dir = std::getenv("VMIX_SNAPSHOT_DIR")) {
		const QString out = QString::fromLocal8Bit(dir);
		QTimer::singleShot(6000, this, [this, out] {
			grab().save(out + QStringLiteral("/dock.png"));
			for (const MixState &s : Manager::get().states())
				snapshot_settings(this, s.cfg, &s, out + QStringLiteral("/settings-%1").arg(s.cfg.id));
			snapshot_settings(this, Manager::get().new_mix("Clean", MixKind::Program), nullptr,
					  out + QStringLiteral("/new-program"));
			snapshot_settings(this, Manager::get().new_mix("Source", MixKind::Source), nullptr,
					  out + QStringLiteral("/new-source"));
			QFile done(out + QStringLiteral("/done.txt"));
			done.open(QIODevice::WriteOnly);
		});
	}
#endif
}

MixesDock::~MixesDock()
{
	obs_frontend_remove_event_callback(&MixesDock::frontend_event, this);
	Manager::get().unsubscribe(listener_);
}

void MixesDock::frontend_event(enum obs_frontend_event event, void *data)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		static_cast<MixesDock *>(data)->preview_->unbind();
}

QMenu *MixesDock::add_menu(QWidget *parent)
{
	auto *menu = new QMenu(parent);
	menu->addAction(T("Dock.AddProgramMix"), this, [this] { add_mix(MixKind::Program); })
		->setToolTip(T("Dock.AddProgramMix.Hint"));
	menu->addAction(T("Dock.AddSourceMix"), this, [this] { add_mix(MixKind::Source); })
		->setToolTip(T("Dock.AddSourceMix.Hint"));
	menu->setToolTipsVisible(true);
	return menu;
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
	tree_->set_mixes(ids, names);
	tree_section_->setVisible(!ids.empty());

	sync_cards(states);
	empty_->setVisible(states.empty());

	preview_->sync();
	const MixConfig *previewed = preview_->mix_id() ? Manager::get().find(preview_->mix_id()) : nullptr;
	preview_section_->setVisible(previewed != nullptr);
	if (previewed)
		preview_title_->setText(T("Dock.PreviewOf", Q(previewed->name)));
	for (MixCard *card : cards_list_)
		card->set_previewing(card->id() == preview_->mix_id());
	update_tree_title();
	update_status();
	fit_cards();
}

/* Until the user drags the splitter, the mixes area takes the height of its cards, up to about half the dock. */
void MixesDock::fit_cards()
{
	if (user_sized_ || !isVisible())
		return;
	QTimer::singleShot(0, this, [this] {
		const QList<int> sizes = splitter_->sizes();
		int total = 0;
		for (int s : sizes)
			total += s;
		if (total <= 0)
			return;
		const bool lower_visible = tree_section_->isVisible() || preview_section_->isVisible();
		const int wanted = lower_visible ? std::min(scroll_content_->sizeHint().height() + 2, total * 55 / 100) : total;
		QList<int> next = sizes;
		next[0] = wanted;
		const int rest = total - wanted;
		const int lower = (tree_section_->isVisible() ? 1 : 0) + (preview_section_->isVisible() ? 1 : 0);
		next[1] = tree_section_->isVisible() ? rest / std::max(1, lower) : 0;
		next[2] = preview_section_->isVisible() ? rest - next[1] : 0;
		splitter_->setSizes(next);
	});
}

void MixesDock::update_tree_title()
{
	const QString scene = tree_->scene_name();
	tree_title_->setText(scene.isEmpty() ? T("Visibility.NoScene") : T("Visibility.Title", scene));
}

void MixesDock::sync_cards(const std::vector<MixState> &states)
{
	/* Keep cards (and their widgets) across updates; only add, remove and reorder. */
	std::vector<MixCard *> next;
	for (const MixState &s : states) {
		auto it = std::find_if(cards_list_.begin(), cards_list_.end(),
				       [&](MixCard *c) { return c->id() == s.cfg.id; });
		if (it != cards_list_.end()) {
			next.push_back(*it);
			cards_list_.erase(it);
		} else {
			next.push_back(new MixCard(this, s));
		}
	}
	for (MixCard *gone : cards_list_) {
		cards_layout_->removeWidget(gone);
		gone->hide();
		gone->deleteLater();
	}
	cards_list_ = std::move(next);
	for (size_t i = 0; i < cards_list_.size(); i++)
		cards_layout_->insertWidget((int)i, cards_list_[i]);
	std::erase_if(stream_rates_, [&](const auto &e) {
		return std::none_of(states.begin(), states.end(), [&](const MixState &s) { return s.cfg.id == e.first; });
	});
}

void MixesDock::update_status()
{
	const std::vector<MixState> states = Manager::get().states();
	const uint64_t now = os_gettime_ns();
	bool any_active = false;

	for (MixCard *card : cards_list_) {
		auto it = std::find_if(states.begin(), states.end(), [&](const MixState &s) { return s.cfg.id == card->id(); });
		if (it == states.end())
			continue;
		const MixState &s = *it;
		any_active = any_active || s.any_active();

		/* Stream bitrate from the byte counter, averaged over the last status interval. */
		std::array<double, kOutputKinds> kbps{-1.0, -1.0, -1.0};
		const OutputState &stream = s.out(OutputKind::Stream);
		Rate &rate = stream_rates_[s.cfg.id];
		if (stream.active) {
			if (rate.at && now > rate.at && stream.total_bytes >= rate.bytes && now - rate.at >= 500000000ULL) {
				rate.kbps = (double)(stream.total_bytes - rate.bytes) * 8.0 / ((double)(now - rate.at) / 1e6);
				rate.bytes = stream.total_bytes;
				rate.at = now;
			} else if (!rate.at) {
				rate = {stream.total_bytes, now, -1.0};
			}
			kbps[(size_t)OutputKind::Stream] = rate.kbps;
		} else {
			rate = {};
		}
		card->update(s, now, kbps);
	}

	if (any_active && !status_timer_.isActive())
		status_timer_.start();
	else if (!any_active)
		status_timer_.stop();
}

void MixesDock::toggle_preview(uint32_t id)
{
	if (preview_->mix_id() == id)
		preview_->unbind();
	else
		preview_->bind(id);
	on_manager_changed();
}

void MixesDock::open_settings(uint32_t id)
{
	const MixConfig *current = Manager::get().find(id);
	if (!current)
		return;
	MixConfig cfg = *current;
	const std::vector<MixState> states = Manager::get().states();
	auto it = std::find_if(states.begin(), states.end(), [&](const MixState &s) { return s.cfg.id == id; });
	switch (edit_mix_settings(this, cfg, it == states.end() ? nullptr : &*it)) {
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

void MixesDock::delete_mix(uint32_t id)
{
	const MixConfig *cfg = Manager::get().find(id);
	if (!cfg || Manager::get().any_active(id))
		return;
	if (QMessageBox::question(this, T("Dock.Delete"), T("Settings.DeleteConfirm", Q(cfg->name))) != QMessageBox::Yes)
		return;
	if (preview_->mix_id() == id)
		preview_->unbind();
	Manager::get().remove_mix(id);
}

void MixesDock::duplicate_mix(uint32_t id)
{
	const MixConfig *cfg = Manager::get().find(id);
	if (!cfg)
		return;
	if (!Manager::get().duplicate_mix(id, T("Dock.CopyName", Q(cfg->name)).toStdString()))
		QMessageBox::information(this, T("Dock.AddMix"), T("Dock.LimitReached"));
}

void MixesDock::add_mix(MixKind kind)
{
	const char *name = kind == MixKind::Source ? "Dock.NewSourceMixName" : "Dock.NewMixName";
	MixConfig cfg = Manager::get().new_mix(T(name).toStdString(), kind);
	if (edit_mix_settings(this, cfg, nullptr) != SettingsResult::Saved)
		return;
	if (!Manager::get().add_mix(std::move(cfg)))
		QMessageBox::information(this, T("Dock.AddMix"), T("Dock.LimitReached"));
}

} // namespace

void create_dock()
{
	auto *dock = new MixesDock();
	if (!obs_frontend_add_dock_by_id("video-mixes", obs_module_text("Dock.Title"), dock))
		delete dock;
}

} // namespace vmix::ui
