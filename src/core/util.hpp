#pragma once

#include <obs.hpp>

namespace vmix {

/* Wraps a +1 reference returned by a libobs *_create / *_get_* call into a shared ref holder. */
inline OBSData adopt(obs_data_t *d)
{
	OBSData r(d);
	obs_data_release(d);
	return r;
}

inline OBSDataArray adopt(obs_data_array_t *a)
{
	OBSDataArray r(a);
	obs_data_array_release(a);
	return r;
}

inline OBSData new_data()
{
	return adopt(obs_data_create());
}

inline OBSDataArray new_array()
{
	return adopt(obs_data_array_create());
}

} // namespace vmix
