#pragma once

#include <obs.hpp>

#include <QWidget>

#include <cstdint>
#include <mutex>

namespace vmix::ui {

/*
 * Live view of one mix canvas. While bound it keeps the canvas alive (and therefore rendering), so the
 * dock only binds it while the preview is toggled on.
 */
class MixPreview : public QWidget {
public:
	explicit MixPreview(QWidget *parent = nullptr);
	~MixPreview() override;

	void bind(uint32_t mix_id);
	void unbind();
	/* Re-query the manager after a notify; the canvas may have been recreated. */
	void sync();
	uint32_t mix_id() const { return mix_id_; }

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	bool event(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	static void draw(void *data, uint32_t cx, uint32_t cy);
	void create_display();
	void set_canvas(obs_canvas_t *canvas);

	uint32_t mix_id_ = 0;
	std::mutex mutex_; /* guards canvas_ against the graphics thread */
	OBSCanvas canvas_;
	OBSDisplay display_;
};

} // namespace vmix::ui
