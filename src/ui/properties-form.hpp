#pragma once

#include <obs.hpp>

#include <QDialog>

#include <functional>

class QFormLayout;
class QWidget;

namespace vmix::ui {

/*
 * Minimal renderer for obs_properties_t (bool/int/float/text/list/group/path), enough for encoder
 * settings. OBS' own properties view is not exported to plugins.
 */
class PropertiesDialog : public QDialog {
public:
	using Factory = std::function<obs_properties_t *()>;

	PropertiesDialog(QWidget *parent, const QString &title, obs_data_t *settings, Factory factory);

	/* Settings as edited; apply to the caller's data only if exec() returned Accepted. */
	obs_data_t *settings() const { return settings_; }

private:
	void rebuild();
	void add_properties(obs_properties_t *props, QFormLayout *form);
	void add_property(obs_property_t *prop, QFormLayout *form);
	void changed(obs_property_t *prop);

	OBSData settings_;
	OBSProperties props_;
	QWidget *body_ = nullptr;
	bool rebuild_queued_ = false;
};

} // namespace vmix::ui
