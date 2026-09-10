package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The parser is the fragile half of this app, so it carries the tests. These
 * run on the JVM with no device or emulator: ./gradlew :app:test
 */
class ManeuverParserTest {

    private fun icon(title: String, text: String = "") =
        ManeuverParser.parse(title, text)?.iconId

    @Test
    fun `basic turns`() {
        assertEquals(ManeuverParser.Icon.TURN_RIGHT, icon("Turn right", "350 m onto Main St"))
        assertEquals(ManeuverParser.Icon.TURN_LEFT, icon("Turn left", "200 m onto Elm Road"))
    }

    @Test
    fun `specific phrases beat the bare direction word`() {
        // "slight left" must not be read as a plain left turn.
        assertEquals(ManeuverParser.Icon.SLIGHT_LEFT, icon("Slight left", "100 m"))
        assertEquals(ManeuverParser.Icon.SLIGHT_RIGHT, icon("Keep right", "400 m"))
        assertEquals(ManeuverParser.Icon.SHARP_LEFT, icon("Sharp left", "80 m"))
        // A roundabout mentioning an exit direction is still a roundabout.
        assertEquals(ManeuverParser.Icon.ROUNDABOUT, icon("At the roundabout, turn right", "250 m"))
        assertEquals(ManeuverParser.Icon.UTURN, icon("Make a U-turn", "150 m"))
    }

    @Test
    fun `distance units all convert to metres`() {
        assertEquals(350, ManeuverParser.parseDistanceMetres("350 m"))
        assertEquals(1200, ManeuverParser.parseDistanceMetres("1.2 km"))
        assertEquals(152, ManeuverParser.parseDistanceMetres("500 ft"))
        assertEquals(483, ManeuverParser.parseDistanceMetres("0.3 mi"))
        assertEquals(1200, ManeuverParser.parseDistanceMetres("1,2 km")) // comma decimal
        assertNull(ManeuverParser.parseDistanceMetres("no numbers here"))
    }

    @Test
    fun `street name is extracted from common phrasings`() {
        assertEquals("Main St", ManeuverParser.extractStreet("Turn right", "350 m onto Main St"))
        assertEquals("Hongqiao Road", ManeuverParser.extractStreet("", "toward Hongqiao Road"))
        // No preposition: strip the distance so it isn't repeated on screen.
        assertEquals("Elm Road", ManeuverParser.extractStreet("", "200 m - Elm Road"))
    }

    @Test
    fun `unrecognised text yields nothing rather than a guess`() {
        assertNull(ManeuverParser.parse("Rate your trip", "How was traffic?"))
        assertNull(ManeuverParser.parse(null, null))
        assertNull(ManeuverParser.parse("", ""))
        // A maneuver with no distance is rejected unless it is an arrival.
        assertNull(ManeuverParser.parse("Turn right", "onto Main St"))
    }

    @Test
    fun `arrival needs no distance`() {
        val arrival = ManeuverParser.parse("Arrive at destination", "")
        assertEquals(ManeuverParser.Icon.ARRIVE, arrival?.iconId)
        assertEquals(0, arrival?.distanceMetres)
    }
}
