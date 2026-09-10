#include "TrackBuffer.h"

static int32_t ToE7(double degrees) {
    /* Round rather than truncate: truncation biases every coordinate toward
       the equator and prime meridian, which over a long trail shows up as a
       consistent drift rather than as noise. */
    return (int32_t)((degrees * 1e7) + (degrees >= 0.0 ? 0.5 : -0.5));
}

void TrackBuffer_Init(TrackBuffer_t *track, int32_t *lat_store, int32_t *lon_store,
                      size_t capacity) {
    if (track == NULL) {
        return;
    }
    track->lat_e7 = lat_store;
    track->lon_e7 = lon_store;
    track->capacity = (lat_store != NULL && lon_store != NULL) ? capacity : 0;
    track->count = 0;
    track->stride = 1;
    track->skipped = 0;
    track->has_bounds = false;
    track->min_lat_e7 = 0;
    track->max_lat_e7 = 0;
    track->min_lon_e7 = 0;
    track->max_lon_e7 = 0;
}

/* Keeps every second stored point, compacting in place. Index 0 is always
   kept, so the start of the trail never moves. */
static void Halve(TrackBuffer_t *track) {
    size_t read;
    size_t write = 0;

    for (read = 0; read < track->count; read += 2) {
        track->lat_e7[write] = track->lat_e7[read];
        track->lon_e7[write] = track->lon_e7[read];
        write++;
    }
    track->count = write;
    track->stride *= 2;
    track->skipped = 0;
}

void TrackBuffer_Add(TrackBuffer_t *track, double lat_deg, double lon_deg) {
    int32_t lat;
    int32_t lon;

    if (track == NULL || track->capacity == 0) {
        return;
    }

    lat = ToE7(lat_deg);
    lon = ToE7(lon_deg);

    if (!track->has_bounds) {
        track->has_bounds = true;
        track->min_lat_e7 = lat;
        track->max_lat_e7 = lat;
        track->min_lon_e7 = lon;
        track->max_lon_e7 = lon;
    } else {
        if (lat < track->min_lat_e7) {
            track->min_lat_e7 = lat;
        }
        if (lat > track->max_lat_e7) {
            track->max_lat_e7 = lat;
        }
        if (lon < track->min_lon_e7) {
            track->min_lon_e7 = lon;
        }
        if (lon > track->max_lon_e7) {
            track->max_lon_e7 = lon;
        }
    }

    /* Thinning: take the first of each stride and drop the rest. */
    if (track->skipped != 0) {
        track->skipped++;
        if (track->skipped >= track->stride) {
            track->skipped = 0;
        }
        return;
    }

    if (track->count >= track->capacity) {
        Halve(track);
    }

    track->lat_e7[track->count] = lat;
    track->lon_e7[track->count] = lon;
    track->count++;

    if (track->stride > 1) {
        track->skipped = 1;
    }
}

bool TrackBuffer_Get(const TrackBuffer_t *track, size_t index, double *out_lat, double *out_lon) {
    if (track == NULL || out_lat == NULL || out_lon == NULL || index >= track->count) {
        return false;
    }
    *out_lat = (double)track->lat_e7[index] * 1e-7;
    *out_lon = (double)track->lon_e7[index] * 1e-7;
    return true;
}

bool TrackBuffer_Center(const TrackBuffer_t *track, double *out_lat, double *out_lon) {
    if (track == NULL || out_lat == NULL || out_lon == NULL || !track->has_bounds) {
        return false;
    }
    /* Averaged as int64 before scaling: two coordinates near the same extreme
       would overflow int32 if added directly. */
    *out_lat = (double)(((int64_t)track->min_lat_e7 + (int64_t)track->max_lat_e7) / 2) * 1e-7;
    *out_lon = (double)(((int64_t)track->min_lon_e7 + (int64_t)track->max_lon_e7) / 2) * 1e-7;
    return true;
}
