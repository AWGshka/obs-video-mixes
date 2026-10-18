#include "ui/preview.hpp"
#include "core/manager.hpp"

#include <QResizeEvent>
#include <QTimer>

#include <algorithm>

namespace vmix::ui {

MixPreview::MixPreview(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumHeight(120);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

MixPreview::~MixPreview()
{
	display_ = nullptr; /* removes the draw callback before the canvas goes away */
	unbind();
}

void MixPreview::bind(uint32_t mix_id)
{
	if (mix_id_ == mix_id)
		return;
	unbind();
	mix_id_ = mix_id;
	set_canvas(Manager::get().acquire_preview(mix_id));
}

void MixPreview::unbind()
{
	if (!mix_id_)
		return;
	set_canvas(nullptr);
	Manager::get().release_preview(mix_id_);
	mix_id_ = 0;
}

void MixPreview::sync()
{
	if (!mix_id_)
		return;
	if (!Manager::get().find(mix_id_)) {
		set_canvas(nullptr);
		mix_id_ = 0;
		return;
	}
	obs_canvas_t *current = Manager::get().canvas(mix_id_);
	if (current != canvas_.Get())
		set_canvas(current);
}

void MixPreview::set_canvas(obs_canvas_t *canvas)
{
	std::lock_guard lock(mutex_);
	canvas_ = canvas;
}

void MixPreview::create_display()
{
#if defined(_WIN32) || defined(__APPLE__)
	if (display_ || !isVisible() || !windowHandle())
		return;
	const qreal dpr = devicePixelRatioF();
	gs_init_data info = {};
	info.cx = (uint32_t)std::max(1.0, width() * dpr);
	info.cy = (uint32_t)std::max(1.0, height() * dpr);
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	info.window.hwnd = reinterpret_cast<void *>(winId());
#else
	info.window.view = reinterpret_cast<id>(winId());
#endif
	display_ = obs_display_create(&info, 0x1f1f1f);
	if (display_)
		obs_display_add_draw_callback(display_, &MixPreview::draw, this);
#endif
}

/* Floating/re-docking can recreate the native window; the display must follow the new handle. */
bool MixPreview::event(QEvent *event)
{
	if (event->type() == QEvent::WinIdChange && display_) {
		display_ = nullptr;
		QTimer::singleShot(0, this, [this] { create_display(); });
	}
	return QWidget::event(event);
}

void MixPreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	create_display();
}

void MixPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	create_display();
	if (display_) {
		const qreal dpr = devicePixelRatioF();
		obs_display_resize(display_, (uint32_t)(event->size().width() * dpr),
				   (uint32_t)(event->size().height() * dpr));
	}
}

/* Graphics thread. Letterboxes the mix's last rendered frame into the display. */
void MixPreview::draw(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MixPreview *>(data);
	std::lock_guard lock(self->mutex_);
	obs_canvas_t *canvas = self->canvas_;
	obs_video_info ovi;
	if (!canvas || !obs_canvas_get_video_info(canvas, &ovi) || !ovi.base_width || !ovi.base_height)
		return;

	const float scale = std::min((float)cx / (float)ovi.base_width, (float)cy / (float)ovi.base_height);
	const int w = (int)((float)ovi.base_width * scale);
	const int h = (int)((float)ovi.base_height * scale);

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
	gs_set_viewport(((int)cx - w) / 2, ((int)cy - h) / 2, w, h);
	obs_render_canvas_texture(canvas);
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace vmix::ui
