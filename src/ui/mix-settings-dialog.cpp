#include "ui/mix-settings-dialog.hpp"
#include "core/encoders.hpp"
#include "core/frame-rate.hpp"
#include "core/item-path.hpp"
#include "core/obs-recording.hpp"
#include "core/util.hpp"
#include "ui/common.hpp"
#include "ui/properties-form.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <array>

namespace vmix::ui {

namespace {

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

constexpr int kNameRole = Qt::UserRole + 1;

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
		list->push_back({scene ? T("Settings.SceneLabel", name) : name,
				 QString::fromUtf8(obs_source_get_uuid(src)), name});
		return true;
	};
	obs_enum_sources(add, &out);
	obs_enum_scenes(add, &out);
	std::sort(out.begin(), out.end(), [](const SourceChoice &a, const SourceChoice &b) {
		return QString::localeAwareCompare(a.label, b.label) < 0;
	});
	return out;
}

QString source_display_name(const QComboBox *box)
{
	const QString name = box->currentData(kNameRole).toString();
	return name.isEmpty() ? box->currentText() : name;
}

} // namespace

SettingsResult edit_mix_settings(QWidget *parent, MixConfig &cfg, bool recording)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(T("Settings.Title", Q(cfg.name)));
	dlg.setMinimumWidth(480);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	const bool source_mix = cfg.kind == MixKind::Source;

	auto *name = new QLineEdit(Q(cfg.name));
	name->setMaxLength(64);
	form->addRow(T("Settings.Name"), name);

	QComboBox *source = nullptr;
	if (source_mix) {
		source = new QComboBox();
		for (const SourceChoice &s : video_sources()) {
			source->addItem(s.label, s.uuid);
			source->setItemData(source->count() - 1, s.name, kNameRole);
		}
		const int idx = source->findData(Q(cfg.source_uuid));
		if (idx >= 0) {
			source->setCurrentIndex(idx);
		} else if (!cfg.source_uuid.empty()) {
			source->insertItem(0, T("Settings.SourceMissing", Q(cfg.source_name)), Q(cfg.source_uuid));
			source->setCurrentIndex(0);
		} else {
			source->setPlaceholderText(T("Settings.ChooseSource"));
			source->setCurrentIndex(-1);
		}
		/* Name follows the source until the user types a name of their own. */
		QObject::connect(source, &QComboBox::currentIndexChanged, &dlg, [&, source, name](int) {
			const QString picked = source_display_name(source);
			if (name->text().trimmed().isEmpty() || name->text() == Q(cfg.name) ||
			    name->property("auto").toBool()) {
				name->setText(picked);
				name->setProperty("auto", true);
			}
		});
		QObject::connect(name, &QLineEdit::textEdited, &dlg, [name] { name->setProperty("auto", false); });
		form->addRow(T("Settings.Source"), source);
	}

	auto *trigger = new QComboBox();
	add_choice(trigger, "Settings.Start.Recording", Trigger::Recording);
	add_choice(trigger, "Settings.Start.Streaming", Trigger::Streaming);
	add_choice(trigger, "Settings.Start.Manual", Trigger::Manual);
	select(trigger, cfg.trigger);
	form->addRow(T("Settings.Start"), trigger);

	auto *transitions = new QComboBox();
	add_choice(transitions, "Settings.Transitions.Fade", TransitionStyle::Fade);
	add_choice(transitions, "Settings.Transitions.Cut", TransitionStyle::Cut);
	select(transitions, cfg.transitions);
	transitions->setToolTip(T("Settings.Transitions.Hint"));
	if (source_mix) {
		/* A source mix renders one source: scene transitions do not apply. */
		transitions->setParent(&dlg);
		transitions->hide();
	} else {
		form->addRow(T("Settings.Transitions"), transitions);
	}

	/* --- output (locked while recording) --- */

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	auto *resolution = new QComboBox();
	resolution->setEditable(true);
	if (source_mix)
		resolution->addItem(QString(), QString()); /* text set by refresh_source_size() */
	else
		resolution->addItem(T("Settings.SameAsOutput") +
					    QStringLiteral(" (%1x%2)").arg(ovi.output_width).arg(ovi.output_height),
				    QString());
	QStringList presets{QStringLiteral("3840x2160"), QStringLiteral("2560x1440"), QStringLiteral("1920x1080"),
			    QStringLiteral("1280x720")};
	if (!source_mix)
		presets.prepend(QStringLiteral("%1x%2").arg(ovi.base_width).arg(ovi.base_height));
	for (const QString &r : presets)
		if (resolution->findData(r) < 0)
			resolution->addItem(r, r);
	if (cfg.width && cfg.height) {
		const QString cur = QStringLiteral("%1x%2").arg(cfg.width).arg(cfg.height);
		const int idx = resolution->findData(cur);
		if (idx >= 0)
			resolution->setCurrentIndex(idx);
		else
			resolution->setEditText(cur);
	} else {
		resolution->setCurrentIndex(0);
	}
	form->addRow(T("Settings.Resolution"), resolution);

	/* Source mixes: show the size the selected source actually occupies on screen. */
	auto refresh_source_size = [resolution, source] {
		if (!source)
			return;
		const std::string uuid = source->currentData().toString().toStdString();
		OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid.c_str());
		PlacedItem placed;
		QString text;
		if (src && place_item(find_item_path(uuid), placed))
			text = T("Settings.SceneSize", QStringLiteral("%1x%2").arg(placed.width()).arg(placed.height()));
		else if (src)
			text = T("Settings.NativeSize", QStringLiteral("%1x%2")
								.arg(obs_source_get_width(src))
								.arg(obs_source_get_height(src)));
		else
			text = T("Settings.SceneSize", QStringLiteral("—"));
		const bool editing_default = resolution->currentIndex() == 0;
		resolution->setItemText(0, text);
		if (editing_default)
			resolution->setEditText(text);
	};
	refresh_source_size();
	if (source)
		QObject::connect(source, &QComboBox::currentIndexChanged, &dlg,
				 [refresh_source_size](int) { refresh_source_size(); });

	/* Frame rate: OBS fps / divisor. Auto resolves per source and says where the value comes from. */
	const double out_fps = obs_fps();
	auto fps_label = [](double fps) { return QString::number(fps, 'g', 4); };
	auto *frame_rate = new QComboBox();
	frame_rate->addItem(QString(), 0u);
	for (uint32_t d = 1; d <= 6; d++) {
		const double f = out_fps / d;
		if (d > 1 && f < 10.0)
			break;
		if (source_mix || d > 1)
			frame_rate->addItem(T("Settings.Fps.Value", fps_label(f)), d);
	}
	frame_rate->setCurrentIndex(std::max(0, frame_rate->findData(cfg.fps_divisor)));
	auto refresh_auto_fps = [&, frame_rate, source, source_mix, fps_label] {
		MixConfig probe = cfg;
		probe.fps_divisor = 0;
		if (source)
			probe.source_uuid = source->currentData().toString().toStdString();
		OBSSourceAutoRelease src = source_mix ? obs_get_source_by_uuid(probe.source_uuid.c_str()) : nullptr;
		const FrameRateChoice auto_rate = resolve_frame_rate(probe, src);
		const char *why = nullptr;
		if (!source_mix)
			why = "Settings.Fps.Program";
		else if (src && auto_rate.basis == FpsBasis::SourceRate)
			why = "Settings.Fps.FromSource";
		else if (src && auto_rate.basis == FpsBasis::EveryFrame)
			why = "Settings.Fps.EveryFrame";
		else if (src)
			why = "Settings.Fps.Unknown";
		frame_rate->setItemText(0, T("Settings.Fps.Auto", fps_label(auto_rate.fps)) +
						   (why ? QStringLiteral(" — ") + T(why) : QString()));
	};
	refresh_auto_fps();
	if (source)
		QObject::connect(source, &QComboBox::currentIndexChanged, &dlg, [refresh_auto_fps](int) { refresh_auto_fps(); });
	form->addRow(T("Settings.FrameRate"), frame_rate);

	auto *container = new QComboBox();
	add_choice(container, "Settings.Format.Mkv", Container::Mkv);
	add_choice(container, "Settings.Format.Mp4", Container::Mp4);
	add_choice(container, "Settings.Format.Mov", Container::Mov);
	add_choice(container, "Settings.Format.HybridMp4", Container::HybridMp4);
	add_choice(container, "Settings.Format.HybridMov", Container::HybridMov);
	select(container, cfg.container);
	form->addRow(T("Settings.Format"), container);

	auto *folder = new QLineEdit(Q(cfg.folder));
	folder->setPlaceholderText(T("Settings.Folder.Placeholder"));
	auto *browse = new QPushButton(T("Settings.Browse"));
	auto *folder_row = new QHBoxLayout();
	folder_row->addWidget(folder, 1);
	folder_row->addWidget(browse);
	QObject::connect(browse, &QPushButton::clicked, &dlg, [&] {
		const QString dir = QFileDialog::getExistingDirectory(&dlg, T("Settings.Folder"), folder->text());
		if (!dir.isEmpty())
			folder->setText(dir);
	});
	form->addRow(T("Settings.Folder"), folder_row);

	OBSData encoder_settings = new_data();
	obs_data_apply(encoder_settings, cfg.encoder_settings);
	auto *encoder = new QComboBox();
	for (const EncoderType &e : video_encoders())
		encoder->addItem(Q(e.name), Q(e.id));
	encoder->setCurrentIndex(std::max(0, encoder->findData(Q(cfg.encoder_id))));
	auto *encoder_button = new QPushButton(T("Settings.EncoderSettings"));
	auto *encoder_row = new QHBoxLayout();
	encoder_row->addWidget(encoder, 1);
	encoder_row->addWidget(encoder_button);
	auto *cbr_warning = new QLabel(T("Settings.CbrWarning"));
	cbr_warning->setWordWrap(true);
	cbr_warning->setProperty("class", QStringLiteral("text-warning text-small"));
	/* A mix that deliberately matches the OBS recording (same bitrate everywhere) is not warned about. */
	MixConfig obs_recording = cfg;
	const bool have_obs_recording = inherit_obs_recording(obs_recording);
	auto update_warning = [&] {
		const QByteArray id = encoder->currentData().toString().toUtf8();
		const bool matches_obs = have_obs_recording && obs_recording.encoder_id == id.constData() &&
					 same_settings(obs_recording.encoder_settings, encoder_settings);
		cbr_warning->setVisible(!matches_obs && uses_constant_bitrate(id.constData(), encoder_settings));
	};
	QObject::connect(encoder, &QComboBox::currentIndexChanged, &dlg, [&](int) {
		/* Settings keys differ per encoder: start from recording defaults for the new one. */
		encoder_settings = adopt(recording_defaults(encoder->currentData().toString().toUtf8().constData()));
		update_warning();
	});
	QObject::connect(encoder_button, &QPushButton::clicked, &dlg, [&] {
		const QByteArray id = encoder->currentData().toString().toUtf8();
		PropertiesDialog props(&dlg, T("Encoder.Title", encoder->currentText()), encoder_settings,
				       [id] { return obs_get_encoder_properties(id.constData()); });
		if (props.exec() == QDialog::Accepted) {
			encoder_settings = new_data();
			obs_data_apply(encoder_settings, props.settings());
			update_warning();
		}
	});
	form->addRow(T("Settings.Encoder"), encoder_row);
	form->addRow(QString(), cbr_warning);
	update_warning();

	auto *tracks_row = new QHBoxLayout();
	std::array<QCheckBox *, kAudioTracks> tracks{};
	for (int i = 0; i < kAudioTracks; i++) {
		tracks[i] = new QCheckBox(QString::number(i + 1));
		tracks[i]->setChecked(cfg.audio_tracks & (1u << i));
		tracks_row->addWidget(tracks[i]);
	}
	tracks_row->addStretch();
	form->addRow(T("Settings.AudioTracks"), tracks_row);

	auto *bitrate = new QComboBox();
	for (int kbps : {96, 128, 160, 192, 256, 320})
		bitrate->addItem(QStringLiteral("%1 kbps").arg(kbps), kbps);
	auto select_bitrate = [bitrate](int kbps) {
		if (bitrate->findData(kbps) < 0)
			bitrate->addItem(QStringLiteral("%1 kbps").arg(kbps), kbps);
		bitrate->setCurrentIndex(bitrate->findData(kbps));
	};
	select_bitrate(cfg.audio_bitrate);
	form->addRow(T("Settings.AudioBitrate"), bitrate);

	/* Re-sync with Settings → Output → Recording (new mixes start from it automatically). */
	auto *copy_obs = new QPushButton(T("Settings.CopyObs"));
	copy_obs->setToolTip(T("Settings.CopyObs.Hint"));
	QObject::connect(copy_obs, &QPushButton::clicked, &dlg, [&, select_bitrate] {
		MixConfig inherited = cfg;
		inherited.name = name->text().toStdString();
		if (!inherit_obs_recording(inherited))
			return;
		{
			const QSignalBlocker block(encoder); /* keep the inherited settings, not encoder defaults */
			encoder->setCurrentIndex(std::max(0, encoder->findData(Q(inherited.encoder_id))));
		}
		encoder_settings = inherited.encoder_settings;
		select(container, inherited.container);
		for (int i = 0; i < kAudioTracks; i++)
			tracks[i]->setChecked(inherited.audio_tracks & (1u << i));
		select_bitrate(inherited.audio_bitrate);
		update_warning();
	});
	form->addRow(QString(), copy_obs);

	for (QWidget *w : std::initializer_list<QWidget *>{resolution, frame_rate, container, folder, browse, encoder,
							    encoder_button, bitrate, copy_obs})
		w->setEnabled(!recording);
	for (QCheckBox *c : tracks)
		c->setEnabled(!recording);
	if (source)
		source->setEnabled(!recording);

	/* --- buttons --- */

	SettingsResult result = SettingsResult::Cancelled;
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	use_obs_button_texts(buttons);
	QPushButton *del = buttons->addButton(T("Settings.Delete"), QDialogButtonBox::DestructiveRole);
	del->setEnabled(!recording);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	QObject::connect(del, &QPushButton::clicked, &dlg, [&] {
		if (QMessageBox::question(&dlg, T("Settings.Delete"), T("Settings.DeleteConfirm", Q(cfg.name))) ==
		    QMessageBox::Yes) {
			result = SettingsResult::Deleted;
			dlg.accept();
		}
	});
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
		uint32_t w = 0, h = 0;
		const QString res = resolution->currentIndex() == 0 && resolution->currentText() == resolution->itemText(0)
					    ? QString()
					    : resolution->currentText().trimmed();
		if (!res.isEmpty()) {
			const auto m = QRegularExpression(QStringLiteral("^(\\d{2,5})\\s*[xX×]\\s*(\\d{2,5})$")).match(res);
			if (!m.hasMatch()) {
				QMessageBox::warning(&dlg, T("Settings.Resolution"), T("Settings.InvalidResolution"));
				return;
			}
			w = m.captured(1).toUInt();
			h = m.captured(2).toUInt();
		}
		if (source && source->currentIndex() < 0) {
			QMessageBox::warning(&dlg, T("Settings.Source"), T("Settings.PickSource"));
			return;
		}

		if (source && !recording) {
			cfg.source_uuid = source->currentData().toString().toStdString();
			cfg.source_name = source_display_name(source).toStdString();
		}
		const QString new_name = name->text().trimmed();
		cfg.name = new_name.isEmpty() ? cfg.name : new_name.toStdString();
		cfg.trigger = selected<Trigger>(trigger);
		cfg.transitions = selected<TransitionStyle>(transitions);
		if (!recording) {
			cfg.fps_divisor = frame_rate->currentData().toUInt();
			cfg.width = w;
			cfg.height = h;
			cfg.container = selected<Container>(container);
			cfg.folder = folder->text().trimmed().toStdString();
			cfg.encoder_id = encoder->currentData().toString().toStdString();
			cfg.encoder_settings = encoder_settings;
			uint32_t mask = 0;
			for (int i = 0; i < kAudioTracks; i++)
				if (tracks[i]->isChecked())
					mask |= 1u << i;
			cfg.audio_tracks = mask ? mask : 1u;
			cfg.audio_bitrate = bitrate->currentData().toInt();
		}
		result = SettingsResult::Saved;
		dlg.accept();
	});

	auto *layout = new QVBoxLayout(&dlg);
	if (recording) {
		auto *note = new QLabel(T("Settings.Recording"));
		note->setWordWrap(true);
		note->setProperty("class", QStringLiteral("text-warning"));
		layout->addWidget(note);
	}
	layout->addLayout(form);
	layout->addWidget(buttons);

	dlg.exec();
	return result;
}

} // namespace vmix::ui
