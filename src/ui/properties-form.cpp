#include "ui/properties-form.hpp"
#include "ui/common.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vmix::ui {

PropertiesDialog::PropertiesDialog(QWidget *parent, const QString &title, obs_data_t *settings, Factory factory)
	: QDialog(parent)
{
	setWindowTitle(title);
	setMinimumSize(460, 420);

	/* Edit a copy so Cancel leaves the caller's settings untouched. */
	OBSDataAutoRelease copy = obs_data_create();
	obs_data_apply(copy, settings);
	settings_ = copy.Get();

	props_ = factory();
	if (props_)
		obs_properties_apply_settings(props_, settings_);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	body_ = new QWidget(scroll);
	scroll->setWidget(body_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	use_obs_button_texts(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(scroll, 1);
	layout->addWidget(buttons);

	rebuild();
}

void PropertiesDialog::rebuild()
{
	rebuild_queued_ = false;
	if (QLayout *old = body_->layout()) {
		QLayoutItem *child;
		while ((child = old->takeAt(0))) {
			delete child->widget();
			delete child;
		}
		delete old;
	}
	for (QWidget *w : body_->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
		delete w;

	auto *form = new QFormLayout(body_);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	if (props_)
		add_properties(props_, form);
}

void PropertiesDialog::add_properties(obs_properties_t *props, QFormLayout *form)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
		add_property(p, form);
}

void PropertiesDialog::changed(obs_property_t *prop)
{
	/* Modified callbacks may add/remove/hide properties; rebuild after the emitting widget returns. */
	if (obs_property_modified(prop, settings_) && !rebuild_queued_) {
		rebuild_queued_ = true;
		QTimer::singleShot(0, this, [this] { rebuild(); });
	}
}

void PropertiesDialog::add_property(obs_property_t *prop, QFormLayout *form)
{
	if (!obs_property_visible(prop))
		return;

	const char *name = obs_property_name(prop);
	const QString label = QString::fromUtf8(obs_property_description(prop));
	const char *tip = obs_property_long_description(prop);
	QWidget *widget = nullptr;
	bool full_row = false;

	switch (obs_property_get_type(prop)) {
	case OBS_PROPERTY_BOOL: {
		auto *w = new QCheckBox(label);
		w->setChecked(obs_data_get_bool(settings_, name));
		connect(w, &QCheckBox::toggled, this, [this, prop, name](bool on) {
			obs_data_set_bool(settings_, name, on);
			changed(prop);
		});
		widget = w;
		full_row = true;
		break;
	}
	case OBS_PROPERTY_INT: {
		auto *w = new QSpinBox();
		w->setRange(obs_property_int_min(prop), obs_property_int_max(prop));
		w->setSingleStep(std::max(1, obs_property_int_step(prop)));
		w->setSuffix(QString::fromUtf8(obs_property_int_suffix(prop)));
		w->setValue((int)obs_data_get_int(settings_, name));
		connect(w, &QSpinBox::valueChanged, this, [this, prop, name](int v) {
			obs_data_set_int(settings_, name, v);
			changed(prop);
		});
		widget = w;
		break;
	}
	case OBS_PROPERTY_FLOAT: {
		auto *w = new QDoubleSpinBox();
		const double step = obs_property_float_step(prop);
		w->setRange(obs_property_float_min(prop), obs_property_float_max(prop));
		w->setSingleStep(step > 0.0 ? step : 0.1);
		w->setDecimals(step > 0.0 ? std::clamp((int)std::ceil(-std::log10(step)), 0, 6) : 2);
		w->setSuffix(QString::fromUtf8(obs_property_float_suffix(prop)));
		w->setValue(obs_data_get_double(settings_, name));
		connect(w, &QDoubleSpinBox::valueChanged, this, [this, prop, name](double v) {
			obs_data_set_double(settings_, name, v);
			changed(prop);
		});
		widget = w;
		break;
	}
	case OBS_PROPERTY_TEXT: {
		const obs_text_type type = obs_property_text_type(prop);
		if (type == OBS_TEXT_INFO) {
			auto *w = new QLabel(label);
			w->setWordWrap(true);
			widget = w;
			full_row = true;
		} else if (type == OBS_TEXT_MULTILINE) {
			auto *w = new QPlainTextEdit(QString::fromUtf8(obs_data_get_string(settings_, name)));
			w->setMaximumHeight(90);
			connect(w, &QPlainTextEdit::textChanged, this, [this, w, prop, name] {
				obs_data_set_string(settings_, name, w->toPlainText().toUtf8().constData());
				changed(prop);
			});
			widget = w;
		} else {
			auto *w = new QLineEdit(QString::fromUtf8(obs_data_get_string(settings_, name)));
			if (type == OBS_TEXT_PASSWORD)
				w->setEchoMode(QLineEdit::Password);
			connect(w, &QLineEdit::textEdited, this, [this, prop, name](const QString &t) {
				obs_data_set_string(settings_, name, t.toUtf8().constData());
				changed(prop);
			});
			widget = w;
		}
		break;
	}
	case OBS_PROPERTY_PATH: {
		auto *w = new QLineEdit(QString::fromUtf8(obs_data_get_string(settings_, name)));
		connect(w, &QLineEdit::textEdited, this, [this, prop, name](const QString &t) {
			obs_data_set_string(settings_, name, t.toUtf8().constData());
			changed(prop);
		});
		widget = w;
		break;
	}
	case OBS_PROPERTY_LIST: {
		auto *w = new QComboBox();
		const obs_combo_format format = obs_property_list_format(prop);
		const size_t count = obs_property_list_item_count(prop);
		int current = -1;
		for (size_t i = 0; i < count; i++) {
			const QString text = QString::fromUtf8(obs_property_list_item_name(prop, i));
			bool match = false;
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				w->addItem(text, QVariant::fromValue<long long>(obs_property_list_item_int(prop, i)));
				match = obs_property_list_item_int(prop, i) == obs_data_get_int(settings_, name);
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				w->addItem(text, obs_property_list_item_float(prop, i));
				match = obs_property_list_item_float(prop, i) == obs_data_get_double(settings_, name);
				break;
			case OBS_COMBO_FORMAT_BOOL:
				w->addItem(text, obs_property_list_item_bool(prop, i));
				match = obs_property_list_item_bool(prop, i) == obs_data_get_bool(settings_, name);
				break;
			default: {
				const char *v = obs_property_list_item_string(prop, i);
				w->addItem(text, QString::fromUtf8(v ? v : ""));
				match = v && std::strcmp(v, obs_data_get_string(settings_, name)) == 0;
				break;
			}
			}
			if (match)
				current = (int)i;
			if (obs_property_list_item_disabled(prop, i))
				if (auto *model = qobject_cast<QStandardItemModel *>(w->model()))
					model->item((int)i)->setEnabled(false);
		}
		w->setCurrentIndex(current);
		connect(w, &QComboBox::currentIndexChanged, this, [this, w, prop, name, format](int idx) {
			if (idx < 0)
				return;
			const QVariant v = w->itemData(idx);
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				obs_data_set_int(settings_, name, v.toLongLong());
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				obs_data_set_double(settings_, name, v.toDouble());
				break;
			case OBS_COMBO_FORMAT_BOOL:
				obs_data_set_bool(settings_, name, v.toBool());
				break;
			default:
				obs_data_set_string(settings_, name, v.toString().toUtf8().constData());
				break;
			}
			changed(prop);
		});
		widget = w;
		break;
	}
	case OBS_PROPERTY_GROUP: {
		auto *box = new QGroupBox(label);
		if (obs_property_group_type(prop) == OBS_GROUP_CHECKABLE) {
			box->setCheckable(true);
			box->setChecked(obs_data_get_bool(settings_, name));
			connect(box, &QGroupBox::toggled, this, [this, prop, name](bool on) {
				obs_data_set_bool(settings_, name, on);
				changed(prop);
			});
		}
		auto *inner = new QFormLayout(box);
		add_properties(obs_property_group_content(prop), inner);
		widget = box;
		full_row = true;
		break;
	}
	default:
		return; /* buttons, colors, fonts, lists of files: not used by encoders */
	}

	widget->setEnabled(obs_property_enabled(prop));
	if (tip && *tip)
		widget->setToolTip(QString::fromUtf8(tip));
	if (full_row)
		form->addRow(widget);
	else
		form->addRow(label, widget);
}

} // namespace vmix::ui
