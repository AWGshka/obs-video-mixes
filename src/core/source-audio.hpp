#pragma once

#include <obs.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vmix {

/*
 * One source's own audio as an audio_t that encoders can use like an OBS mixer track: the source's audio after its
 * filters, with its volume and mute applied, and nothing else mixed in.
 *
 * Captured blocks are placed on a sample timeline by their timestamps (plus the source's sync offset) and read back
 * a fixed delay behind real time, so the encoded audio keeps the same clock as the mix video. Gaps become silence.
 */
class SourceAudio {
public:
	static std::unique_ptr<SourceAudio> create(obs_source_t *source, std::string &error);
	~SourceAudio();

	SourceAudio(const SourceAudio &) = delete;
	SourceAudio &operator=(const SourceAudio &) = delete;

	audio_t *audio() const { return audio_; }

private:
	SourceAudio() = default;

	static void on_capture(void *param, obs_source_t *source, const audio_data *data, bool muted);
	static bool on_input(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts, uint32_t mixers,
			     audio_output_data *mixes);

	void reset(uint64_t ts);
	int64_t index_of(uint64_t ts) const;
	uint64_t ts_of(int64_t index) const;
	void pop_front(size_t frames);

	OBSSource source_;
	audio_t *audio_ = nullptr;
	uint32_t rate_ = 48000;
	size_t channels_ = 2;

	std::mutex mutex_;
	std::vector<std::deque<float>> buffer_; /* per channel; buffer_[c][0] is sample front_ */
	bool timed_ = false;
	uint64_t base_ts_ = 0;  /* timestamp of sample index 0 */
	int64_t front_ = 0;     /* index of the first buffered sample */
};

} // namespace vmix
