#pragma once

#include "core/config.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QStyle>
#include <QToolButton>

namespace vmix::ui {

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

inline QString T(const char *key, const QString &arg)
{
	return T(key).replace(QStringLiteral("%1"), arg);
}

inline QString T(const char *key, const QString &arg1, const QString &arg2)
{
	return T(key).replace(QStringLiteral("%1"), arg1).replace(QStringLiteral("%2"), arg2);
}

/* OBS' own UI strings (e.g. "OK", "Cancel"), in OBS' UI language. */
inline QString OBST(const char *key)
{
	return QString::fromUtf8(obs_frontend_get_locale_string(key));
}

inline QString Q(const std::string &s)
{
	return QString::fromStdString(s);
}

/*
 * OBS 32 themes style utility classes for every theme and variant: text-muted, text-danger, text-warning,
 * text-success, text-small, text-bold, text-heading, btn-tool, icon-gear, icon-plus, icon-trash, icon-dots-vert,
 * icon-media-play/stop/pause, icon-save, indicator-visibility, dialog-frame... Using them keeps the plugin's UI
 * consistent with OBS in light and dark themes without hardcoded colors.
 */
inline void set_class(QWidget *w, const QString &cls)
{
	if (w->property("class").toString() == cls)
		return;
	w->setProperty("class", cls);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

inline QLabel *label(const QString &text, const QString &cls = {})
{
	auto *l = new QLabel(text);
	l->setTextFormat(Qt::PlainText);
	if (!cls.isEmpty())
		l->setProperty("class", cls);
	return l;
}

inline QLabel *hint(const QString &text)
{
	auto *l = label(text, QStringLiteral("text-muted text-small"));
	l->setWordWrap(true);
	return l;
}

/* A flat themed tool button showing one of OBS' theme icons. */
inline QToolButton *icon_button(const char *icon_class, const QString &tooltip)
{
	auto *b = new QToolButton();
	b->setProperty("class", QStringLiteral("btn-tool %1").arg(QLatin1String(icon_class)));
	b->setToolTip(tooltip);
	b->setAccessibleName(tooltip);
	b->setAutoRaise(true);
	return b;
}

/* Qt translates standard buttons by system locale; use OBS' UI language instead. */
inline void use_obs_button_texts(QDialogButtonBox *box)
{
	if (QPushButton *ok = box->button(QDialogButtonBox::Ok))
		ok->setText(OBST("OK"));
	if (QPushButton *cancel = box->button(QDialogButtonBox::Cancel))
		cancel->setText(OBST("Cancel"));
	if (QPushButton *save = box->button(QDialogButtonBox::Save))
		save->setText(OBST("Save"));
}

inline const char *trigger_key(Trigger t)
{
	switch (t) {
	case Trigger::Recording:
		return "Trigger.Recording";
	case Trigger::Streaming:
		return "Trigger.Streaming";
	case Trigger::ReplayBuffer:
		return "Trigger.ReplayBuffer";
	default:
		return "Trigger.Manual";
	}
}

} // namespace vmix::ui
