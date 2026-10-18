#include "core/source-audio.hpp"

#include <obs-module.h>
#include <media-io/audio-io.h>

#include <algorithm>
#include <cmath>

namespace vmix {

namespace {

/* Read position behind real time: covers the delivery jitter of capture devices, browsers and media sources. */
constexpr uint64_t kDelayNs = 300'000'000ULL;

/* Timestamps closer than this to the expected position are treated as continuous (libobs uses 70 ms too). */
constexpr uint64_t kSmoothingNs = 70'000'000ULL;

/* Larger forward gaps are not filled with silence; the timeline restarts instead. */
constexpr uint64_t kMaxGapNs = 5'000'000'000ULL;

int64_t frames_of(uint32_t rate, int64_t ns)
{
	return ns >= 0 ? (int64_t)ns_to_audio_frames(rate, (uint64_t)ns) : -(int64_t)ns_to_audio_frames(rate, (uint64_t)-ns);
}

} // namespace

std::unique_ptr<SourceAudio> SourceAudio::create(obs_source_t *source, std::string &error)
{
	obs_audio_info oai;
	if (!source || !obs_get_audio_info(&oai)) {
		error = obs_module_text("Error.Audio");
		return nullptr;
	}

	std::unique_ptr<SourceAudio> sa(new SourceAudio());
	sa->source_ = source;
	sa->rate_ = oai.samples_per_sec;
	sa->channels_ = get_audio_channels(oai.speakers);
	sa->buffer_.resize(sa->channels_);

	audio_output_info info = {};
	info.name = "video-mixes source audio";
	info.samples_per_sec = oai.samples_per_sec;
	info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	info.speakers = oai.speakers;
	info.input_callback = &SourceAudio::on_input;
	info.input_param = sa.get();
	if (audio_output_open(&sa->audio_, &info) != AUDIO_OUTPUT_SUCCESS) {
		error = obs_module_text("Error.Audio");
		return nullptr;
	}

	obs_source_add_audio_capture_callback(source, &SourceAudio::on_capture, sa.get());
	return sa;
}

SourceAudio::~SourceAudio()
{
	if (source_)
		obs_source_remove_audio_capture_callback(source_, &SourceAudio::on_capture, this);
	if (audio_)
		audio_output_close(audio_);
}

void SourceAudio::reset(uint64_t ts)
{
	for (auto &ch : buffer_)
		ch.clear();
	base_ts_ = ts;
	front_ = 0;
	timed_ = true;
}

int64_t SourceAudio::index_of(uint64_t ts) const
{
	return frames_of(rate_, (int64_t)(ts - base_ts_));
}

uint64_t SourceAudio::ts_of(int64_t index) const
{
	return index >= 0 ? base_ts_ + audio_frames_to_ns(rate_, (uint64_t)index)
			  : base_ts_ - audio_frames_to_ns(rate_, (uint64_t)-index);
}

void SourceAudio::pop_front(size_t frames)
{
	frames = std::min(frames, buffer_[0].size());
	for (auto &ch : buffer_)
		ch.erase(ch.begin(), ch.begin() + (std::ptrdiff_t)frames);
	front_ += (int64_t)frames;
}

/* Source audio thread. */
void SourceAudio::on_capture(void *param, obs_source_t *source, const audio_data *data, bool muted)
{
	auto *self = static_cast<SourceAudio *>(param);
	if (!data || !data->frames)
		return;

	const uint64_t ts = data->timestamp + (uint64_t)obs_source_get_sync_offset(source);
	const float gain = muted ? 0.0f : obs_source_get_volume(source);

	std::lock_guard lock(self->mutex_);
	auto &buf = self->buffer_;
	if (!self->timed_)
		self->reset(ts);

	const int64_t end = self->front_ + (int64_t)buf[0].size();
	const uint64_t expected = self->ts_of(end);
	if (ts > expected + kSmoothingNs) {
		if (ts - expected > kMaxGapNs) {
			self->reset(ts);
		} else {
			const int64_t gap = self->index_of(ts) - end;
			for (auto &ch : buf)
				ch.insert(ch.end(), (size_t)std::max<int64_t>(0, gap), 0.0f);
		}
	} else if (ts + kSmoothingNs < expected) {
		self->reset(ts); /* timestamps jumped backwards */
	}

	for (size_t c = 0; c < self->channels_; c++) {
		const float *in = reinterpret_cast<const float *>(data->data[c]);
		auto &ch = buf[c];
		if (!in) {
			ch.insert(ch.end(), data->frames, 0.0f);
			continue;
		}
		for (uint32_t i = 0; i < data->frames; i++)
			ch.push_back(in[i] * gain);
	}
}

/* audio-io thread: one AUDIO_OUTPUT_FRAMES block ending kDelayNs before now. */
bool SourceAudio::on_input(void *param, uint64_t start_ts, uint64_t, uint64_t *new_ts, uint32_t,
			   audio_output_data *mixes)
{
	auto *self = static_cast<SourceAudio *>(param);
	const uint64_t window_ts = start_ts - kDelayNs;
	*new_ts = window_ts;

	std::lock_guard lock(self->mutex_);
	if (!self->timed_)
		return true; /* silence until the source delivers audio */

	const int64_t window = self->index_of(window_ts);
	if (self->front_ < window)
		self->pop_front((size_t)(window - self->front_));
	if (self->buffer_[0].empty() && self->front_ < window)
		self->front_ = window; /* nothing buffered: keep the timeline at the read position */

	const int64_t offset = self->front_ - window; /* leading silence if the buffer starts later */
	for (size_t c = 0; c < self->channels_ && c < MAX_AV_PLANES; c++) {
		float *out = reinterpret_cast<float *>(mixes[0].data[c]);
		const auto &ch = self->buffer_[c];
		for (int64_t i = std::max<int64_t>(0, offset); i < AUDIO_OUTPUT_FRAMES; i++) {
			const size_t at = (size_t)(i - offset);
			if (at >= ch.size())
				break;
			out[i] = ch[at];
		}
	}
	self->pop_front((size_t)std::max<int64_t>(0, AUDIO_OUTPUT_FRAMES - offset));
	return true;
}

} // namespace vmix
