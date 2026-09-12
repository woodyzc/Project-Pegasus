package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The destination field is the one place a rider types before setting off, so
 * every way a paste can be wrong is worth a case here rather than at a
 * trailhead.
 */
class DestinationParserTest {

    private fun ok(raw: String): Destination {
        val result = DestinationParser.parse(raw)
        assertTrue("expected success for: $raw, got $result", result is DestinationParser.Result.Ok)
        return (result as DestinationParser.Result.Ok).destination
    }

    private fun err(raw: String): String {
        val result = DestinationParser.parse(raw)
        assertTrue("expected failure for: $raw", result is DestinationParser.Result.Err)
        return (result as DestinationParser.Result.Err).reason
    }

    @Test
    fun `reads a plain coordinate pair`() {
        val d = ok("47.6062, -122.3321")
        assertEquals(47.6062, d.latitude, 1e-9)
        assertEquals(-122.3321, d.longitude, 1e-9)
    }

    @Test
    fun `tolerates missing space and surrounding blanks`() {
        val d = ok("  47.6062,-122.3321  ")
        assertEquals(47.6062, d.latitude, 1e-9)
    }

    @Test
    fun `prefers the pin over the camera in a place URL`() {
        // The @ pair is where the map was centred; !3d!4d is the place itself.
        // They differ here on purpose, and taking the wrong one puts the
        // destination a street away with nothing to show it went wrong.
        val url = "https://www.google.com/maps/place/Pike+Place/@47.6000,-122.3000,17z/" +
            "data=!3m1!4b1!4m5!3m4!1s0x0:0x0!8m2!3d47.6097!4d-122.3422"
        val d = ok(url)
        assertEquals(47.6097, d.latitude, 1e-9)
        assertEquals(-122.3422, d.longitude, 1e-9)
    }

    @Test
    fun `falls back to the camera when there is no pin`() {
        val d = ok("https://www.google.com/maps/@47.6205,-122.3493,15z")
        assertEquals(47.6205, d.latitude, 1e-9)
        assertEquals(-122.3493, d.longitude, 1e-9)
    }

    @Test
    fun `reads a query parameter`() {
        val d = ok("https://www.google.com/maps/search/?api=1&query=47.6205,-122.3493")
        assertEquals(47.6205, d.latitude, 1e-9)
    }

    @Test
    fun `reads a geo URI`() {
        val d = ok("geo:47.6205,-122.3493?z=15")
        assertEquals(-122.3493, d.longitude, 1e-9)
    }

    @Test
    fun `names the shortened link problem instead of failing vaguely`() {
        // The paste that looks perfectly reasonable and cannot ever work.
        val reason = err("https://maps.app.goo.gl/AbCdEfGhIjK")
        assertTrue(reason.contains("shortened"))
        assertTrue(reason.contains("full link"))
    }

    @Test
    fun `catches a swapped pair by its latitude`() {
        val reason = err("-122.3321, 47.6062")
        assertTrue(reason.contains("wrong way round"))
    }

    @Test
    fun `rejects an out of range longitude`() {
        assertTrue(err("47.6, 200.0").contains("Longitude"))
    }

    @Test
    fun `rejects empty input and prose`() {
        assertTrue(err("").isNotEmpty())
        assertTrue(err("the coffee place on the corner").isNotEmpty())
    }

    @Test
    fun `handles negative and integer coordinates`() {
        val d = ok("-33,151")
        assertEquals(-33.0, d.latitude, 1e-9)
        assertEquals(151.0, d.longitude, 1e-9)
    }

    @Test
    fun `describes a destination for display`() {
        assertEquals("47.60620, -122.33210", DestinationParser.describe(Destination(47.6062, -122.3321)))
    }
}
