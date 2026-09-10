#include "MapProject.h"

#include <math.h>

static int16_t ClampCoord(double value) {
    if (value > (double)MAP_COORD_LIMIT) {
        return (int16_t)MAP_COORD_LIMIT;
    }
    if (value < -(double)MAP_COORD_LIMIT) {
        return (int16_t)(-MAP_COORD_LIMIT);
    }
    /* Round rather than truncate so a trail does not drift half a pixel
       toward the view centre. */
    return (int16_t)(value + (value >= 0.0 ? 0.5 : -0.5));
}

void Map_Project(double lat, double lon, double center_lat, double center_lon,
                 double metres_per_pixel, int16_t center_x, int16_t center_y,
                 int16_t *out_x, int16_t *out_y) {
    if (out_x == NULL || out_y == NULL) {
        return;
    }
    if (metres_per_pixel <= 0.0) {
        *out_x = center_x;
        *out_y = center_y;
        return;
    }

    {
        /* cos() of the VIEW centre, not of each point: using the point's own
           latitude would scale every row differently and shear the trail. */
        const double lon_scale = cos(center_lat * M_PI / 180.0);
        const double dx_m = (lon - center_lon) * MAP_EARTH_METRES_PER_DEGREE * lon_scale;
        const double dy_m = (lat - center_lat) * MAP_EARTH_METRES_PER_DEGREE;

        *out_x = ClampCoord((double)center_x + dx_m / metres_per_pixel);
        /* Screen y grows downward while latitude grows north, hence the sign. */
        *out_y = ClampCoord((double)center_y - dy_m / metres_per_pixel);
    }
}

double Map_FitScale(double min_lat, double max_lat, double min_lon, double max_lon,
                    int16_t width, int16_t height, int16_t margin_px) {
    /* Roughly 30cm per pixel: close enough to see a driveway, and the floor
       that stops a stationary rider's zero-extent track from dividing by
       zero or zooming to atomic scale. */
    const double MIN_SCALE = 0.3;

    double usable_w = (double)width - 2.0 * (double)margin_px;
    double usable_h = (double)height - 2.0 * (double)margin_px;
    double mid_lat;
    double span_lat_m;
    double span_lon_m;
    double scale_x;
    double scale_y;
    double scale;

    if (usable_w < 1.0) {
        usable_w = 1.0;
    }
    if (usable_h < 1.0) {
        usable_h = 1.0;
    }

    mid_lat = (min_lat + max_lat) / 2.0;
    span_lat_m = (max_lat - min_lat) * MAP_EARTH_METRES_PER_DEGREE;
    span_lon_m = (max_lon - min_lon) * MAP_EARTH_METRES_PER_DEGREE *
                 cos(mid_lat * M_PI / 180.0);

    if (span_lat_m < 0.0) {
        span_lat_m = -span_lat_m;
    }
    if (span_lon_m < 0.0) {
        span_lon_m = -span_lon_m;
    }

    scale_x = span_lon_m / usable_w;
    scale_y = span_lat_m / usable_h;

    /* The larger of the two: whichever axis is tighter decides the zoom, or
       the trail spills off the other edge. */
    scale = (scale_x > scale_y) ? scale_x : scale_y;

    return (scale < MIN_SCALE) ? MIN_SCALE : scale;
}

size_t Map_BuildPolyline(const TrackBuffer_t *track, double center_lat, double center_lon,
                         double metres_per_pixel, int16_t center_x, int16_t center_y,
                         MapPoint_t *out, size_t max_points) {
    size_t written = 0;
    size_t i;
    int16_t last_x = 0;
    int16_t last_y = 0;

    if (track == NULL || out == NULL || max_points == 0) {
        return 0;
    }

    for (i = 0; i < track->count; i++) {
        double lat;
        double lon;
        int16_t x;
        int16_t y;

        if (!TrackBuffer_Get(track, i, &lat, &lon)) {
            continue;
        }

        Map_Project(lat, lon, center_lat, center_lon, metres_per_pixel, center_x, center_y, &x, &y);

        /* Collapse runs that land on one pixel. A receiver logging at 1Hz
           while the rider waits at a light emits hundreds of points that draw
           nothing new. */
        if (written > 0 && x == last_x && y == last_y) {
            continue;
        }

        out[written].x = x;
        out[written].y = y;
        last_x = x;
        last_y = y;
        written++;

        if (written >= max_points) {
            break;
        }
    }
    return written;
}
