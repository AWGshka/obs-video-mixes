#pragma once

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QTreeWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace vmix::ui {

/*
 * The sources of the current scene (the preview scene in Studio Mode) with one eye toggle per program mix, like the
 * OBS Sources dock. Groups and nested scenes are expandable, so items inside them can be hidden too. Follows scene
 * switches, item changes and renames; all OBS signals are coalesced into one rebuild on the UI thread.
 */
class VisibilityTree : public QTreeWidget {
public:
	explicit VisibilityTree(QWidget *parent = nullptr);
	~VisibilityTree() override;

	/* Program mixes shown as columns. */
	void set_mixes(const std::vector<uint32_t> &ids, const std::vector<std::string> &names);
	/* Scene shown, for the section title (empty if none). */
	QString scene_name() const { return scene_name_; }
	void set_scene_changed_callback(std::function<void()> fn) { scene_changed_ = std::move(fn); }

	void schedule_rebuild();
	/* Drops every reference into the scene collection (collection switch, exit). */
	void release();

private:
	void rebuild();
	void add_scene(obs_scene_t *scene, QTreeWidgetItem *parent, int depth);
	QWidget *eye(uint32_t mix_id, bool visible); /* the item is the last one in items_ */

	static void on_signal(void *data, calldata_t *);
	static void on_frontend_event(enum obs_frontend_event event, void *data);

	std::vector<uint32_t> ids_;
	std::vector<std::string> names_;
	std::vector<OBSSceneItem> items_;
	std::vector<OBSSignal> signals_;
	OBSSignal rename_signal_;
	std::atomic<bool> pending_{false};
	QString scene_name_;
	std::function<void()> scene_changed_;
	std::set<QString> collapsed_; /* "scene uuid/item id" of collapsed groups and nested scenes */
};

} // namespace vmix::ui
