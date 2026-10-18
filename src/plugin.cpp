#include "core/manager.hpp"
#include "render/root-source.hpp"
#include "ui/dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-video-mixes", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Video Mixes";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

bool obs_module_load(void)
{
	vmix::register_root_source();

	/* No main window when libobs is used headless (e.g. the smoke test). */
	if (obs_frontend_get_main_window())
		vmix::ui::create_dock();

	blog(LOG_INFO, "[video-mixes] Loaded version %s", VMIX_VERSION);
	return true;
}

/* Mixes are loaded once every module is loaded: encoder availability (NVENC, AMF, QSV) must be known. */
void obs_module_post_load(void)
{
	vmix::Manager::get().init();
}

void obs_module_unload(void)
{
	vmix::Manager::get().shutdown();
}
