#include "core/encoders.hpp"

#include <obs.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <tuple>

namespace vmix {

namespace {

struct QualityMode {
	const char *rate_control;
	const char *key; /* integer quality property for this mode */
	int value;
};

/* Preference order; values are visually near-lossless for 1080p recordings. */
constexpr std::array kQualityModes = {
	QualityMode{"CQP", "cqp", 20},           /* NVENC, AMF, QSV */
	QualityMode{"CRF", "crf", 18},           /* x264, SVT-AV1 */
	QualityMode{"CQVBR", "target_quality", 20}, /* NVENC */
	QualityMode{"ICQ", "icq_quality", 20},   /* QSV */
	QualityMode{"QVBR", "qvbr_quality", 20}, /* AMF */
};

uint32_t caps(const char *id)
{
	return obs_get_encoder_caps(id);
}

bool list_has(obs_property_t *list, const char *value)
{
	for (size_t i = 0, n = obs_property_list_item_count(list); i < n; i++) {
		const char *v = obs_property_list_item_string(list, i);
		if (v && std::strcmp(v, value) == 0 && !obs_property_list_item_disabled(list, i))
			return true;
	}
	return false;
}

void set_int_clamped(obs_properties_t *props, obs_data_t *settings, const char *key, int value)
{
	obs_property_t *p = obs_properties_get(props, key);
	if (!p || obs_property_get_type(p) != OBS_PROPERTY_INT)
		return;
	obs_data_set_int(settings, key, std::clamp(value, obs_property_int_min(p), obs_property_int_max(p)));
}

} // namespace

bool encoder_selectable(const char *id)
{
	if (!id || !*id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
		return false;
	return (caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) == 0;
}

std::vector<EncoderType> video_encoders()
{
	std::vector<EncoderType> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (!encoder_selectable(id))
			continue;
		const char *name = obs_encoder_get_display_name(id);
		out.push_back({id, name ? name : id, (caps(id) & OBS_ENCODER_CAP_PASS_TEXTURE) != 0});
	}

	auto key = [](const EncoderType &e) {
		const char *codec = obs_get_encoder_codec(e.id.c_str());
		const bool h264 = codec && std::strcmp(codec, "h264") == 0;
		return std::make_tuple(!e.hardware, !h264, e.id != "obs_x264");
	};
	std::stable_sort(out.begin(), out.end(), [&](const EncoderType &a, const EncoderType &b) { return key(a) < key(b); });
	return out;
}

std::string preferred_video_encoder()
{
	auto all = video_encoders();
	return all.empty() ? std::string("obs_x264") : all.front().id;
}

bool apply_quality_defaults(const char *encoder_id, obs_data_t *settings)
{
	OBSProperties props = obs_get_encoder_properties(encoder_id);
	if (!props)
		return false;
	obs_properties_apply_settings(props, settings);

	if (obs_properties_get(props, "keyint_sec"))
		obs_data_set_int(settings, "keyint_sec", 2);

	obs_property_t *rc = obs_properties_get(props, "rate_control");
	if (!rc || obs_property_get_type(rc) != OBS_PROPERTY_LIST || obs_property_list_format(rc) != OBS_COMBO_FORMAT_STRING)
		return false;

	for (const QualityMode &mode : kQualityModes) {
		if (!list_has(rc, mode.rate_control))
			continue;
		obs_data_set_string(settings, "rate_control", mode.rate_control);
		/* Re-run modified callbacks so mode-specific properties (and their ranges) become visible. */
		obs_property_modified(rc, settings);
		set_int_clamped(props, settings, mode.key, mode.value);
		return true;
	}
	return false;
}

obs_data_t *recording_defaults(const char *encoder_id)
{
	obs_data_t *s = obs_data_create();
	if (encoder_id && std::strcmp(encoder_id, "obs_x264") == 0)
		obs_data_set_string(s, "preset", "veryfast");
	apply_quality_defaults(encoder_id, s);
	return s;
}

obs_data_t *stream_defaults(const char *encoder_id, int bitrate_kbps)
{
	obs_data_t *s = obs_data_create();
	if (encoder_id && std::strcmp(encoder_id, "obs_x264") == 0)
		obs_data_set_string(s, "preset", "veryfast");

	OBSProperties props = obs_get_encoder_properties(encoder_id);
	if (!props)
		return s;
	obs_properties_apply_settings(props, s);
	if (obs_properties_get(props, "keyint_sec"))
		obs_data_set_int(s, "keyint_sec", 2);
	obs_property_t *rc = obs_properties_get(props, "rate_control");
	if (rc && obs_property_get_type(rc) == OBS_PROPERTY_LIST && obs_property_list_format(rc) == OBS_COMBO_FORMAT_STRING &&
	    list_has(rc, "CBR")) {
		obs_data_set_string(s, "rate_control", "CBR");
		obs_property_modified(rc, s);
	}
	set_int_clamped(props, s, "bitrate", bitrate_kbps);
	return s;
}

namespace {

size_t user_value_count(obs_data_t *d)
{
	size_t n = 0;
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it))
		n += obs_data_item_has_user_value(it) ? 1 : 0;
	return n;
}

bool item_equals(obs_data_item_t *a, obs_data_t *other)
{
	const char *name = obs_data_item_get_name(a);
	if (!obs_data_has_user_value(other, name))
		return false;
	obs_data_item_t *b = obs_data_item_byname(other, name);
	bool equal = false;
	if (b && obs_data_item_gettype(a) == obs_data_item_gettype(b)) {
		switch (obs_data_item_gettype(a)) {
		case OBS_DATA_STRING:
			equal = std::strcmp(obs_data_item_get_string(a), obs_data_item_get_string(b)) == 0;
			break;
		case OBS_DATA_NUMBER:
			equal = obs_data_item_numtype(a) == OBS_DATA_NUM_INT
					? obs_data_item_get_int(a) == obs_data_item_get_int(b)
					: obs_data_item_get_double(a) == obs_data_item_get_double(b);
			break;
		case OBS_DATA_BOOLEAN:
			equal = obs_data_item_get_bool(a) == obs_data_item_get_bool(b);
			break;
		default:
			break; /* objects/arrays do not occur in encoder settings: treat as different */
		}
	}
	obs_data_item_release(&b);
	return equal;
}

} // namespace

bool same_settings(obs_data_t *a, obs_data_t *b)
{
	if (!a || !b || user_value_count(a) != user_value_count(b))
		return false;
	for (obs_data_item_t *it = obs_data_first(a); it; obs_data_item_next(&it)) {
		if (obs_data_item_has_user_value(it) && !item_equals(it, b)) {
			obs_data_item_release(&it);
			return false;
		}
	}
	return true;
}

bool uses_constant_bitrate(const char *encoder_id, obs_data_t *settings)
{
	/* Unset keys fall back to the encoder's defaults, which are often CBR. */
	OBSDataAutoRelease effective = obs_encoder_defaults(encoder_id);
	if (!effective)
		return false;
	obs_data_apply(effective, settings);
	const char *rc = obs_data_get_string(effective, "rate_control");
	return rc && (std::strcmp(rc, "CBR") == 0 || std::strcmp(rc, "ABR") == 0);
}

} // namespace vmix
