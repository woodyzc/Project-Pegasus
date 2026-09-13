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

void Map_PrepareProjection(MapProjection_t *proj, double center_lat, double center_lon,
                           double metres_per_pixel, int16_t center_x, int16_t center_y) {
    if (proj == NULL) {
        return;
    }
    proj->center_lat = center_lat;
    proj->center_lon = center_lon;
    proj->center_x = center_x;
    proj->center_y = center_y;
    proj->valid = (metres_per_pixel > 0.0);
    if (!proj->valid) {
        proj->px_per_deg_lon = 0.0;
        proj->px_per_deg_lat = 0.0;
        return;
    }

    /* cos() of the VIEW centre, not of each point: using the point's own
       latitude would scale every row differently and shear the map. Folding
       the division in here is what makes the per-point path multiply-only. */
    const double lon_scale = cos(center_lat * M_PI / 180.0);
    proj->px_per_deg_lon = MAP_EARTH_METRES_PER_DEGREE * lon_scale / metres_per_pixel;
    proj->px_per_deg_lat = MAP_EARTH_METRES_PER_DEGREE / metres_per_pixel;
}

void Map_ProjectPrepared(const MapProjection_t *proj, double lat, double lon, int16_t *out_x,
                         int16_t *out_y) {
    if (proj == NULL || out_x == NULL || out_y == NULL) {
        return;
    }
    if (!proj->valid) {
        *out_x = proj->center_x;
        *out_y = proj->center_y;
        return;
    }
    *out_x = ClampCoord((double)proj->center_x + (lon - proj->center_lon) * proj->px_per_deg_lon);
    /* Screen y grows downward while latitude grows north, hence the sign. */
    *out_y = ClampCoord((double)proj->center_y - (lat - proj->center_lat) * proj->px_per_deg_lat);
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
        /* Delegates, so there is one projection rather than two that agree
           until someone edits one of them. */
        MapProjection_t proj;
        Map_PrepareProjection(&proj, center_lat, center_lon, metres_per_pixel, center_x, center_y);
        Map_ProjectPrepared(&proj, lat, lon, out_x, out_y);
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

/* Whether a projected point lies inside the viewport, with a margin so a line
   entering from just off the edge still has a point to come from.

   The viewport is 2*center_x by 2*center_y: both callers pass half the widget's
   size, which is what puts the map's centre in the middle of it. */
static int Map_PointVisible(int16_t x, int16_t y, int16_t center_x, int16_t center_y) {
    const int32_t margin = 8;
    return x >= -margin && y >= -margin && x <= (int32_t)center_x * 2 + margin &&
           y <= (int32_t)center_y * 2 + margin;
}

size_t Map_BuildPolyline(const TrackBuffer_t *track, double center_lat, double center_lon,
                         double metres_per_pixel, int16_t center_x, int16_t center_y,
                         MapPoint_t *out, size_t max_points) {
    size_t written = 0;
    size_t i;
    size_t first = 0;
    size_t last = 0;
    size_t span;
    size_t stride;
    int found = 0;
    int16_t last_x = 0;
    int16_t last_y = 0;

    if (track == NULL || out == NULL || max_points == 0 || track->count == 0) {
        return 0;
    }

    /* First pass: the range of the track that is anywhere near the viewport.
       Only this stretch is drawn.

       The walk used to start at the track's beginning and stop when the buffer
       filled, which at any zoom needing more points than the buffer holds drew
       the START of the track and nothing where the rider was -- an empty map.
       Collapsing off-screen runs instead was worse: merging an excursion into
       one point draws a straight chord from where the track left the screen to
       where it came back, cutting across a map it never crosses. */
    for (i = 0; i < track->count; i++) {
        double lat;
        double lon;
        int16_t x;
        int16_t y;

        if (!TrackBuffer_Get(track, i, &lat, &lon)) {
            continue;
        }
        Map_Project(lat, lon, center_lat, center_lon, metres_per_pixel, center_x, center_y, &x, &y);
        if (!Map_PointVisible(x, y, center_x, center_y)) {
            continue;
        }
        if (!found) {
            first = i;
            found = 1;
        }
        last = i;
    }

    if (!found) {
        return 0; /* The track is somewhere else entirely. */
    }

    /* One point either side, so the line enters and leaves the viewport from
       the direction it really comes from rather than starting at the edge. */
    if (first > 0) {
        first--;
    }
    if (last + 1 < track->count) {
        last++;
    }

    /* Second pass over that range. If it holds more points than the buffer,
       take every Nth: a thinned line keeps the shape, where stopping partway
       through would draw half a route and call it the whole one. */
    span = last - first + 1;
    stride = (span + max_points - 1) / max_points;
    if (stride == 0) {
        stride = 1;
    }

    for (i = first; i <= last; i += stride) {
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
