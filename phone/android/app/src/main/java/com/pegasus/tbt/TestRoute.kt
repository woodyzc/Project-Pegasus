package com.pegasus.tbt

import kotlin.math.cos
import kotlin.math.PI

/**
 * A synthetic route, so the whole upload path can be exercised on a bench.
 *
 * This exists because of a gap that would otherwise block the feature
 * indefinitely: the head unit's offline navigation needs a GPS fix, and no GPS
 * module has ever been attached to this project. Waiting for one before
 * testing anything would leave the protocol, the chunking, the reassembly, the
 * PSRAM store and the progress reporting all unexercised.
 *
 * None of those need a fix. Sending this route proves every one of them: the
 * head unit will report chunks arriving, assemble them, and hold a route it
 * says is loaded. Only the final step -- turning a position into a turn --
 * needs hardware that does not exist yet.
 *
 * Built around Germantown MD, matching the .prd road extract already on the
 * card, so the uploaded line lands on top of real streets rather than in the
 * ocean off Ghana where 0,0 would put it.
 */
object TestRoute {

    /** Roughly the centre of the 20874 extract. */
    private const val START_LAT = 39.1694
    private const val START_LON = -77.2711

    /**
     * A square loop with a turn at each corner and an arrival back at the
     * start. Deliberately a loop rather than a straight line: it crosses its
     * own start, which is the geometry that breaks nearest-maneuver
     * navigation, so what gets uploaded is a route worth following rather than
     * a shape that would work under any implementation.
     *
     * @param sideMetres length of each side
     * @param pointsPerSide how finely the line is sampled, to exercise
     *        multi-chunk transfers rather than fitting in one write
     */
    fun square(sideMetres: Int = 400, pointsPerSide: Int = 60): PlannedRoute {
        val metresPerDegLat = 111_320.0
        val metresPerDegLon = 111_320.0 * cos(START_LAT * PI / 180.0)

        val dLat = sideMetres / metresPerDegLat
        val dLon = sideMetres / metresPerDegLon

        // Corners, anticlockwise from the start: east, north, west, back.
        val corners = listOf(
            START_LAT to START_LON,
            START_LAT to (START_LON + dLon),
            (START_LAT + dLat) to (START_LON + dLon),
            (START_LAT + dLat) to START_LON,
            START_LAT to START_LON,
        )

        val points = ArrayList<RouteFrame.Point>()
        for (i in 0 until corners.size - 1) {
            val (aLat, aLon) = corners[i]
            val (bLat, bLon) = corners[i + 1]
            // The last point of each side is the first of the next, so it is
            // emitted once rather than twice -- a duplicated point is a
            // zero-length segment, which is exactly the degenerate case the
            // head unit's snap has to guard against anyway.
            for (s in 0 until pointsPerSide) {
                val t = s.toDouble() / pointsPerSide
                points.add(
                    RouteFrame.Point(
                        aLat + (bLat - aLat) * t,
                        aLon + (bLon - aLon) * t,
                    )
                )
            }
        }
        points.add(RouteFrame.Point(corners.last().first, corners.last().second))

        val steps = listOf(
            PlannedRoute.Step(ManeuverParser.Icon.STRAIGHT, "Test Loop Start", sideMetres),
            PlannedRoute.Step(ManeuverParser.Icon.TURN_LEFT, "North Side", sideMetres),
            PlannedRoute.Step(ManeuverParser.Icon.TURN_LEFT, "West Side", sideMetres),
            PlannedRoute.Step(ManeuverParser.Icon.TURN_LEFT, "South Side", sideMetres),
            PlannedRoute.Step(ManeuverParser.Icon.ARRIVE, "Back At Start", 0),
        )

        // A route id that changes each time, so re-sending starts a fresh
        // transfer on the head unit rather than being taken for a retry of the
        // one it already holds.
        val routeId = (System.currentTimeMillis() and 0x7FFFFFFF).toInt()
        return PlannedRoute.fromSteps(routeId, points, steps)
    }
}
