/*
 * Headless end-to-end checks against a real libobs + D3D11 and the installed OBS plugins.
 *
 * Program mix
 *   Main scene: red background, nested scene "Overlay" (green square, hidden in mix 1), cropped blue square.
 *   1. program shows green, mix 1 shows red underneath (libobs' nested-scene cache bypassed correctly)
 *   2. items without hidden content render identically in both (blue square, background)
 *   3. mid-transition to a scene without hidden items, the mix fades from the *clean* scene
 *   4. after the transition nothing differs and the mix equals program (fast path)
 * Source mix
 *   5. a mix of the 120x120 blue source is a 120x120 canvas filled by that source (no layout, no black)
 * Encoders (OBS' real encoder plugins)
 *   6. preferred encoder is a GPU encoder when one exists; deprecated ids (jim_nvenc) are never selectable
 *   7. every selectable encoder with rate control gets a constant-quality mode, never CBR
 *   8. the preferred encoder records a mix canvas to a file
 *
 * Usage: vmix-smoke <obs install dir> <plugin dll> <plugin data dir>   (cwd = <obs>/bin/64bit)
 */
#include "core/encoders.hpp"
#include "core/frame-rate.hpp"
#include "core/item-path.hpp"

#include <obs.hpp>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t W = 640, H = 360;

struct Pixel {
	int r = -1, g = -1, b = -1;
};

struct Probe {
	explicit Probe(std::vector<std::pair<int, int>> points) : points(std::move(points)), at(this->points.size()) {}
	std::vector<std::pair<int, int>> points;
	std::mutex mutex;
	std::vector<Pixel> at;
	uint64_t frames = 0;
};

void on_frame(void *param, video_data *frame)
{
	auto *probe = static_cast<Probe *>(param);
	std::lock_guard lock(probe->mutex);
	for (size_t i = 0; i < probe->points.size(); i++) {
		const auto [x, y] = probe->points[i];
		const uint8_t *px = frame->data[0] + y * frame->linesize[0] + x * 4;
		probe->at[i] = {px[2], px[1], px[0]}; /* BGRA */
	}
	probe->frames++;
}

/* A raw video output is the supported way to make libobs download frames of a (non-main) canvas. */
void register_probe_output()
{
	obs_output_info info = {};
	info.id = "vmix_probe";
	info.flags = OBS_OUTPUT_VIDEO;
	info.get_name = [](void *) { return "probe"; };
	info.create = [](obs_data_t *settings, obs_output_t *) -> void * {
		return reinterpret_cast<void *>(static_cast<uintptr_t>(obs_data_get_int(settings, "probe")));
	};
	info.destroy = [](void *) {};
	info.start = [](void *) { return true; };
	info.stop = [](void *, uint64_t) {};
	info.raw_video = [](void *data, video_data *frame) { on_frame(data, frame); };
	obs_register_output(&info);
}

obs_output_t *attach_probe(Probe &probe, video_t *video, const char *name)
{
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_int(s, "probe", (long long)reinterpret_cast<uintptr_t>(&probe));
	obs_output_t *out = obs_output_create("vmix_probe", name, s, nullptr);
	obs_output_set_media(out, video, obs_get_audio());
	if (!obs_output_begin_data_capture(out, 0))
		std::fprintf(stderr, "begin_data_capture failed for %s\n", name);
	return out;
}

void detach_probe(obs_output_t *out)
{
	obs_output_end_data_capture(out);
	obs_output_release(out);
}

Pixel sample(Probe &p, size_t i)
{
	std::lock_guard lock(p.mutex);
	return p.at[i];
}

void wait_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

obs_source_t *color(const char *name, uint32_t abgr, uint32_t cx, uint32_t cy)
{
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_int(s, "color", abgr);
	obs_data_set_int(s, "width", cx);
	obs_data_set_int(s, "height", cy);
	return obs_source_create("color_source_v3", name, s, nullptr);
}

obs_sceneitem_t *place(obs_scene_t *scene, obs_source_t *src, float x, float y)
{
	obs_sceneitem_t *item = obs_scene_add(scene, src);
	vec2 pos = {{{x, y}}};
	obs_sceneitem_set_pos(item, &pos);
	return item;
}

bool load_module(const std::string &path, const std::string &data, bool required = true)
{
	obs_module_t *mod = nullptr;
	if (obs_open_module(&mod, path.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(mod)) {
		std::fprintf(stderr, "%s to load %s\n", required ? "failed" : "skipped (optional)", path.c_str());
		return false;
	}
	return true;
}

/* Exactly what MixCanvas creates: a private canvas with the plugin's root source on channel 0. */
struct MixUnderTest {
	obs_canvas_t *canvas = nullptr;
	obs_source_t *root = nullptr;

	MixUnderTest(obs_video_info ovi, uint32_t cx, uint32_t cy, const char *source_uuid,
		     const vmix::ItemPath &placement = {})
	{
		ovi.base_width = ovi.output_width = cx;
		ovi.base_height = ovi.output_height = cy;
		OBSDataAutoRelease rs = obs_data_create();
		obs_data_set_int(rs, "mix_id", 1);
		obs_data_set_int(rs, "width", cx);
		obs_data_set_int(rs, "height", cy);
		obs_data_set_bool(rs, "fade", true);
		if (source_uuid)
			obs_data_set_string(rs, "source_uuid", source_uuid);
		if (!placement.empty())
			obs_data_set_array(rs, "item_path", vmix::item_path_to_data(placement));
		root = obs_source_create_private("video_mixes_root", "root", rs);
		canvas = obs_canvas_create_private("mix", &ovi, EPHEMERAL);
		if (canvas && root)
			obs_canvas_set_channel(canvas, 0, root);
	}
	~MixUnderTest()
	{
		if (canvas) {
			obs_canvas_set_channel(canvas, 0, nullptr);
			obs_canvas_remove(canvas);
			obs_canvas_release(canvas);
		}
		obs_source_release(root);
	}
	bool ok() const { return canvas && root; }
	video_t *video() const { return obs_canvas_get_video(canvas); }
};

int failures = 0;
std::atomic<bool> texture_fallback{false};

/* Mirrors libobs log output and notices if a GPU encoder had to copy frames back to the CPU. */
void log_handler(int level, const char *format, va_list args, void *)
{
	char msg[4096];
	std::vsnprintf(msg, sizeof(msg), format, args);
	if (std::strstr(msg, "falling back to non-texture encoder"))
		texture_fallback = true;
	if (level <= LOG_INFO)
		std::fprintf(stderr, "%s\n", msg);
}

void expect(bool ok, const char *what, Pixel p)
{
	std::printf("[%s] %-60s rgb(%3d,%3d,%3d)\n", ok ? " OK " : "FAIL", what, p.r, p.g, p.b);
	if (!ok)
		failures++;
}

void expect(bool ok, const std::string &what)
{
	std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
	if (!ok)
		failures++;
}

bool is(Pixel p, int r, int g, int b)
{
	return std::abs(p.r - r) < 24 && std::abs(p.g - g) < 24 && std::abs(p.b - b) < 24;
}

void check_encoders()
{
	std::printf("\nencoders:\n");
	const auto all = vmix::video_encoders();
	bool any_hardware = false;
	for (const auto &e : all) {
		any_hardware = any_hardware || e.hardware;
		OBSDataAutoRelease s = vmix::recording_defaults(e.id.c_str());
		const char *rc = obs_data_get_string(s, "rate_control");
		const bool has_rc = rc && *rc;
		std::printf("  %-22s %-4s rate_control=%-6s %s\n", e.id.c_str(), e.hardware ? "GPU" : "CPU",
			    has_rc ? rc : "-", obs_data_get_json(s));
		if (has_rc)
			expect(!vmix::uses_constant_bitrate(e.id.c_str(), s), e.id + ": constant-quality recording default");
	}
	const std::string preferred = vmix::preferred_video_encoder();
	expect(!any_hardware || (obs_get_encoder_caps(preferred.c_str()) & OBS_ENCODER_CAP_PASS_TEXTURE),
	       "preferred encoder is a GPU encoder when available: " + preferred);
	if (obs_get_encoder_type("jim_nvenc") == OBS_ENCODER_VIDEO)
		expect(!vmix::encoder_selectable("jim_nvenc"), "deprecated jim_nvenc is not selectable");
}

/* Records ~2 s of the canvas; returns the number of encoded frames (0 on failure). */
int check_recording(video_t *video, const std::string &dir, uint32_t fps_divisor)
{
	const std::string id = vmix::preferred_video_encoder();
	texture_fallback = false;
	const std::string path = dir + "/vmix-smoke-" + id + "-d" + std::to_string(fps_divisor) + ".mkv";
	os_unlink(path.c_str());

	OBSDataAutoRelease vs = vmix::recording_defaults(id.c_str());
	OBSEncoderAutoRelease venc = obs_video_encoder_create(id.c_str(), "smoke-video", vs, nullptr);
	OBSEncoderAutoRelease aenc = obs_audio_encoder_create("ffmpeg_aac", "smoke-audio", nullptr, 0, nullptr);
	OBSDataAutoRelease os = obs_data_create();
	obs_data_set_string(os, "path", path.c_str());
	OBSOutputAutoRelease out = obs_output_create("ffmpeg_muxer", "smoke-rec", os, nullptr);
	if (!venc || !aenc || !out) {
		expect(false, "create encoder/output for " + id);
		return 0;
	}
	obs_encoder_set_video(venc, video);
	if (fps_divisor > 1)
		obs_encoder_set_frame_rate_divisor(venc, fps_divisor);
	obs_encoder_set_audio(aenc, obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);

	const bool started = obs_output_start(out);
	if (!started) {
		const char *err = obs_output_get_last_error(out);
		expect(false, "start recording with " + id + (err ? std::string(": ") + err : ""));
		return 0;
	}
	wait_ms(2000);
	obs_output_stop(out);
	for (int i = 0; i < 100 && obs_output_active(out); i++)
		wait_ms(50);
	const int frames = obs_output_get_total_frames(out);
	const int64_t size = os_get_file_size(path.c_str());
	expect(frames > 10 && size > 0, "mix canvas recorded with " + id + " (fps divisor " +
						std::to_string(fps_divisor) + "): " + std::to_string(frames) + " frames, " +
						std::to_string(size) + " bytes");
	if (obs_get_encoder_caps(id.c_str()) & OBS_ENCODER_CAP_PASS_TEXTURE)
		expect(!texture_fallback, "GPU encoder used the texture path on the mix canvas (no GPU->CPU copy)");
	os_unlink(path.c_str());
	return frames;
}

void check_frame_rate_rules(obs_source_t *sync_source)
{
	std::printf("\nframe rate:\n");
	const double fps = vmix::obs_fps();
	vmix::MixConfig m;
	m.kind = vmix::MixKind::Source;
	const vmix::FrameRateChoice synced = vmix::resolve_frame_rate(m, sync_source);
	expect(synced.divisor == 1 && synced.fps == fps, "auto: a source drawn every frame records at OBS fps");
	m.fps_divisor = 2;
	const vmix::FrameRateChoice fixed = vmix::resolve_frame_rate(m, sync_source);
	expect(fixed.divisor == 2 && fixed.fps == fps / 2, "fixed divisor 2 halves the frame rate");
	m.fps_divisor = 0;
	m.kind = vmix::MixKind::Program;
	expect(vmix::resolve_frame_rate(m, nullptr).divisor == 1, "auto: program mixes record at OBS fps");
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "usage: vmix-smoke <obs dir> <plugin dll> <plugin data dir>\n");
		return 2;
	}
	const std::string obs_dir = argv[1];

#ifdef _WIN32
	/* Real OBS has bin/64bit as its exe dir; plugin dependencies (Qt, frontend API) resolve from there. */
	wchar_t *wbin = nullptr;
	os_utf8_to_wcs_ptr((obs_dir + "/bin/64bit").c_str(), 0, &wbin);
	AddDllDirectory(wbin);
	bfree(wbin);
#endif

	base_set_log_handler(log_handler, nullptr);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 3;

	/* libobs loads modules with the restricted DLL search path, so use an absolute path. */
	const std::string d3d11 = obs_dir + "/bin/64bit/libobs-d3d11";
	obs_video_info ovi = {};
	ovi.graphics_module = d3d11.c_str();
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = W;
	ovi.base_height = ovi.output_height = H;
	ovi.output_format = VIDEO_FORMAT_BGRA;
	ovi.colorspace = VIDEO_CS_SRGB;
	ovi.range = VIDEO_RANGE_FULL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		std::fprintf(stderr, "obs_reset_video failed\n");
		return 4;
	}
	obs_audio_info2 oai = {};
	oai.samples_per_sec = 48000;
	oai.speakers = SPEAKERS_STEREO;
	obs_reset_audio2(&oai);

	const std::string plugins = obs_dir + "/obs-plugins/64bit/";
	const std::string data = obs_dir + "/data/obs-plugins/";
	if (!load_module(plugins + "image-source.dll", data + "image-source") ||
	    !load_module(plugins + "obs-transitions.dll", data + "obs-transitions") ||
	    !load_module(plugins + "obs-ffmpeg.dll", data + "obs-ffmpeg") ||
	    !load_module(plugins + "obs-x264.dll", data + "obs-x264") || !load_module(argv[2], argv[3]))
		return 5;
	load_module(plugins + "obs-nvenc.dll", data + "obs-nvenc", false);
	load_module(plugins + "obs-qsv11.dll", data + "obs-qsv11", false);
	register_probe_output();

	/* ---- scene graph ---- */
	obs_source_t *red = color("red", 0xFF0000FF, W, H);
	obs_source_t *green = color("green", 0xFF00FF00, 100, 100);
	obs_source_t *blue = color("blue", 0xFFFF0000, 120, 120);
	obs_source_t *yellow = color("yellow", 0xFF00FFFF, W, H);

	obs_scene_t *overlay = obs_scene_create("Overlay");
	obs_sceneitem_t *green_item = place(overlay, green, 0, 0);

	obs_scene_t *main_scene = obs_scene_create("Main");
	place(main_scene, red, 0, 0);
	obs_sceneitem_t *overlay_item = place(main_scene, obs_scene_get_source(overlay), 0, 0);
	vec2 overlay_scale = {{{2.0f, 2.0f}}}; /* nested scene scaled: green shows as 200x200 */
	obs_sceneitem_set_scale(overlay_item, &overlay_scale);
	obs_sceneitem_t *blue_item = place(main_scene, blue, 500, 200);
	obs_sceneitem_crop crop = {10, 10, 10, 10};
	obs_sceneitem_set_crop(blue_item, &crop);

	obs_scene_t *other_scene = obs_scene_create("Other");
	place(other_scene, yellow, 0, 0);

	/* Hide the green square in mix 1 (what the dock writes). */
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(green_item);
	obs_data_set_int(priv, "video_mixes_hidden", 1 << 1);

	obs_source_t *fade = obs_source_create("fade_transition", "Fade", nullptr, nullptr);
	obs_transition_set(fade, obs_scene_get_source(main_scene));
	obs_set_output_source(0, fade);

	{
		/* ---- program mix ---- */
		MixUnderTest mix(ovi, W, H, nullptr);
		if (!mix.ok()) {
			std::fprintf(stderr, "root source or canvas creation failed\n");
			return 6;
		}
		Probe program({{50, 50}, {550, 250}, {300, 300}}), mixp({{50, 50}, {550, 250}, {300, 300}});
		obs_output_t *program_out = attach_probe(program, obs_get_video(), "program-probe");
		obs_output_t *mix_out = attach_probe(mixp, mix.video(), "mix-probe");

		wait_ms(1500);
		std::printf("program mix:\n");
		expect(is(sample(program, 0), 0, 255, 0), "program shows green square (nested scene)", sample(program, 0));
		expect(is(sample(mixp, 0), 255, 0, 0), "mix hides green square, shows red background", sample(mixp, 0));
		expect(is(sample(program, 1), 0, 0, 255), "program shows cropped blue square", sample(program, 1));
		expect(is(sample(mixp, 1), 0, 0, 255), "mix shows cropped blue square (own item texture)", sample(mixp, 1));
		expect(is(sample(mixp, 2), 255, 0, 0), "mix background", sample(mixp, 2));

		/* Real mixes inherit OBS' NV12 output format, which GPU encoders consume as textures. */
		obs_video_info nv12 = ovi;
		nv12.output_format = VIDEO_FORMAT_NV12;
		nv12.colorspace = VIDEO_CS_709;
		nv12.range = VIDEO_RANGE_PARTIAL;
		MixUnderTest rec_mix(nv12, W, H, nullptr);
		const char *tmp = std::getenv("TEMP");
		const int full = check_recording(rec_mix.video(), tmp ? tmp : ".", 1);
		const int half = check_recording(rec_mix.video(), tmp ? tmp : ".", 2);
		expect(full > 0 && half > 0 && std::abs(half * 2 - full) <= full / 5,
		       "fps divisor 2 encodes half the frames (" + std::to_string(full) + " vs " + std::to_string(half) + ")");

		obs_transition_start(fade, OBS_TRANSITION_MODE_AUTO, 3000, obs_scene_get_source(other_scene));
		wait_ms(1500);
		const Pixel pm = sample(program, 0), mm = sample(mixp, 0);
		expect(pm.g > 200 && mm.r > 200 && mm.r > pm.r + 40, "mid-fade: mix fades from clean scene, not green", mm);
		wait_ms(2500);
		expect(is(sample(program, 0), 255, 255, 0), "after transition program is yellow", sample(program, 0));
		expect(is(sample(mixp, 0), 255, 255, 0), "after transition mix equals program (fast path)", sample(mixp, 0));

		detach_probe(program_out);
		detach_probe(mix_out);
	}

	{
		/* ---- source mix, not placed anywhere: the source at its own size ---- */
		obs_source_t *loose = color("loose", 0xFFFF00FF, 64, 48); /* magenta, in no scene */
		MixUnderTest mix(ovi, 64, 48, obs_source_get_uuid(loose));
		Probe p({{2, 2}, {32, 24}, {61, 45}});
		obs_output_t *out = attach_probe(p, mix.video(), "loose-probe");
		wait_ms(800);
		std::printf("\nsource mix, not placed in a scene:\n");
		expect(vmix::find_item_path(obs_source_get_uuid(loose)).empty(), "no placement found");
		for (size_t i = 0; i < 3; i++)
			expect(is(sample(p, i), 255, 0, 255), "source fills its own-size canvas", sample(p, i));
		detach_probe(out);
		obs_source_release(loose);
	}

	/* ---- source mix as shown in the scene: crop applied, nested scenes followed ---- */
	struct PlacedCase {
		const char *what;
		obs_source_t *source;
		uint32_t cx, cy;
		size_t depth;
		int r, g, b;
	};
	for (const PlacedCase &c : {PlacedCase{"cropped blue (120x120, 10px crop)", blue, 100, 100, 1, 0, 0, 255},
				    PlacedCase{"green 100x100 inside nested scene scaled 2x", green, 200, 200, 2, 0, 255, 0}}) {
		const vmix::ItemPath path = vmix::find_item_path(obs_source_get_uuid(c.source));
		vmix::PlacedItem placed;
		const bool ok = vmix::place_item(path, placed);
		std::printf("\nsource mix as placed: %s\n", c.what);
		expect(ok && path.size() == c.depth, "placement found through " + std::to_string(c.depth) + " scene level(s)");
		expect(ok && placed.width() == c.cx && placed.height() == c.cy,
		       "canvas = on-screen size " + std::to_string(placed.width()) + "x" + std::to_string(placed.height()));
		if (!ok)
			continue;
		MixUnderTest mix(ovi, placed.width(), placed.height(), obs_source_get_uuid(c.source), path);
		const int w = (int)placed.width(), h = (int)placed.height();
		Probe p({{1, 1}, {w / 2, h / 2}, {w - 2, h - 2}});
		obs_output_t *out = attach_probe(p, mix.video(), "placed-probe");
		wait_ms(800);
		for (size_t i = 0; i < 3; i++)
			expect(is(sample(p, i), c.r, c.g, c.b), "item fills the canvas edge to edge (no background)",
			       sample(p, i));
		detach_probe(out);
	}

	check_encoders();
	check_frame_rate_rules(blue);

	/* ---- teardown ---- */
	obs_set_output_source(0, nullptr);
	obs_source_release(fade);
	for (obs_scene_t *scene : {main_scene, other_scene, overlay}) {
		obs_source_remove(obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	for (obs_source_t *s : {red, green, blue, yellow})
		obs_source_release(s);
	obs_shutdown();

	std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
	return failures ? 1 : 0;
}
