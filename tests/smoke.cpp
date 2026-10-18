/*
 * Headless end-to-end checks against a real libobs + D3D11 and the installed OBS plugins.
 *
 * Program mix
 *   Main scene: red background, nested scene "Overlay" (green square, hidden in mix 1), cropped blue square.
 *   1. program shows green, mix 1 shows red underneath (libobs' nested-scene cache bypassed correctly)
 *   2. items without hidden content render identically in both (blue square, background)
 *   3. mid-transition to a scene without hidden items, the mix fades from the *clean* scene
 *   4. after the transition nothing differs and the mix equals program (fast path)
 *   5. a filter on a re-rendered (nested) scene applies in the mix and follows settings / enable changes
 *   6. an item hidden with a hide transition fades out in the mix like in program
 * Source mix
 *   7. a mix of the 120x120 blue source is a 120x120 canvas filled by that source (no layout, no black)
 * Encoders (OBS' real encoder plugins)
 *   8. preferred encoder is a GPU encoder when one exists; deprecated ids (jim_nvenc) are never selectable
 *   9. every selectable encoder with rate control gets a constant-quality mode, never CBR
 *  10. the preferred encoder records a mix canvas to a file
 * Outputs
 *  11. a recording and a replay buffer share one encoder set; the replay saves a file, both stop cleanly
 *  12. stream encoders default to CBR; an unreachable server ends the stream with an error, not a hang
 *  13. a source's own audio (volume and mute applied) becomes a track on the OBS clock
 * Shutdown
 *  14. no libobs allocations are left
 *
 * Usage: vmix-smoke <obs install dir> <plugin dll> <plugin data dir>   (cwd = <obs>/bin/64bit)
 */
#include "core/encoders.hpp"
#include "core/frame-rate.hpp"
#include "core/item-path.hpp"
#include "core/outputs.hpp"
#include "core/source-audio.hpp"
#include "core/util.hpp"

#include <obs.hpp>
#include <media-io/audio-io.h>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
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

/* OBS_TASK_UI tasks (filter copies, output stop events) run on the main thread while it waits. */
struct UiTask {
	obs_task_t task;
	void *param;
	std::atomic<bool> *done;
};
std::mutex ui_mutex;
std::vector<UiTask> ui_tasks;
std::thread::id main_thread;

void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	if (std::this_thread::get_id() == main_thread) {
		task(param);
		return;
	}
	std::atomic<bool> done{false};
	{
		std::lock_guard lock(ui_mutex);
		ui_tasks.push_back({task, param, wait ? &done : nullptr});
	}
	while (wait && !done)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void pump_ui()
{
	std::vector<UiTask> tasks;
	{
		std::lock_guard lock(ui_mutex);
		tasks.swap(ui_tasks);
	}
	for (UiTask &t : tasks) {
		t.task(t.param);
		if (t.done)
			*t.done = true;
	}
}

void wait_ms(int ms)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		pump_ui();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	pump_ui();
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

/* VMIX_SMOKE_SECTIONS=program,recording,outputs,filters,source,encoders,audio,framerate runs only those parts
 * (default: all), to narrow down a failure. */
bool section(const char *name)
{
	const char *only = std::getenv("VMIX_SMOKE_SECTIONS");
	if (!only || !*only)
		return true;
	const std::string list = std::string(",") + only + ",";
	return list.find(std::string(",") + name + ",") != std::string::npos;
}

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

/* ---- outputs ---- */

struct OutputEventRecord {
	uint64_t token;
	vmix::OutputEvent event;
	int code;
	std::string text;
};
std::mutex events_mutex;
std::vector<OutputEventRecord> output_events;

void on_output_event(uint64_t token, vmix::OutputEvent event, int code, const std::string &text)
{
	std::lock_guard lock(events_mutex);
	output_events.push_back({token, event, code, text});
}

bool wait_event(uint64_t token, vmix::OutputEvent event, int timeout_ms, int *code = nullptr, std::string *text = nullptr)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < end) {
		pump_ui();
		{
			std::lock_guard lock(events_mutex);
			for (const OutputEventRecord &e : output_events) {
				if (e.token != token || e.event != event)
					continue;
				if (code)
					*code = e.code;
				if (text)
					*text = e.text;
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

void check_outputs(video_t *video, const std::string &dir)
{
	std::printf("\nrecording + replay buffer on shared encoders:\n");
	vmix::set_output_event_handler(on_output_event);

	vmix::EncoderSpec spec;
	spec.name = "smoke-files";
	spec.encoder_id = vmix::preferred_video_encoder();
	spec.encoder_settings = vmix::adopt(vmix::recording_defaults(spec.encoder_id.c_str()));
	spec.audio_tracks = 1;
	std::string error;
	std::shared_ptr<vmix::EncoderSet> encoders = vmix::EncoderSet::create(spec, video, 1, error);
	expect(encoders != nullptr, "encoder set created " + error);
	if (!encoders)
		return;

	vmix::OutputSpec rec;
	rec.name = "smoke-record";
	rec.path = dir + "/vmix-smoke-shared.mkv";
	os_unlink(rec.path.c_str());
	auto record = vmix::MixOutput::start(vmix::OutputKind::Record, rec, encoders, 1, error);
	expect(record != nullptr, "recording started " + error);

	vmix::OutputSpec rb;
	rb.name = "smoke-replay";
	rb.directory = dir;
	rb.filename_format = "vmix-smoke-replay %hh-%mm-%ss";
	rb.extension = "mkv";
	rb.max_seconds = 10;
	rb.max_mb = 64;
	auto replay = vmix::MixOutput::start(vmix::OutputKind::Replay, rb, encoders, 2, error);
	expect(replay != nullptr, "replay buffer started on the same encoders " + error);
	encoders.reset(); /* the outputs keep them alive */

	wait_ms(2500);
	if (replay) {
		expect(replay->save_replay(), "replay save requested");
		std::string path;
		const bool saved = wait_event(2, vmix::OutputEvent::ReplaySaved, 8000, nullptr, &path);
		const int64_t size = saved ? os_get_file_size(path.c_str()) : 0;
		expect(saved && size > 0, "replay saved: " + path + " (" + std::to_string(size) + " bytes)");
		if (saved)
			os_unlink(path.c_str());
		replay->stop();
		expect(wait_event(2, vmix::OutputEvent::Stopped, 5000), "replay buffer stopped");
	}
	if (record) {
		record->stop();
		int code = -1;
		const bool stopped = wait_event(1, vmix::OutputEvent::Stopped, 5000, &code);
		expect(stopped && code == OBS_OUTPUT_SUCCESS, "recording finished cleanly (code " + std::to_string(code) + ")");
		const int frames = record->total_frames();
		expect(frames > 60 && os_get_file_size(rec.path.c_str()) > 0,
		       "recording kept encoding next to the replay buffer: " + std::to_string(frames) + " frames");
		os_unlink(rec.path.c_str());
	}
	wait_ms(200);
	record.reset();
	replay.reset();

	std::printf("\nstream output:\n");
	vmix::EncoderSpec sspec = spec;
	sspec.name = "smoke-stream";
	sspec.encoder_settings = vmix::adopt(vmix::stream_defaults(sspec.encoder_id.c_str(), 2500));
	expect(std::strcmp(obs_data_get_string(sspec.encoder_settings, "rate_control"), "CBR") == 0,
	       std::string("stream encoder defaults to CBR: ") + obs_data_get_json(sspec.encoder_settings));
	std::shared_ptr<vmix::EncoderSet> stream_encoders = vmix::EncoderSet::create(sspec, video, 1, error);
	vmix::OutputSpec st;
	st.name = "smoke-stream";
	st.server = "rtmp://127.0.0.1:1/live";
	st.key = "smoke";
	auto stream = stream_encoders
			      ? vmix::MixOutput::start(vmix::OutputKind::Stream, st, stream_encoders, 3, error)
			      : nullptr;
	if (stream) {
		int code = 0;
		const bool stopped = wait_event(3, vmix::OutputEvent::Stopped, 20000, &code);
		expect(stopped && code != OBS_OUTPUT_SUCCESS,
		       "unreachable server: the stream stops with an error (code " + std::to_string(code) + ")");
	} else {
		expect(!error.empty(), "unreachable server: start fails with a message: " + error);
	}
	wait_ms(200); /* like Manager::retire(): the output's connect thread finishes after signalling stop */
	stream.reset();
	stream_encoders.reset();
	vmix::set_output_event_handler(nullptr);
}

/* ---- source audio ---- */

/* An audio-only source playing a 440 Hz sine (amplitude 0.5) with real-time timestamps. */
struct Tone {
	obs_source_t *source;
	std::atomic<bool> run{true};
	std::thread thread;
};

void register_tone_source()
{
	obs_source_info info = {};
	info.id = "vmix_tone";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = [](void *) { return "tone"; };
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		auto *tone = new Tone{source};
		tone->thread = std::thread([tone] {
			constexpr uint32_t rate = 48000, frames = 480;
			std::vector<float> buf(frames);
			const uint64_t start = os_gettime_ns();
			uint64_t n = 0;
			while (tone->run) {
				for (uint32_t i = 0; i < frames; i++)
					buf[i] = 0.5f * std::sin(6.2831853f * 440.0f * (float)(n + i) / (float)rate);
				obs_source_audio a = {};
				a.data[0] = a.data[1] = reinterpret_cast<const uint8_t *>(buf.data());
				a.frames = frames;
				a.speakers = SPEAKERS_STEREO;
				a.format = AUDIO_FORMAT_FLOAT_PLANAR;
				a.samples_per_sec = rate;
				a.timestamp = start + audio_frames_to_ns(rate, n);
				obs_source_output_audio(tone->source, &a);
				n += frames;
				os_sleepto_ns(start + audio_frames_to_ns(rate, n));
			}
		});
		return tone;
	};
	info.destroy = [](void *data) {
		auto *tone = static_cast<Tone *>(data);
		tone->run = false;
		tone->thread.join();
		delete tone;
	};
	obs_register_source(&info);
}

struct AudioProbe {
	std::mutex mutex;
	double rms = 0.0;
	uint64_t last_ts = 0;
	uint64_t received_at = 0;
};

void on_probe_audio(void *param, size_t, audio_data *data)
{
	auto *p = static_cast<AudioProbe *>(param);
	const float *s = reinterpret_cast<const float *>(data->data[0]);
	double sum = 0.0;
	for (uint32_t i = 0; i < data->frames; i++)
		sum += (double)s[i] * s[i];
	std::lock_guard lock(p->mutex);
	p->rms = data->frames ? std::sqrt(sum / data->frames) : 0.0;
	p->last_ts = data->timestamp;
	p->received_at = os_gettime_ns();
}

void check_source_audio()
{
	std::printf("\nsource audio (a source's own audio as a track):\n");
	obs_source_t *tone = obs_source_create("vmix_tone", "tone", nullptr, nullptr);
	std::string error;
	auto audio = vmix::SourceAudio::create(tone, error);
	expect(audio != nullptr, "source audio created " + error);
	if (!audio) {
		obs_source_release(tone);
		return;
	}
	AudioProbe probe;
	audio_output_connect(audio->audio(), 0, nullptr, on_probe_audio, &probe);
	wait_ms(1500);
	{
		std::lock_guard lock(probe.mutex);
		const double lag_ms = (double)(probe.received_at - probe.last_ts) / 1e6;
		expect(probe.rms > 0.3 && probe.rms < 0.4,
		       "the source's sine arrives on the track (rms " + std::to_string(probe.rms) + ")");
		expect(lag_ms > 250.0 && lag_ms < 450.0,
		       "timestamps stay on the OBS clock, a fixed delay behind real time (" + std::to_string(lag_ms) +
			       " ms)");
	}
	obs_source_set_volume(tone, 0.5f);
	wait_ms(800);
	{
		std::lock_guard lock(probe.mutex);
		expect(probe.rms > 0.15 && probe.rms < 0.2,
		       "the source's volume applies (rms " + std::to_string(probe.rms) + " at 50%)");
	}
	obs_source_set_muted(tone, true);
	wait_ms(800);
	{
		std::lock_guard lock(probe.mutex);
		expect(probe.rms < 0.001, "muting the source silences its track (rms " + std::to_string(probe.rms) + ")");
	}
	audio_output_disconnect(audio->audio(), 0, on_probe_audio, &probe);
	audio.reset();
	obs_source_release(tone);
}

} // namespace

/* The core files linked into this test look up their (error) texts through the module API. */
extern "C" const char *obs_module_text(const char *lookup)
{
	return lookup;
}

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
	main_thread = std::this_thread::get_id();
	obs_set_ui_task_handler(ui_task_handler);

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
	    !load_module(plugins + "obs-filters.dll", data + "obs-filters") ||
	    !load_module(plugins + "obs-outputs.dll", data + "obs-outputs") ||
	    !load_module(plugins + "rtmp-services.dll", data + "rtmp-services") ||
	    !load_module(plugins + "obs-x264.dll", data + "obs-x264") || !load_module(argv[2], argv[3]))
		return 5;
	load_module(plugins + "obs-nvenc.dll", data + "obs-nvenc", false);
	load_module(plugins + "obs-qsv11.dll", data + "obs-qsv11", false);
	register_probe_output();
	register_tone_source();

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
	{
		OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(green_item);
		obs_data_set_int(priv, "video_mixes_hidden", 1 << 1);
	}

	obs_source_t *fade = obs_source_create("fade_transition", "Fade", nullptr, nullptr);
	obs_transition_set(fade, obs_scene_get_source(main_scene));
	obs_set_output_source(0, fade);

	if (section("program") || section("recording") || section("outputs")) {
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
		if (section("recording")) {
			const int full = check_recording(rec_mix.video(), tmp ? tmp : ".", 1);
			const int half = check_recording(rec_mix.video(), tmp ? tmp : ".", 2);
			expect(full > 0 && half > 0 && std::abs(half * 2 - full) <= full / 5,
			       "fps divisor 2 encodes half the frames (" + std::to_string(full) + " vs " + std::to_string(half) +
				       ")");
		}
		if (section("outputs"))
			check_outputs(rec_mix.video(), tmp ? tmp : ".");

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

	if (section("filters")) {
		/* ---- scene filters and hide transitions inside a re-rendered scene ----
		 * FilteredMain: nested scene "Tinted" at (100,100) = white 200x200 + lime 100x100 (hidden in mix 1),
		 * with a color-multiply filter on Tinted; a cyan square at (400,40) that is hidden with a fade. */
		obs_source_t *white = color("white", 0xFFFFFFFF, 200, 200);
		obs_source_t *lime = color("lime", 0xFF00FF00, 100, 100);
		obs_source_t *cyan = color("cyan", 0xFFFFFF00, 60, 60);
		obs_scene_t *tinted = obs_scene_create("Tinted");
		place(tinted, white, 0, 0);
		obs_sceneitem_t *lime_item = place(tinted, lime, 0, 0);
		OBSDataAutoRelease lime_priv = obs_sceneitem_get_private_settings(lime_item);
		obs_data_set_int(lime_priv, "video_mixes_hidden", 1 << 1);

		OBSDataAutoRelease fs = obs_data_create();
		obs_data_set_int(fs, "color_multiply", 0xFF0000FF); /* ABGR red */
		obs_source_t *tint = obs_source_create("color_filter_v2", "tint", fs, nullptr);
		obs_source_filter_add(obs_scene_get_source(tinted), tint);

		obs_scene_t *filtered = obs_scene_create("FilteredMain");
		place(filtered, obs_scene_get_source(tinted), 100, 100);
		obs_sceneitem_t *cyan_item = place(filtered, cyan, 400, 40);
		obs_source_t *hide_fade = obs_source_create_private("fade_transition", "hide-fade", nullptr);
		obs_sceneitem_set_transition(cyan_item, false, hide_fade);
		obs_sceneitem_set_transition_duration(cyan_item, false, 3000);
		obs_transition_set(fade, obs_scene_get_source(filtered));

		MixUnderTest mix(ovi, W, H, nullptr);
		const std::vector<std::pair<int, int>> points{{150, 150}, {250, 250}, {430, 70}};
		Probe program(points), mixp(points);
		obs_output_t *program_out = attach_probe(program, obs_get_video(), "filter-program-probe");
		obs_output_t *mix_out = attach_probe(mixp, mix.video(), "filter-mix-probe");
		wait_ms(1500);

		std::printf("\nscene filters in a re-rendered scene:\n");
		expect(is(sample(program, 1), 255, 0, 0), "program: white tinted red by the scene filter", sample(program, 1));
		expect(is(sample(mixp, 1), 255, 0, 0), "mix: scene filter applied to the re-rendered scene", sample(mixp, 1));
		expect(is(sample(program, 0), 0, 0, 0), "program: lime x red = black", sample(program, 0));
		expect(is(sample(mixp, 0), 255, 0, 0), "mix: lime hidden, white below tinted red", sample(mixp, 0));

		obs_data_set_int(fs, "color_multiply", 0xFFFF0000); /* ABGR blue */
		obs_source_update(tint, fs);
		wait_ms(500);
		expect(is(sample(program, 1), 0, 0, 255), "program follows filter settings changes", sample(program, 1));
		expect(is(sample(mixp, 1), 0, 0, 255), "mix follows filter settings changes", sample(mixp, 1));
		obs_source_set_enabled(tint, false);
		wait_ms(500);
		expect(is(sample(mixp, 1), 255, 255, 255), "mix follows filter enable/disable", sample(mixp, 1));
		obs_source_set_enabled(tint, true);

		std::printf("\nshow/hide transitions in a re-rendered scene:\n");
		expect(is(sample(mixp, 2), 0, 255, 255), "cyan item visible before hiding", sample(mixp, 2));
		obs_sceneitem_set_visible(cyan_item, false);
		wait_ms(1500);
		const Pixel pp = sample(program, 2), mp = sample(mixp, 2);
		expect(mp.g > 40 && mp.g < 215 && std::abs(mp.g - pp.g) < 48,
		       "mid hide-fade: mix fades the item out like program (program g=" + std::to_string(pp.g) + ")");
		wait_ms(2500);
		expect(is(sample(mixp, 2), 0, 0, 0), "after the hide transition the item is gone", sample(mixp, 2));

		detach_probe(program_out);
		detach_probe(mix_out);
		obs_transition_set(fade, obs_scene_get_source(main_scene));
		for (obs_scene_t *scene : {filtered, tinted}) {
			obs_source_remove(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
		for (obs_source_t *s : {white, lime, cyan, tint, hide_fade})
			obs_source_release(s);
	}

	if (section("source")) {
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
	for (const PlacedCase &c : section("source") ? std::vector<PlacedCase>{PlacedCase{"cropped blue (120x120, 10px crop)", blue, 100, 100, 1, 0, 0, 255},
				    PlacedCase{"green 100x100 inside nested scene scaled 2x", green, 200, 200, 2, 0, 255, 0}}
									 : std::vector<PlacedCase>{}) {
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

	if (section("encoders"))
		check_encoders();
	if (section("audio"))
		check_source_audio();
	if (section("framerate"))
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
	wait_ms(100); /* queued UI tasks (retired filter copies, stop events) */
	obs_shutdown();
	std::printf("\nshutdown:\n");
	expect(bnum_allocs() == 0, "no libobs memory leaks (" + std::to_string(bnum_allocs()) + " allocations left)");

	std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
	return failures ? 1 : 0;
}
