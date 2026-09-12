package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The per-step to along-the-route conversion, and the ordering the head unit
 * depends on.
 *
 * Both failures here are invisible until a rider is at a junction, which is
 * why they are pinned rather than trusted.
 */
class PlannedRouteTest {

    private val points = listOf(
        RouteFrame.Point(39.0, -77.0),
        RouteFrame.Point(39.001, -77.0),
        RouteFrame.Point(39.002, -77.0),
    )

    /**
     * A directions API gives the length OF each step; the head unit needs the
     * distance TO each maneuver from the route start. Mixing the two puts
     * every turn in the wrong place.
     */
    @Test
    fun stepLengthsBecomeDistancesAlongTheRoute() {
        val route = PlannedRoute.fromSteps(
            routeId = 1,
            points = points,
            steps = listOf(
                PlannedRoute.Step(ManeuverParser.Icon.STRAIGHT, "Depart", 200),
                PlannedRoute.Step(ManeuverParser.Icon.TURN_RIGHT, "Richter Farm Rd", 450),
                PlannedRoute.Step(ManeuverParser.Icon.TURN_LEFT, "Clopper Rd", 300),
                PlannedRoute.Step(ManeuverParser.Icon.ARRIVE, "Destination", 0),
            ),
        )

        // The first maneuver is at the start, not 200m in.
        assertEquals(0, route.maneuvers[0].distanceAlongRouteM)
        assertEquals(200, route.maneuvers[1].distanceAlongRouteM)
        assertEquals(650, route.maneuvers[2].distanceAlongRouteM)
        assertEquals(950, route.maneuvers[3].distanceAlongRouteM)
        assertEquals(950, route.totalLengthM)
    }

    @Test
    fun stepFieldsSurviveTheConversion() {
        val route = PlannedRoute.fromSteps(
            routeId = 1,
            points = points,
            steps = listOf(
                PlannedRoute.Step(ManeuverParser.Icon.ROUNDABOUT, "Great Seneca Hwy", 120, 3),
            ),
        )
        val m = route.maneuvers.single()
        assertEquals(ManeuverParser.Icon.ROUNDABOUT, m.iconId)
        assertEquals("Great Seneca Hwy", m.streetName)
        assertEquals(3, m.exitNumber)
    }

    /**
     * RouteFollow_NextManeuver walks the list in order and takes the first at
     * or past the rider. Out of order it navigates badly rather than failing,
     * so the encoder sorts rather than trusting the caller.
     */
    @Test
    fun encodingSortsManeuversIntoRouteOrder() {
        val route = PlannedRoute(
            routeId = 1,
            points = points,
            maneuvers = listOf(
                RouteFrame.Maneuver(ManeuverParser.Icon.ARRIVE, 0, 900, "Destination"),
                RouteFrame.Maneuver(ManeuverParser.Icon.TURN_LEFT, 0, 100, "First St"),
                RouteFrame.Maneuver(ManeuverParser.Icon.TURN_RIGHT, 0, 500, "Second St"),
            ),
            totalLengthM = 900,
        )

        val order = route.sorted().maneuvers.map { it.distanceAlongRouteM }
        assertEquals(listOf(100, 500, 900), order)

        // And the encoded bytes carry that order, not the order supplied.
        val encoded = route.encode()
        val payload = encoded.chunks[1]
        val at = RouteFrame.CHUNK_HEADER_LEN + points.size * RouteFrame.POINT_SIZE
        assertEquals(ManeuverParser.Icon.TURN_LEFT.toByte(), payload[at])
    }

    @Test
    fun sortingLeavesAnOrderedRouteAlone() {
        val route = PlannedRoute.fromSteps(
            routeId = 1,
            points = points,
            steps = listOf(
                PlannedRoute.Step(ManeuverParser.Icon.STRAIGHT, "A", 100),
                PlannedRoute.Step(ManeuverParser.Icon.TURN_LEFT, "B", 100),
            ),
        )
        assertEquals(route.maneuvers, route.sorted().maneuvers)
    }

    @Test
    fun aRouteEncodesIntoSendableChunks() {
        val many = (0 until 800).map {
            RouteFrame.Point(39.0 + it * 0.0001, -77.0 + it * 0.0001)
        }
        val route = PlannedRoute.fromSteps(
            routeId = 42,
            points = many,
            steps = (0 until 20).map {
                PlannedRoute.Step(ManeuverParser.Icon.TURN_LEFT, "Street $it", 250)
            },
        )
        val encoded = route.encode()

        assertEquals(42, encoded.routeId)
        assertTrue(encoded.chunks.size > 1)
        assertEquals(5000, route.totalLengthM)
    }

    @Test
    fun anInstructionEncodesAsATurnFrame() {
        val instruction = NavigationInstruction(
            iconId = ManeuverParser.Icon.TURN_RIGHT,
            distanceMetres = 150,
            streetName = "Richter Farm Rd",
        )
        val frame = instruction.toFrame()

        assertEquals(TbtFrame.MAGIC, frame[0])
        assertEquals(ManeuverParser.Icon.TURN_RIGHT.toByte(), frame[2])
        assertEquals(150, frame[4].toInt() and 0xFF)
    }
}
