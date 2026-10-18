#pragma once

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDialogButtonBox>
#include <QPushButton>
#include <QString>

namespace vmix::ui {

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

inline QString T(const char *key, const QString &arg)
{
	return T(key).replace(QStringLiteral("%1"), arg);
}

inline QString Q(const std::string &s)
{
	return QString::fromStdString(s);
}

/* Qt translates standard buttons by system locale; use OBS' UI language instead. */
inline void use_obs_button_texts(QDialogButtonBox *box)
{
	if (QPushButton *ok = box->button(QDialogButtonBox::Ok))
		ok->setText(QString::fromUtf8(obs_frontend_get_locale_string("OK")));
	if (QPushButton *cancel = box->button(QDialogButtonBox::Cancel))
		cancel->setText(QString::fromUtf8(obs_frontend_get_locale_string("Cancel")));
}

} // namespace vmix::ui
