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

/* Same rounding as ClampCoord, without a double in it. */
static int16_t ClampCoordF(float value) {
    if (value > (float)MAP_COORD_LIMIT) {
        return (int16_t)MAP_COORD_LIMIT;
    }
    if (value < -(float)MAP_COORD_LIMIT) {
        return (int16_t)(-MAP_COORD_LIMIT);
    }
    return (int16_t)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

/* A centre in 1e-7 degrees, split into the nearest whole unit and what is left.
   Clamped to what an int32 holds: a view panned past the antimeridian can
   carry a longitude beyond 180, and nothing there is drawn anyway. */
static void SplitE7(double degrees, int32_t *out_whole, float *out_frac) {
    double scaled = degrees * 1e7;
    double whole;
    if (scaled > 2.0e9) {
        scaled = 2.0e9;
    } else if (scaled < -2.0e9) {
        scaled = -2.0e9;
    }
    whole = floor(scaled + 0.5);
    *out_whole = (int32_t)whole;
    *out_frac = (float)(scaled - whole);
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
    /* North-up unless asked otherwise, so every existing caller is unchanged
       and Map_ProjectPrepared still matches Map_Project point for point. */
    proj->cos_h = 1.0;
    proj->sin_h = 0.0;
    proj->cos_h_f = 1.0f;
    proj->sin_h_f = 0.0f;
    proj->rotated = false;
    SplitE7(center_lat, &proj->center_lat_e7, &proj->center_lat_frac_e7);
    SplitE7(center_lon, &proj->center_lon_e7, &proj->center_lon_frac_e7);
    proj->valid = (metres_per_pixel > 0.0);
    if (!proj->valid) {
        proj->px_per_deg_lon = 0.0;
        proj->px_per_deg_lat = 0.0;
        proj->px_per_deg_lon_f = 0.0f;
        proj->px_per_deg_lat_f = 0.0f;
        proj->px_per_e7_lon_f = 0.0f;
        proj->px_per_e7_lat_f = 0.0f;
        return;
    }

    /* cos() of the VIEW centre, not of each point: using the point's own
       latitude would scale every row differently and shear the map. Folding
       the division in here is what makes the per-point path multiply-only. */
    const double lon_scale = cos(center_lat * M_PI / 180.0);
    proj->px_per_deg_lon = MAP_EARTH_METRES_PER_DEGREE * lon_scale / metres_per_pixel;
    proj->px_per_deg_lat = MAP_EARTH_METRES_PER_DEGREE / metres_per_pixel;

    /* Mirrored once per frame so the per-point path never touches a double
       multiply. Kept alongside the doubles rather than replacing them: the
       doubles are what Map_Project and the tests compare against, and the
       pair agreeing is the thing worth preserving. */
    proj->px_per_deg_lon_f = (float)proj->px_per_deg_lon;
    proj->px_per_deg_lat_f = (float)proj->px_per_deg_lat;
    proj->px_per_e7_lon_f = (float)(proj->px_per_deg_lon * 1e-7);
    proj->px_per_e7_lat_f = (float)(proj->px_per_deg_lat * 1e-7);
}

void Map_SetProjectionHeading(MapProjection_t *proj, double heading_deg) {
    if (proj == NULL) {
        return;
    }
    const double radians = heading_deg * M_PI / 180.0;
    proj->cos_h = cos(radians);
    proj->sin_h = sin(radians);
    /* A heading of zero is north-up, which is the untransformed case, so it
       takes the cheap path rather than multiplying by an identity. */
    proj->rotated = (proj->sin_h != 0.0 || proj->cos_h != 1.0);
    proj->cos_h_f = (float)proj->cos_h;
    proj->sin_h_f = (float)proj->sin_h;
}

double Map_RotatedRadiusPx(int16_t width, int16_t height) {
    const double w = (double)width * 0.5;
    const double h = (double)height * 0.5;
    return sqrt(w * w + h * h);
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
    /* Subtract in double, scale in float. See the note on px_per_deg_lon_f:
       the coordinates need double, the small difference does not, and the
       difference is where the arithmetic is. */
    const float dlon = (float)(lon - proj->center_lon);
    const float dlat = (float)(lat - proj->center_lat);

    const float dx = dlon * proj->px_per_deg_lon_f;
    /* Screen y grows downward while latitude grows north, hence the sign. */
    const float dy = -dlat * proj->px_per_deg_lat_f;

    if (!proj->rotated) {
        *out_x = ClampCoord((double)proj->center_x + (double)dx);
        *out_y = ClampCoord((double)proj->center_y + (double)dy);
        return;
    }

    /* Rotate the offset by MINUS the heading, which is what puts the heading
       at the top of the screen rather than at the right of it. Worked through
       for heading 90: a point due east has (dx, dy) = (r, 0) and comes out at
       (0, -r), which is straight up. */
    *out_x = ClampCoord((double)proj->center_x +
                        (double)(dx * proj->cos_h_f + dy * proj->sin_h_f));
    *out_y = ClampCoord((double)proj->center_y +
                        (double)(-dx * proj->sin_h_f + dy * proj->cos_h_f));
}

void Map_ProjectE7Prepared(const MapProjection_t *proj, int32_t lat_e7, int32_t lon_e7,
                           int16_t *out_x, int16_t *out_y) {
    if (proj == NULL || out_x == NULL || out_y == NULL) {
        return;
    }
    if (!proj->valid) {
        *out_x = proj->center_x;
        *out_y = proj->center_y;
        return;
    }
    {
        /* Exact integer differences. Through uint32 so a longitude far across
           the antimeridian wraps rather than overflowing: anything that wraps
           is at least 69 degrees away and lands off the screen either way.
           Exact in float below 2^24 units -- 1.6 degrees, wider than any view
           this draws -- and within float precision beyond. */
        const int32_t dlat_i = (int32_t)((uint32_t)lat_e7 - (uint32_t)proj->center_lat_e7);
        const int32_t dlon_i = (int32_t)((uint32_t)lon_e7 - (uint32_t)proj->center_lon_e7);
        const float dlat = (float)dlat_i - proj->center_lat_frac_e7;
        const float dlon = (float)dlon_i - proj->center_lon_frac_e7;

        const float dx = dlon * proj->px_per_e7_lon_f;
        /* Screen y grows downward while latitude grows north, hence the sign. */
        const float dy = -dlat * proj->px_per_e7_lat_f;

        if (!proj->rotated) {
            *out_x = ClampCoordF((float)proj->center_x + dx);
            *out_y = ClampCoordF((float)proj->center_y + dy);
            return;
        }
        /* The same rotation as Map_ProjectPrepared, by minus the heading. */
        *out_x = ClampCoordF((float)proj->center_x + (dx * proj->cos_h_f + dy * proj->sin_h_f));
        *out_y = ClampCoordF((float)proj->center_y + (-dx * proj->sin_h_f + dy * proj->cos_h_f));
    }
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

/* ---- Finding the drawn stretch without projecting the whole track ----

   Only two numbers come out of the search below: the first and last track
   index near the viewport. It used to find them by projecting EVERY point and
   testing each one, which on a 20,000-point route is twenty thousand
   software-double projections to produce two indices -- against the ~256 the
   second pass actually draws. 98% of the arithmetic existed in order to be
   thrown away, and it ran on every LV_EVENT_PRESSING: about 33 times a second
   while the finger is down.

   Two changes, and together they leave the ANSWER bit-identical:

   - Scan inward from both ends rather than across the middle. `first` is the
     lowest visible index and `last` the highest, so a forward walk can stop
     at its first hit and a backward walk at its first. A whole route framed
     on screen -- the zoomed-out case -- now costs two projections instead of
     twenty thousand.
   - Reject by a box in SOURCE coordinates before projecting. The viewport
     covers a bounded range of latitude and longitude, so a point outside that
     range cannot be visible and is dismissed with four integer compares
     against the raw e7 values the buffer already holds. Same trade RoadView
     makes for the road layer, for the same reason: a projection here is soft
     float and an integer compare is not.

   ⚠️ The box must be CONSERVATIVE. It may admit a point the exact test then
   rejects; it must never reject one the exact test would admit, or the scans
   would walk straight past the true first or last and the drawn stretch would
   be wrong rather than merely slower. Rotation is covered by taking the
   half-diagonal in place of the half-extents: turning the view preserves a
   point's distance from the centre, so the circumscribed radius bounds every
   angle at once. Candidates are still projected and still go through
   Map_PointVisible, so what this changes is how many points reach that test,
   never which of them pass it. */

#define MAP_E7_LIMIT 2000000000

typedef struct {
    int32_t min_lat_e7;
    int32_t max_lat_e7;
    int32_t min_lon_e7;
    int32_t max_lon_e7;
    int usable;
} MapSourceBox_t;

static int32_t Map_ClampE7(double value) {
    if (value > (double)MAP_E7_LIMIT) {
        return (int32_t)MAP_E7_LIMIT;
    }
    if (value < -(double)MAP_E7_LIMIT) {
        return (int32_t)(-MAP_E7_LIMIT);
    }
    return (int32_t)value;
}

static void Map_SourceBox(const MapProjection_t *proj, MapSourceBox_t *box) {
    box->min_lat_e7 = 0;
    box->max_lat_e7 = 0;
    box->min_lon_e7 = 0;
    box->max_lon_e7 = 0;
    box->usable = 0;

    if (!proj->valid || proj->px_per_deg_lat < 1e-9 || proj->px_per_deg_lon < 1e-9) {
        return; /* No usable scale: no box, and every point is a candidate. */
    }

    {
        /* Map_PointVisible's own 8px margin, plus one for the half-pixel
           rounding in ClampCoord. Generous costs a few extra candidates;
           tight costs correctness. */
        const double margin = 9.0;
        double ex = (double)proj->center_x + margin;
        double ey = (double)proj->center_y + margin;
        double dlat_e7;
        double dlon_e7;

        if (proj->rotated) {
            const double r = sqrt(ex * ex + ey * ey);
            ex = r;
            ey = r;
        }

        dlat_e7 = (ey / proj->px_per_deg_lat) * 1e7 + 1.0;
        dlon_e7 = (ex / proj->px_per_deg_lon) * 1e7 + 1.0;

        box->min_lat_e7 = Map_ClampE7(proj->center_lat * 1e7 - dlat_e7);
        box->max_lat_e7 = Map_ClampE7(proj->center_lat * 1e7 + dlat_e7);
        box->min_lon_e7 = Map_ClampE7(proj->center_lon * 1e7 - dlon_e7);
        box->max_lon_e7 = Map_ClampE7(proj->center_lon * 1e7 + dlon_e7);
        box->usable = 1;
    }
}

/* Reads the raw stored coordinate. The caller guarantees index < count, which
   is why this indexes the arrays rather than going through TrackBuffer_Get:
   Get converts to double on the way out, and in this loop that conversion is
   two software multiplies per point for a value most points never need. */
static int Map_IndexVisible(const TrackBuffer_t *track, const MapProjection_t *proj,
                            const MapSourceBox_t *box, size_t index) {
    const int32_t lat_e7 = track->lat_e7[index];
    const int32_t lon_e7 = track->lon_e7[index];
    int16_t x = 0;
    int16_t y = 0;

    if (box->usable && (lat_e7 < box->min_lat_e7 || lat_e7 > box->max_lat_e7 ||
                        lon_e7 < box->min_lon_e7 || lon_e7 > box->max_lon_e7)) {
        return 0;
    }

    /* Culling compares against the ROTATED screen position, so a track that
       leaves the top of a turned view is culled at the top -- which it would
       not be if visibility were tested before the rotation. The box above is
       the opposite case on purpose: it is pre-rotation, which is exactly why
       it has to be the circumscribed one. */
    Map_ProjectPrepared(proj, (double)lat_e7 * 1e-7, (double)lon_e7 * 1e-7, &x, &y);
    return Map_PointVisible(x, y, proj->center_x, proj->center_y);
}

size_t Map_BuildPolyline(const TrackBuffer_t *track, double center_lat, double center_lon,
                         double metres_per_pixel, int16_t center_x, int16_t center_y,
                         MapPoint_t *out, size_t max_points) {
    MapProjection_t proj;
    Map_PrepareProjection(&proj, center_lat, center_lon, metres_per_pixel, center_x, center_y);
    return Map_BuildPolylinePrepared(track, &proj, out, max_points);
}

size_t Map_BuildPolylinePrepared(const TrackBuffer_t *track, const MapProjection_t *proj,
                                 MapPoint_t *out, size_t max_points) {
    size_t written = 0;
    size_t i;
    size_t first = 0;
    size_t last = 0;
    size_t span;
    size_t stride;
    MapSourceBox_t box;
    int found = 0;
    int16_t last_x = 0;
    int16_t last_y = 0;

    if (track == NULL || proj == NULL || out == NULL || max_points == 0 || track->count == 0) {
        return 0;
    }

    /* First pass: the range of the track that is anywhere near the viewport.
       Only this stretch is drawn.

       The walk used to start at the track's beginning and stop when the buffer
       filled, which at any zoom needing more points than the buffer holds drew
       the START of the track and nothing where the rider was -- an empty map.
       Collapsing off-screen runs instead was worse: merging an excursion into
       one point draws a straight chord from where the track left the screen to
       where it came back, cutting across a map it never crosses.

       Two scans inward rather than one across, and a source-space box in
       front of the projection. See the note above Map_SourceBox for why both,
       and for why the pair of indices this produces is the same pair the
       single full-projection pass produced. */
    Map_SourceBox(proj, &box);

    for (i = 0; i < track->count; i++) {
        if (Map_IndexVisible(track, proj, &box, i)) {
            first = i;
            found = 1;
            break;
        }
    }

    if (!found) {
        return 0; /* The track is somewhere else entirely. */
    }

    /* Backward from the end, stopping above `first` -- which is itself
       visible, so it is the answer when nothing later is. */
    last = first;
    for (i = track->count; i-- > first + 1;) {
        if (Map_IndexVisible(track, proj, &box, i)) {
            last = i;
            break;
        }
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
        Map_ProjectPrepared(proj, lat, lon, &x, &y);

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
