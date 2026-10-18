#include "ui/mix-settings-dialog.hpp"
#include "core/encoders.hpp"
#include "core/frame-rate.hpp"
#include "core/item-path.hpp"
#include "core/manager.hpp"
#include "core/obs-recording.hpp"
#include "core/util.hpp"
#include "ui/common.hpp"
#include "ui/properties-form.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>

namespace vmix::ui {

namespace {

constexpr int kNameRole = Qt::UserRole + 1;

template<typename E> void add_choice(QComboBox *box, const char *key, E value)
{
	box->addItem(T(key), (int)value);
}

template<typename E> void select(QComboBox *box, E value)
{
	box->setCurrentIndex(std::max(0, box->findData((int)value)));
}

template<typename E> E selected(const QComboBox *box)
{
	return (E)box->currentData().toInt();
}

QComboBox *trigger_box(bool allow_replay_buffer)
{
	auto *box = new QComboBox();
	add_choice(box, "Settings.Start.Recording", Trigger::Recording);
	add_choice(box, "Settings.Start.Streaming", Trigger::Streaming);
	if (allow_replay_buffer)
		add_choice(box, "Settings.Start.ReplayBuffer", Trigger::ReplayBuffer);
	add_choice(box, "Settings.Start.Manual", Trigger::Manual);
	return box;
}

struct SourceChoice {
	QString label;
	QString uuid;
	QString name;
};

/* Inputs with video plus scenes, sorted by name: anything a source mix can record on its own. */
std::vector<SourceChoice> video_sources()
{
	std::vector<SourceChoice> out;
	auto add = [](void *param, obs_source_t *src) {
		auto *list = static_cast<std::vector<SourceChoice> *>(param);
		const bool scene = obs_source_is_scene(src);
		if (!scene && !(obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
			return true;
		const QString name = QString::fromUtf8(obs_source_get_name(src));
		list->push_back({scene ? T("Settings.SceneLabel", name) : name, QString::fromUtf8(obs_source_get_uuid(src)),
				 name});
		return true;
	};
	obs_enum_sources(add, &out);
	obs_enum_scenes(add, &out);
	std::sort(out.begin(), out.end(), [](const SourceChoice &a, const SourceChoice &b) {
		return QString::localeAwareCompare(a.label, b.label) < 0;
	});
	return out;
}

void fill_encoders(QComboBox *box, const std::string &current)
{
	for (const EncoderType &e : video_encoders())
		box->addItem(e.hardware ? T("Settings.EncoderGpu", Q(e.name)) : Q(e.name), Q(e.id));
	box->setCurrentIndex(std::max(0, box->findData(Q(current))));
}

QComboBox *bitrate_box(int kbps)
{
	auto *box = new QComboBox();
	for (int v : {96, 128, 160, 192, 256, 320})
		box->addItem(QStringLiteral("%1 kbps").arg(v), v);
	if (box->findData(kbps) < 0)
		box->addItem(QStringLiteral("%1 kbps").arg(kbps), kbps);
	box->setCurrentIndex(box->findData(kbps));
	return box;
}

void select_bitrate(QComboBox *box, int kbps)
{
	if (box->findData(kbps) < 0)
		box->addItem(QStringLiteral("%1 kbps").arg(kbps), kbps);
	box->setCurrentIndex(box->findData(kbps));
}

/* A page: title, optional enable switch, then a form. */
struct Page {
	QWidget *widget = nullptr;
	QFormLayout *form = nullptr;
	QVBoxLayout *layout = nullptr;
};

Page make_page(const QString &title, const QString &description)
{
	Page p;
	p.widget = new QWidget();
	p.layout = new QVBoxLayout(p.widget);
	p.layout->setContentsMargins(16, 12, 16, 12);
	p.layout->setSpacing(8);
	p.layout->addWidget(label(title, QStringLiteral("text-title")));
	if (!description.isEmpty())
		p.layout->addWidget(hint(description));
	p.form = new QFormLayout();
	p.form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	p.form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	p.layout->addLayout(p.form);
	return p;
}

QWidget *scrollable(QWidget *page)
{
	auto *scroll = new QScrollArea();
	scroll->setWidget(page);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	return scroll;
}

QWidget *row(std::initializer_list<QWidget *> widgets, int stretch_index = 0)
{
	auto *w = new QWidget();
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 0, 0, 0);
	int i = 0;
	for (QWidget *child : widgets)
		h->addWidget(child, i++ == stretch_index ? 1 : 0);
	if (stretch_index < 0)
		h->addStretch(1);
	return w;
}

/* Encoder settings button: edits `settings` with the encoder's own property UI. */
void edit_encoder_settings(QWidget *parent, QComboBox *encoder, OBSData &settings, const std::function<void()> &done)
{
	const QByteArray id = encoder->currentData().toString().toUtf8();
	PropertiesDialog props(parent, T("Encoder.Title", encoder->currentText()), settings,
			       [id] { return obs_get_encoder_properties(id.constData()); });
	if (props.exec() == QDialog::Accepted) {
		settings = new_data();
		obs_data_apply(settings, props.settings());
		done();
	}
}

} // namespace

class MixSettingsDialog : public QDialog {
public:
	MixSettingsDialog(QWidget *parent, MixConfig &cfg, const MixState *state);

	SettingsResult result_ = SettingsResult::Cancelled;

	int page_count() const { return nav_->count(); }
	void show_page(int index) { nav_->setCurrentRow(index); }

private:
	void build_general();
	void build_recording();
	void build_audio();
	void build_replay();
	void build_stream();
	void apply_locks();
	void update_enabled();
	void update_cbr_warning();
	void refresh_source_size();
	void refresh_auto_fps();
	void refresh_stream_bitrate();
	bool validate_and_store();

	bool active(OutputKind k) const { return state_ && state_->out(k).active; }
	bool any_active() const { return state_ && state_->any_active(); }

	MixConfig &cfg_;
	const MixState *state_;
	const bool source_mix_;

	QListWidget *nav_ = nullptr;
	QStackedWidget *pages_ = nullptr;

	/* General */
	QLineEdit *name_ = nullptr;
	QComboBox *source_ = nullptr;
	QComboBox *resolution_ = nullptr;
	QComboBox *frame_rate_ = nullptr;
	QComboBox *transitions_ = nullptr;

	/* Recording */
	QCheckBox *record_ = nullptr;
	QComboBox *trigger_ = nullptr;
	QComboBox *container_ = nullptr;
	QLineEdit *folder_ = nullptr;
	QPushButton *browse_ = nullptr;
	QComboBox *encoder_ = nullptr;
	QPushButton *encoder_button_ = nullptr;
	QLabel *cbr_warning_ = nullptr;
	QPushButton *copy_obs_ = nullptr;
	OBSData encoder_settings_;
	MixConfig obs_recording_;
	bool have_obs_recording_ = false;
	QWidget *recording_fields_ = nullptr;

	/* Audio */
	QRadioButton *audio_source_ = nullptr;
	QRadioButton *audio_tracks_ = nullptr;
	std::array<QCheckBox *, kAudioTracks> tracks_{};
	QComboBox *audio_bitrate_ = nullptr;
	QWidget *audio_fields_ = nullptr;

	/* Replay */
	QCheckBox *replay_ = nullptr;
	QComboBox *replay_trigger_ = nullptr;
	QSpinBox *replay_seconds_ = nullptr;
	QSpinBox *replay_mb_ = nullptr;
	QCheckBox *replay_with_obs_ = nullptr;
	QWidget *replay_fields_ = nullptr;

	/* Stream */
	QCheckBox *stream_ = nullptr;
	QComboBox *stream_trigger_ = nullptr;
	QLineEdit *server_ = nullptr;
	QLineEdit *key_ = nullptr;
	QComboBox *stream_encoder_ = nullptr;
	QPushButton *stream_encoder_button_ = nullptr;
	QSpinBox *stream_bitrate_ = nullptr;
	QComboBox *stream_track_ = nullptr;
	QComboBox *stream_audio_bitrate_ = nullptr;
	QPushButton *stream_copy_obs_ = nullptr;
	OBSData stream_settings_;
	QWidget *stream_fields_ = nullptr;
};

MixSettingsDialog::MixSettingsDialog(QWidget *parent, MixConfig &cfg, const MixState *state)
	: QDialog(parent), cfg_(cfg), state_(state), source_mix_(cfg.kind == MixKind::Source)
{
	setWindowTitle(state ? T("Settings.Title", Q(cfg.name)) : T("Settings.NewTitle"));
	setMinimumSize(720, 540);

	encoder_settings_ = new_data();
	obs_data_apply(encoder_settings_, cfg.encoder_settings);
	stream_settings_ = new_data();
	obs_data_apply(stream_settings_, cfg.stream_encoder_settings);
	obs_recording_ = cfg;
	have_obs_recording_ = inherit_obs_recording(obs_recording_);

	nav_ = new QListWidget();
	nav_->setFixedWidth(170);
	nav_->setFrameShape(QFrame::NoFrame);
	pages_ = new QStackedWidget();

	build_general();
	build_recording();
	build_audio();
	build_replay();
	build_stream();

	connect(nav_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
	nav_->setCurrentRow(0);

	/* Buttons */
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	use_obs_button_texts(buttons);
	if (!state)
		buttons->button(QDialogButtonBox::Save)->setText(T("Settings.AddMix"));
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		if (validate_and_store()) {
			result_ = SettingsResult::Saved;
			accept();
		}
	});

	auto *bottom = new QHBoxLayout();
	if (state) {
		auto *del = new QPushButton(T("Settings.Delete"));
		del->setEnabled(!any_active());
		del->setToolTip(any_active() ? T("Settings.DeleteLocked") : QString());
		connect(del, &QPushButton::clicked, this, [this] {
			if (QMessageBox::question(this, T("Settings.Delete"), T("Settings.DeleteConfirm", Q(cfg_.name))) ==
			    QMessageBox::Yes) {
				result_ = SettingsResult::Deleted;
				accept();
			}
		});
		bottom->addWidget(del);
	}
	bottom->addStretch(1);
	bottom->addWidget(buttons);

	auto *body = new QHBoxLayout();
	body->setSpacing(0);
	body->addWidget(nav_);
	body->addWidget(pages_, 1);

	auto *layout = new QVBoxLayout(this);
	if (any_active()) {
		auto *note = hint(T("Settings.Locked"));
		set_class(note, QStringLiteral("text-warning"));
		layout->addWidget(note);
	}
	layout->addLayout(body, 1);
	layout->addLayout(bottom);

	apply_locks();
	update_enabled();
}

void MixSettingsDialog::build_general()
{
	Page p = make_page(T("Settings.Page.General"), source_mix_ ? T("Settings.SourceMix.Hint")
								    : T("Settings.ProgramMix.Hint"));

	name_ = new QLineEdit(Q(cfg_.name));
	name_->setMaxLength(64);
	p.form->addRow(T("Settings.Name"), name_);

	if (source_mix_) {
		source_ = new QComboBox();
		for (const SourceChoice &s : video_sources()) {
			source_->addItem(s.label, s.uuid);
			source_->setItemData(source_->count() - 1, s.name, kNameRole);
		}
		const int idx = source_->findData(Q(cfg_.source_uuid));
		if (idx >= 0) {
			source_->setCurrentIndex(idx);
		} else if (!cfg_.source_uuid.empty()) {
			source_->insertItem(0, T("Settings.SourceMissing", Q(cfg_.source_name)), Q(cfg_.source_uuid));
			source_->setCurrentIndex(0);
		} else {
			source_->setPlaceholderText(T("Settings.ChooseSource"));
			source_->setCurrentIndex(-1);
		}
		/* The name follows the source until the user types a name of their own. */
		name_->setProperty("auto", cfg_.source_uuid.empty());
		connect(source_, &QComboBox::currentIndexChanged, this, [this](int) {
			if (name_->property("auto").toBool() || name_->text().trimmed().isEmpty())
				name_->setText(source_->currentData(kNameRole).toString());
			refresh_source_size();
			refresh_auto_fps();
		});
		connect(name_, &QLineEdit::textEdited, this, [this] { name_->setProperty("auto", false); });
		p.form->addRow(T("Settings.Source"), source_);
	}

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	resolution_ = new QComboBox();
	resolution_->setEditable(true);
	resolution_->setInsertPolicy(QComboBox::NoInsert);
	if (source_mix_)
		resolution_->addItem(QString(), QString()); /* text set by refresh_source_size() */
	else
		resolution_->addItem(T("Settings.SameAsOutput", QStringLiteral("%1x%2").arg(ovi.output_width).arg(ovi.output_height)),
				     QString());
	QStringList presets{QStringLiteral("3840x2160"), QStringLiteral("2560x1440"), QStringLiteral("1920x1080"),
			    QStringLiteral("1280x720"), QStringLiteral("854x480")};
	if (!source_mix_)
		presets.prepend(QStringLiteral("%1x%2").arg(ovi.base_width).arg(ovi.base_height));
	for (const QString &r : presets)
		if (resolution_->findData(r) < 0)
			resolution_->addItem(r, r);
	if (cfg_.width && cfg_.height) {
		const QString cur = QStringLiteral("%1x%2").arg(cfg_.width).arg(cfg_.height);
		const int idx = resolution_->findData(cur);
		if (idx >= 0)
			resolution_->setCurrentIndex(idx);
		else
			resolution_->setEditText(cur);
	} else {
		resolution_->setCurrentIndex(0);
	}
	resolution_->setToolTip(source_mix_ ? T("Settings.Resolution.SourceHint") : T("Settings.Resolution.ProgramHint"));
	p.form->addRow(T("Settings.Resolution"), resolution_);
	refresh_source_size();

	/* Frame rate: OBS fps / divisor. Auto resolves per source and says where the value comes from. */
	const double out_fps = obs_fps();
	frame_rate_ = new QComboBox();
	frame_rate_->addItem(QString(), 0u);
	for (uint32_t d = 1; d <= 6; d++) {
		const double f = out_fps / d;
		if (d > 1 && f < 10.0)
			break;
		if (source_mix_ || d > 1)
			frame_rate_->addItem(T("Settings.Fps.Value", QString::number(f, 'g', 4)), d);
	}
	frame_rate_->setCurrentIndex(std::max(0, frame_rate_->findData(cfg_.fps_divisor)));
	p.form->addRow(T("Settings.FrameRate"), frame_rate_);
	refresh_auto_fps();

	transitions_ = new QComboBox();
	add_choice(transitions_, "Settings.Transitions.Fade", TransitionStyle::Fade);
	add_choice(transitions_, "Settings.Transitions.Cut", TransitionStyle::Cut);
	select(transitions_, cfg_.transitions);
	if (!source_mix_) {
		p.form->addRow(T("Settings.Transitions"), transitions_);
		p.form->addRow(QString(), hint(T("Settings.Transitions.Hint")));
	} else {
		transitions_->setParent(p.widget);
		transitions_->hide();
	}

	p.layout->addStretch(1);
	nav_->addItem(T("Settings.Page.General"));
	pages_->addWidget(scrollable(p.widget));
}

void MixSettingsDialog::build_recording()
{
	Page p = make_page(T("Settings.Page.Recording"), T("Settings.Recording.Hint"));

	record_ = new QCheckBox(T("Settings.Record.Enable"));
	record_->setChecked(cfg_.record);
	connect(record_, &QCheckBox::toggled, this, [this] { update_enabled(); });
	p.layout->insertWidget(2, record_);

	recording_fields_ = new QWidget();
	auto *form = new QFormLayout(recording_fields_);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	p.layout->addWidget(recording_fields_);

	trigger_ = trigger_box(true);
	select(trigger_, cfg_.trigger);
	form->addRow(T("Settings.Start"), trigger_);

	container_ = new QComboBox();
	add_choice(container_, "Settings.Format.Mkv", Container::Mkv);
	add_choice(container_, "Settings.Format.HybridMp4", Container::HybridMp4);
	add_choice(container_, "Settings.Format.Mp4", Container::Mp4);
	add_choice(container_, "Settings.Format.HybridMov", Container::HybridMov);
	add_choice(container_, "Settings.Format.Mov", Container::Mov);
	select(container_, cfg_.container);
	form->addRow(T("Settings.Format"), container_);

	folder_ = new QLineEdit(Q(cfg_.folder));
	folder_->setPlaceholderText(T("Settings.Folder.Placeholder"));
	browse_ = new QPushButton(T("Settings.Browse"));
	connect(browse_, &QPushButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(this, T("Settings.Folder"), folder_->text());
		if (!dir.isEmpty())
			folder_->setText(dir);
	});
	form->addRow(T("Settings.Folder"), row({folder_, browse_}));

	encoder_ = new QComboBox();
	fill_encoders(encoder_, cfg_.encoder_id);
	encoder_button_ = new QPushButton(T("Settings.EncoderSettings"));
	connect(encoder_, &QComboBox::currentIndexChanged, this, [this](int) {
		/* Settings keys differ per encoder: start from recording defaults for the new one. */
		encoder_settings_ = adopt(recording_defaults(encoder_->currentData().toString().toUtf8().constData()));
		update_cbr_warning();
	});
	connect(encoder_button_, &QPushButton::clicked, this,
		[this] { edit_encoder_settings(this, encoder_, encoder_settings_, [this] { update_cbr_warning(); }); });
	form->addRow(T("Settings.Encoder"), row({encoder_, encoder_button_}));

	cbr_warning_ = hint(T("Settings.CbrWarning"));
	set_class(cbr_warning_, QStringLiteral("text-warning text-small"));
	form->addRow(QString(), cbr_warning_);
	update_cbr_warning();

	/* Re-sync with Settings → Output → Recording (new mixes start from it automatically). */
	copy_obs_ = new QPushButton(T("Settings.CopyObs"));
	copy_obs_->setToolTip(T("Settings.CopyObs.Hint"));
	copy_obs_->setEnabled(have_obs_recording_);
	connect(copy_obs_, &QPushButton::clicked, this, [this] {
		MixConfig inherited = cfg_;
		if (!inherit_obs_recording(inherited))
			return;
		{
			const QSignalBlocker block(encoder_); /* keep the inherited settings, not encoder defaults */
			encoder_->setCurrentIndex(std::max(0, encoder_->findData(Q(inherited.encoder_id))));
		}
		encoder_settings_ = inherited.encoder_settings;
		select(container_, inherited.container);
		for (int i = 0; i < kAudioTracks; i++)
			tracks_[i]->setChecked(inherited.audio_tracks & (1u << i));
		select_bitrate(audio_bitrate_, inherited.audio_bitrate);
		update_cbr_warning();
	});
	form->addRow(QString(), row({copy_obs_}, -1));

	p.layout->addStretch(1);
	nav_->addItem(T("Settings.Page.Recording"));
	pages_->addWidget(scrollable(p.widget));
}

void MixSettingsDialog::build_audio()
{
	Page p = make_page(T("Settings.Page.Audio"), T("Settings.Audio.Hint"));

	if (source_mix_) {
		audio_source_ = new QRadioButton(T("Settings.Audio.SourceOnly"));
		audio_tracks_ = new QRadioButton(T("Settings.Audio.Tracks"));
		auto *group = new QButtonGroup(p.widget);
		group->addButton(audio_source_);
		group->addButton(audio_tracks_);
		(cfg_.audio_source == AudioSource::Source ? audio_source_ : audio_tracks_)->setChecked(true);
		connect(audio_tracks_, &QRadioButton::toggled, this, [this] { update_enabled(); });
		p.form->addRow(T("Settings.Audio.Record"), audio_source_);
		p.form->addRow(QString(), audio_tracks_);
	}

	audio_fields_ = new QWidget();
	auto *tracks_row = new QHBoxLayout(audio_fields_);
	tracks_row->setContentsMargins(0, 0, 0, 0);
	for (int i = 0; i < kAudioTracks; i++) {
		tracks_[i] = new QCheckBox(QString::number(i + 1));
		tracks_[i]->setChecked(cfg_.audio_tracks & (1u << i));
		tracks_row->addWidget(tracks_[i]);
	}
	tracks_row->addStretch();
	p.form->addRow(T("Settings.AudioTracks"), audio_fields_);

	audio_bitrate_ = bitrate_box(cfg_.audio_bitrate);
	p.form->addRow(T("Settings.AudioBitrate"), audio_bitrate_);
	p.form->addRow(QString(), hint(T("Settings.Audio.AlertsTip")));

	p.layout->addStretch(1);
	nav_->addItem(T("Settings.Page.Audio"));
	pages_->addWidget(scrollable(p.widget));
}

void MixSettingsDialog::build_replay()
{
	Page p = make_page(T("Settings.Page.Replay"), T("Settings.Replay.Hint"));

	replay_ = new QCheckBox(T("Settings.Replay.Enable"));
	replay_->setChecked(cfg_.replay);
	connect(replay_, &QCheckBox::toggled, this, [this] { update_enabled(); });
	p.layout->insertWidget(2, replay_);

	replay_fields_ = new QWidget();
	auto *form = new QFormLayout(replay_fields_);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	p.layout->addWidget(replay_fields_);

	replay_trigger_ = trigger_box(true);
	select(replay_trigger_, cfg_.replay_trigger);
	form->addRow(T("Settings.Start"), replay_trigger_);

	replay_seconds_ = new QSpinBox();
	replay_seconds_->setRange(5, 21600);
	replay_seconds_->setSuffix(QStringLiteral(" s"));
	replay_seconds_->setValue(cfg_.replay_seconds);
	form->addRow(T("Settings.Replay.Duration"), replay_seconds_);

	replay_mb_ = new QSpinBox();
	replay_mb_->setRange(16, 32768);
	replay_mb_->setSingleStep(64);
	replay_mb_->setSuffix(QStringLiteral(" MB"));
	replay_mb_->setValue(cfg_.replay_mb);
	replay_mb_->setToolTip(T("Settings.Replay.Memory.Hint"));
	form->addRow(T("Settings.Replay.Memory"), replay_mb_);

	replay_with_obs_ = new QCheckBox(T("Settings.Replay.WithObs"));
	replay_with_obs_->setChecked(cfg_.replay_save_with_obs);
	form->addRow(QString(), replay_with_obs_);
	form->addRow(QString(), hint(T("Settings.Replay.HotkeyTip")));

	p.layout->addStretch(1);
	nav_->addItem(T("Settings.Page.Replay"));
	pages_->addWidget(scrollable(p.widget));
}

void MixSettingsDialog::build_stream()
{
	Page p = make_page(T("Settings.Page.Stream"), T("Settings.Stream.Hint"));

	stream_ = new QCheckBox(T("Settings.Stream.Enable"));
	stream_->setChecked(cfg_.stream);
	connect(stream_, &QCheckBox::toggled, this, [this] { update_enabled(); });
	p.layout->insertWidget(2, stream_);

	stream_fields_ = new QWidget();
	auto *form = new QFormLayout(stream_fields_);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	p.layout->addWidget(stream_fields_);

	stream_trigger_ = trigger_box(false);
	select(stream_trigger_, cfg_.stream_trigger);
	form->addRow(T("Settings.Start"), stream_trigger_);

	server_ = new QLineEdit(Q(cfg_.stream_server));
	server_->setPlaceholderText(QStringLiteral("rtmp://live.example.com/app"));
	form->addRow(T("Settings.Stream.Server"), server_);

	key_ = new QLineEdit(Q(cfg_.stream_key));
	key_->setEchoMode(QLineEdit::Password);
	auto *show = new QPushButton(T("Settings.Stream.ShowKey"));
	show->setCheckable(true);
	connect(show, &QPushButton::toggled, this, [this, show](bool on) {
		key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		show->setText(T(on ? "Settings.Stream.HideKey" : "Settings.Stream.ShowKey"));
	});
	form->addRow(T("Settings.Stream.Key"), row({key_, show}));

	stream_encoder_ = new QComboBox();
	fill_encoders(stream_encoder_, cfg_.stream_encoder_id);
	stream_encoder_button_ = new QPushButton(T("Settings.EncoderSettings"));
	connect(stream_encoder_, &QComboBox::currentIndexChanged, this, [this](int) {
		const int kbps = stream_bitrate_->value();
		stream_settings_ = adopt(stream_defaults(stream_encoder_->currentData().toString().toUtf8().constData(), kbps));
		refresh_stream_bitrate();
	});
	connect(stream_encoder_button_, &QPushButton::clicked, this, [this] {
		edit_encoder_settings(this, stream_encoder_, stream_settings_, [this] { refresh_stream_bitrate(); });
	});
	form->addRow(T("Settings.Encoder"), row({stream_encoder_, stream_encoder_button_}));

	stream_bitrate_ = new QSpinBox();
	stream_bitrate_->setRange(200, 100000);
	stream_bitrate_->setSingleStep(500);
	stream_bitrate_->setSuffix(QStringLiteral(" kbps"));
	connect(stream_bitrate_, &QSpinBox::valueChanged, this,
		[this](int v) { obs_data_set_int(stream_settings_, "bitrate", v); });
	form->addRow(T("Settings.Stream.Bitrate"), stream_bitrate_);
	refresh_stream_bitrate();

	stream_track_ = new QComboBox();
	for (int i = 1; i <= kAudioTracks; i++)
		stream_track_->addItem(T("Settings.Stream.Track", QString::number(i)), i);
	stream_track_->setCurrentIndex(std::max(0, stream_track_->findData(cfg_.stream_audio_track)));
	stream_track_->setToolTip(T("Settings.Stream.Track.Hint"));
	form->addRow(T("Settings.Stream.AudioTrack"), stream_track_);

	stream_audio_bitrate_ = bitrate_box(cfg_.stream_audio_bitrate);
	form->addRow(T("Settings.AudioBitrate"), stream_audio_bitrate_);

	stream_copy_obs_ = new QPushButton(T("Settings.Stream.CopyObs"));
	stream_copy_obs_->setToolTip(T("Settings.Stream.CopyObs.Hint"));
	connect(stream_copy_obs_, &QPushButton::clicked, this, [this] {
		MixConfig inherited = cfg_;
		if (!inherit_obs_stream(inherited))
			return;
		{
			const QSignalBlocker block(stream_encoder_);
			stream_encoder_->setCurrentIndex(std::max(0, stream_encoder_->findData(Q(inherited.stream_encoder_id))));
		}
		stream_settings_ = inherited.stream_encoder_settings;
		refresh_stream_bitrate();
		stream_track_->setCurrentIndex(std::max(0, stream_track_->findData(inherited.stream_audio_track)));
		select_bitrate(stream_audio_bitrate_, inherited.stream_audio_bitrate);
	});
	form->addRow(QString(), row({stream_copy_obs_}, -1));

	p.layout->addStretch(1);
	nav_->addItem(T("Settings.Page.Stream"));
	pages_->addWidget(scrollable(p.widget));
}

/* Running outputs keep the settings they started with: lock what they use. */
void MixSettingsDialog::apply_locks()
{
	const bool files = active(OutputKind::Record) || active(OutputKind::Replay);
	const bool stream = active(OutputKind::Stream);
	for (QWidget *w : std::initializer_list<QWidget *>{resolution_, frame_rate_})
		w->setEnabled(!any_active());
	if (source_)
		source_->setEnabled(!any_active());
	for (QWidget *w :
	     std::initializer_list<QWidget *>{container_, folder_, browse_, encoder_, encoder_button_, copy_obs_, audio_bitrate_})
		w->setEnabled(!files && (w != copy_obs_ || have_obs_recording_));
	for (QCheckBox *c : tracks_)
		c->setEnabled(!files);
	if (audio_source_) {
		audio_source_->setEnabled(!any_active());
		audio_tracks_->setEnabled(!any_active());
	}
	record_->setEnabled(!active(OutputKind::Record));
	replay_->setEnabled(!active(OutputKind::Replay));
	for (QWidget *w : std::initializer_list<QWidget *>{replay_seconds_, replay_mb_})
		w->setEnabled(!active(OutputKind::Replay));
	stream_->setEnabled(!stream);
	for (QWidget *w : std::initializer_list<QWidget *>{server_, key_, stream_encoder_, stream_encoder_button_, stream_bitrate_,
							    stream_track_, stream_audio_bitrate_, stream_copy_obs_})
		w->setEnabled(!stream);
}

void MixSettingsDialog::update_enabled()
{
	/* Disabled outputs keep their fields visible (greyed), so what each output offers stays discoverable. */
	recording_fields_->setEnabled(record_->isChecked());
	replay_fields_->setEnabled(replay_->isChecked());
	stream_fields_->setEnabled(stream_->isChecked());
	audio_fields_->setEnabled(!audio_source_ || audio_tracks_->isChecked());
}

void MixSettingsDialog::update_cbr_warning()
{
	/* A mix that deliberately matches the OBS recording (same bitrate everywhere) is not warned about. */
	const QByteArray id = encoder_->currentData().toString().toUtf8();
	const bool matches_obs = have_obs_recording_ && obs_recording_.encoder_id == id.constData() &&
				 same_settings(obs_recording_.encoder_settings, encoder_settings_);
	cbr_warning_->setVisible(!matches_obs && uses_constant_bitrate(id.constData(), encoder_settings_));
}

void MixSettingsDialog::refresh_stream_bitrate()
{
	const QSignalBlocker block(stream_bitrate_);
	const QByteArray id = stream_encoder_->currentData().toString().toUtf8();
	OBSDataAutoRelease effective = obs_encoder_defaults(id.constData());
	if (effective)
		obs_data_apply(effective, stream_settings_);
	const int kbps = effective ? (int)obs_data_get_int(effective, "bitrate") : 0;
	stream_bitrate_->setValue(kbps > 0 ? kbps : 6000);
	if (kbps <= 0)
		obs_data_set_int(stream_settings_, "bitrate", stream_bitrate_->value());
}

/* Source mixes: show the size the selected source actually occupies on screen. */
void MixSettingsDialog::refresh_source_size()
{
	if (!source_ || !resolution_)
		return;
	const std::string uuid = source_->currentData().toString().toStdString();
	OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid.c_str());
	PlacedItem placed;
	QString text;
	if (src && place_item(find_item_path(uuid), placed))
		text = T("Settings.SceneSize", QStringLiteral("%1x%2").arg(placed.width()).arg(placed.height()));
	else if (src)
		text = T("Settings.NativeSize",
			 QStringLiteral("%1x%2").arg(obs_source_get_width(src)).arg(obs_source_get_height(src)));
	else
		text = T("Settings.SceneSize", QStringLiteral("—"));
	const bool editing_default = resolution_->currentIndex() == 0;
	resolution_->setItemText(0, text);
	if (editing_default)
		resolution_->setEditText(text);
}

void MixSettingsDialog::refresh_auto_fps()
{
	if (!frame_rate_)
		return;
	MixConfig probe = cfg_;
	probe.fps_divisor = 0;
	if (source_)
		probe.source_uuid = source_->currentData().toString().toStdString();
	OBSSourceAutoRelease src = source_mix_ ? obs_get_source_by_uuid(probe.source_uuid.c_str()) : nullptr;
	const FrameRateChoice auto_rate = resolve_frame_rate(probe, src);
	const char *why = nullptr;
	if (!source_mix_)
		why = "Settings.Fps.Program";
	else if (src && auto_rate.basis == FpsBasis::SourceRate)
		why = "Settings.Fps.FromSource";
	else if (src && auto_rate.basis == FpsBasis::EveryFrame)
		why = "Settings.Fps.EveryFrame";
	else if (src)
		why = "Settings.Fps.Unknown";
	frame_rate_->setItemText(0, T("Settings.Fps.Auto", QString::number(auto_rate.fps, 'g', 4)) +
					    (why ? QStringLiteral(" — ") + T(why) : QString()));
}

bool MixSettingsDialog::validate_and_store()
{
	auto fail = [this](int page, QWidget *focus, const QString &message) {
		nav_->setCurrentRow(page);
		QMessageBox::warning(this, windowTitle(), message);
		if (focus)
			focus->setFocus();
		return false;
	};

	const QString name = name_->text().trimmed();
	if (name.isEmpty())
		return fail(0, name_, T("Settings.NameRequired"));
	for (const MixState &other : Manager::get().states())
		if (other.cfg.id != cfg_.id && Q(other.cfg.name).compare(name, Qt::CaseInsensitive) == 0)
			return fail(0, name_, T("Settings.NameTaken", name));
	if (source_ && source_->currentIndex() < 0)
		return fail(0, source_, T("Settings.PickSource"));

	uint32_t w = 0, h = 0;
	const QString res = resolution_->currentIndex() == 0 && resolution_->currentText() == resolution_->itemText(0)
				    ? QString()
				    : resolution_->currentText().trimmed();
	if (!res.isEmpty()) {
		const auto m = QRegularExpression(QStringLiteral("^(\\d{2,5})\\s*[xX×]\\s*(\\d{2,5})$")).match(res);
		if (!m.hasMatch())
			return fail(0, resolution_, T("Settings.InvalidResolution"));
		w = m.captured(1).toUInt();
		h = m.captured(2).toUInt();
		if (w < 16 || h < 16 || w > 16384 || h > 16384)
			return fail(0, resolution_, T("Settings.InvalidResolution"));
	}
	if (stream_->isChecked() && server_->text().trimmed().isEmpty())
		return fail(4, server_, T("Settings.Stream.ServerRequired"));

	cfg_.name = name.toStdString();
	if (source_ && !any_active()) {
		cfg_.source_uuid = source_->currentData().toString().toStdString();
		cfg_.source_name = source_->currentData(kNameRole).toString().toStdString();
		if (cfg_.source_name.empty())
			cfg_.source_name = source_->currentText().toStdString();
	}
	if (!any_active()) {
		cfg_.width = w;
		cfg_.height = h;
		cfg_.fps_divisor = frame_rate_->currentData().toUInt();
	}
	cfg_.transitions = selected<TransitionStyle>(transitions_);

	cfg_.record = record_->isChecked();
	cfg_.trigger = selected<Trigger>(trigger_);
	if (container_->isEnabled()) {
		cfg_.container = selected<Container>(container_);
		cfg_.folder = folder_->text().trimmed().toStdString();
		cfg_.encoder_id = encoder_->currentData().toString().toStdString();
		cfg_.encoder_settings = encoder_settings_;
		uint32_t mask = 0;
		for (int i = 0; i < kAudioTracks; i++)
			if (tracks_[i]->isChecked())
				mask |= 1u << i;
		cfg_.audio_tracks = mask ? mask : 1u;
		cfg_.audio_bitrate = audio_bitrate_->currentData().toInt();
	}
	if (audio_source_ && audio_source_->isEnabled())
		cfg_.audio_source = audio_source_->isChecked() ? AudioSource::Source : AudioSource::Tracks;

	cfg_.replay = replay_->isChecked();
	cfg_.replay_trigger = selected<Trigger>(replay_trigger_);
	if (replay_seconds_->isEnabled()) {
		cfg_.replay_seconds = replay_seconds_->value();
		cfg_.replay_mb = replay_mb_->value();
	}
	cfg_.replay_save_with_obs = replay_with_obs_->isChecked();

	cfg_.stream = stream_->isChecked();
	cfg_.stream_trigger = selected<Trigger>(stream_trigger_);
	if (server_->isEnabled()) {
		cfg_.stream_server = server_->text().trimmed().toStdString();
		cfg_.stream_key = key_->text().trimmed().toStdString();
		cfg_.stream_encoder_id = stream_encoder_->currentData().toString().toStdString();
		cfg_.stream_encoder_settings = stream_settings_;
		cfg_.stream_audio_track = stream_track_->currentData().toInt();
		cfg_.stream_audio_bitrate = stream_audio_bitrate_->currentData().toInt();
	}
	return true;
}

SettingsResult edit_mix_settings(QWidget *parent, MixConfig &cfg, const MixState *state)
{
	MixSettingsDialog dlg(parent, cfg, state);
	dlg.exec();
	return dlg.result_;
}

#ifdef VMIX_DEV_SNAPSHOTS
void snapshot_settings(QWidget *parent, const MixConfig &cfg, const MixState *state, const QString &prefix)
{
	MixConfig copy = cfg;
	MixSettingsDialog dlg(parent, copy, state);
	dlg.show();
	for (int i = 0; i < dlg.page_count(); i++) {
		dlg.show_page(i);
		QCoreApplication::processEvents();
		dlg.grab().save(prefix + QStringLiteral("-%1.png").arg(i));
	}
	dlg.close();
}
#endif

} // namespace vmix::ui
