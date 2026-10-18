#include "ui/visibility-tree.hpp"
#include "core/visibility.hpp"
#include "ui/common.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollBar>

namespace vmix::ui {

namespace {

constexpr int kMaxDepth = 8;
constexpr int kKeyRole = Qt::UserRole + 1;

QString item_key(obs_sceneitem_t *item)
{
	obs_source_t *scene = obs_scene_get_source(obs_sceneitem_get_scene(item));
	return QStringLiteral("%1/%2").arg(QString::fromUtf8(obs_source_get_uuid(scene))).arg(obs_sceneitem_get_id(item));
}

} // namespace

VisibilityTree::VisibilityTree(QWidget *parent) : QTreeWidget(parent)
{
	setUniformRowHeights(true);
	setSelectionMode(QAbstractItemView::NoSelection);
	setFocusPolicy(Qt::NoFocus);
	setIndentation(14);
	setAllColumnsShowFocus(false);
	header()->setStretchLastSection(false);
	header()->setDefaultAlignment(Qt::AlignCenter);
	header()->setMinimumSectionSize(36);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *it) {
		collapsed_.insert(it->data(0, kKeyRole).toString());
	});
	connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) {
		collapsed_.erase(it->data(0, kKeyRole).toString());
	});

	obs_frontend_add_event_callback(&VisibilityTree::on_frontend_event, this);
	rename_signal_.Connect(obs_get_signal_handler(), "source_rename", &VisibilityTree::on_signal, this);
	schedule_rebuild();
}

VisibilityTree::~VisibilityTree()
{
	obs_frontend_remove_event_callback(&VisibilityTree::on_frontend_event, this);
	rename_signal_.Disconnect();
	release();
}

void VisibilityTree::on_frontend_event(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<VisibilityTree *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		self->schedule_rebuild();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		self->release();
		break;
	default:
		break;
	}
}

/* Any thread. */
void VisibilityTree::on_signal(void *data, calldata_t *)
{
	static_cast<VisibilityTree *>(data)->schedule_rebuild();
}

void VisibilityTree::schedule_rebuild()
{
	if (pending_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			pending_ = false;
			rebuild();
		},
		Qt::QueuedConnection);
}

void VisibilityTree::release()
{
	signals_.clear();
	clear();
	items_.clear();
}

void VisibilityTree::set_mixes(const std::vector<uint32_t> &ids, const std::vector<std::string> &names)
{
	if (ids == ids_ && names == names_)
		return;
	ids_ = ids;
	names_ = names;
	schedule_rebuild();
}

void VisibilityTree::rebuild()
{
	const int scroll = verticalScrollBar() ? verticalScrollBar()->value() : 0;
	setUpdatesEnabled(false);
	release();

	QStringList headers{T("Visibility.Source")};
	for (const std::string &n : names_)
		headers << Q(n);
	setColumnCount((int)headers.size());
	setHeaderLabels(headers);
	headerItem()->setToolTip(0, T("Visibility.Hint"));
	for (int c = 1; c < headers.size(); c++)
		headerItem()->setToolTip(c, T("Visibility.ColumnHint", headers[c]));
	header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int c = 1; c < headers.size(); c++)
		header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);

	OBSSourceAutoRelease scene_source = obs_frontend_preview_program_mode_active()
						    ? obs_frontend_get_current_preview_scene()
						    : obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	scene_name_ = scene ? QString::fromUtf8(obs_source_get_name(scene_source)) : QString();
	if (scene)
		add_scene(scene, nullptr, 0);

	/* Only reserve the expand-arrow gutter when something can be expanded. */
	bool nested = false;
	for (int i = 0; i < topLevelItemCount() && !nested; i++)
		nested = topLevelItem(i)->childCount() > 0;
	setRootIsDecorated(nested);

	setUpdatesEnabled(true);
	if (verticalScrollBar())
		verticalScrollBar()->setValue(scroll);
	if (scene_changed_)
		scene_changed_();
}

QWidget *VisibilityTree::eye(uint32_t mix_id, bool visible)
{
	auto *box = new QCheckBox();
	box->setProperty("class", QStringLiteral("indicator-visibility"));
	box->setChecked(visible);
	box->setFocusPolicy(Qt::NoFocus);
	const size_t index = items_.size() - 1; /* the item was pushed before its eyes are made */
	connect(box, &QCheckBox::toggled, this, [this, index, mix_id](bool checked) {
		if (index < items_.size())
			set_hidden(items_[index], 1u << mix_id, !checked);
	});

	auto *cell = new QWidget();
	auto *layout = new QHBoxLayout(cell);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(box, 0, Qt::AlignCenter);
	return cell;
}

/* Items top-most first, like the OBS Sources dock; groups and nested scenes become subtrees. */
void VisibilityTree::add_scene(obs_scene_t *scene, QTreeWidgetItem *parent, int depth)
{
	signal_handler_t *sh = obs_source_get_signal_handler(obs_scene_get_source(scene));
	for (const char *sig : {"item_add", "item_remove", "reorder", "refresh", "item_visible"})
		signals_.emplace_back(sh, sig, &VisibilityTree::on_signal, this);

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
		items_.push_back(item);

		auto *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
		node->setText(0, QString::fromUtf8(obs_source_get_name(source)));
		node->setData(0, kKeyRole, item_key(item));
		node->setFlags(Qt::ItemIsEnabled);
		const char *type = obs_source_get_display_name(obs_source_get_id(source));
		QString tip = type ? QString::fromUtf8(type) : QString();
		if (!obs_sceneitem_visible(item)) {
			/* Hidden in OBS itself: the mixes cannot show it either. */
			QFont f = node->font(0);
			f.setItalic(true);
			node->setFont(0, f);
			tip += QStringLiteral("\n") + T("Visibility.HiddenInObs");
		}
		node->setToolTip(0, tip);
		for (size_t c = 0; c < ids_.size(); c++)
			setItemWidget(node, (int)c + 1, eye(ids_[c], !(mask & (1u << ids_[c]))));

		if (depth < kMaxDepth) {
			obs_scene_t *child = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
									  : obs_scene_from_source(source);
			if (child) {
				add_scene(child, node, depth + 1);
				node->setExpanded(!collapsed_.count(item_key(item)));
			}
		}
	}
}

} // namespace vmix::ui
