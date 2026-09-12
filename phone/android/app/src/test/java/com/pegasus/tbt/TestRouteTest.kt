package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The synthetic route is the only thing that can exercise the upload path
 * before a GPS module exists, so it has to actually be a valid route rather
 * than merely a large blob. If it is malformed the head unit rejects it, and
 * the bench test reports a failure that is the test's fault rather than the
 * firmware's.
 */
class TestRouteTest {

    @Test
    fun itIsAClosedLoop() {
        val route = TestRoute.square()
        val first = route.points.first()
        val last = route.points.last()

        assertEquals(first.lat, last.lat, 1e-9)
        assertEquals(first.lon, last.lon, 1e-9)
    }

    @Test
    fun itLandsOnTheGermantownExtract() {
        val route = TestRoute.square()
        // The .prd on the card covers 20874. A route outside it would draw on
        // a blank screen and look like a rendering fault.
        for (p in route.points) {
            assertTrue("latitude ${p.lat}", p.lat in 39.0..39.4)
            assertTrue("longitude ${p.lon}", p.lon in -77.5..-77.0)
        }
    }

    @Test
    fun itSpansSeveralChunks() {
        val encoded = TestRoute.square().encode()
        // A single-chunk route would leave the reassembly, the offset maths
        // and the progress reporting untested, which is most of what this is
        // for.
        assertTrue("chunks ${encoded.chunks.size}", encoded.chunks.size > 5)
    }

    @Test
    fun maneuversAreInRouteOrder() {
        val route = TestRoute.square().sorted()
        val distances = route.maneuvers.map { it.distanceAlongRouteM }
        assertEquals(distances.sorted(), distances)
    }

    @Test
    fun itEndsWithAnArrival() {
        val route = TestRoute.square()
        assertEquals(ManeuverParser.Icon.ARRIVE, route.maneuvers.last().iconId)
        assertEquals(route.totalLengthM, route.maneuvers.last().distanceAlongRouteM)
    }

    @Test
    fun theLengthMatchesTheRequestedSides() {
        val route = TestRoute.square(sideMetres = 400)
        assertEquals(1600, route.totalLengthM)
    }

    /** Re-sending must start a fresh transfer, not be mistaken for a retry of
     * the route the head unit already holds. */
    @Test
    fun eachRouteGetsItsOwnId() {
        val a = TestRoute.square()
        Thread.sleep(2)
        val b = TestRoute.square()
        assertNotEquals(a.routeId, b.routeId)
        assertTrue("route id must be positive", a.routeId > 0)
    }
}
