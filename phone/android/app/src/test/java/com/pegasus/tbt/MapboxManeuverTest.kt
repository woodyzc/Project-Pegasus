package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The Mapbox maneuver vocabulary mapped onto the head unit's ten icons.
 *
 * Worth testing despite being a lookup table, because the failure mode is
 * silent and consequential: a wrong entry shows a rider a confident arrow
 * pointing the wrong way at a junction, and nothing in the system can detect
 * that. The pairs below are from the Mapbox Directions API reference.
 */
class MapboxManeuverTest {

    @Test
    fun plainTurns() {
        assertEquals(ManeuverParser.Icon.TURN_LEFT, MapboxManeuver.toIcon("turn", "left"))
        assertEquals(ManeuverParser.Icon.TURN_RIGHT, MapboxManeuver.toIcon("turn", "right"))
        assertEquals(ManeuverParser.Icon.SHARP_LEFT, MapboxManeuver.toIcon("turn", "sharp left"))
        assertEquals(ManeuverParser.Icon.SHARP_RIGHT, MapboxManeuver.toIcon("turn", "sharp right"))
        assertEquals(ManeuverParser.Icon.SLIGHT_LEFT, MapboxManeuver.toIcon("turn", "slight left"))
        assertEquals(
            ManeuverParser.Icon.SLIGHT_RIGHT,
            MapboxManeuver.toIcon("turn", "slight right"),
        )
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("turn", "straight"))
    }

    @Test
    fun uTurnIsAModifierNotAType() {
        assertEquals(ManeuverParser.Icon.UTURN, MapboxManeuver.toIcon("turn", "uturn"))
        assertEquals(ManeuverParser.Icon.UTURN, MapboxManeuver.toIcon("continue", "uturn"))
        assertEquals(ManeuverParser.Icon.UTURN, MapboxManeuver.toIcon("end of road", "uturn"))
    }

    @Test
    fun everyRoundaboutFormIsOneIcon() {
        for (t in listOf(
            "roundabout", "rotary", "roundabout turn", "exit roundabout", "exit rotary",
        )) {
            assertEquals(t, ManeuverParser.Icon.ROUNDABOUT, MapboxManeuver.toIcon(t, "right"))
        }
    }

    /** An arrival is an arrival whatever side it is on. Showing the modifier's
     * turn would send the rider past the destination looking for a junction. */
    @Test
    fun arrivalBeatsItsModifier() {
        assertEquals(ManeuverParser.Icon.ARRIVE, MapboxManeuver.toIcon("arrive", "left"))
        assertEquals(ManeuverParser.Icon.ARRIVE, MapboxManeuver.toIcon("arrive", "right"))
        assertEquals(ManeuverParser.Icon.ARRIVE, MapboxManeuver.toIcon("arrive", null))
    }

    @Test
    fun departAndContinueAreStraightUnlessTheyTurn() {
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("depart", "straight"))
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("depart", null))
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("continue", null))
        assertEquals(ManeuverParser.Icon.TURN_LEFT, MapboxManeuver.toIcon("continue", "left"))
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("new name", "straight"))
    }

    /** Leaving a carriageway is gentler than a junction turn, so ramps, merges
     * and forks round down to a slight turn. */
    @Test
    fun rampsAndForksAreGentle() {
        assertEquals(ManeuverParser.Icon.SLIGHT_RIGHT, MapboxManeuver.toIcon("off ramp", "right"))
        assertEquals(ManeuverParser.Icon.SLIGHT_LEFT, MapboxManeuver.toIcon("on ramp", "left"))
        assertEquals(
            ManeuverParser.Icon.SLIGHT_RIGHT,
            MapboxManeuver.toIcon("fork", "slight right"),
        )
        assertEquals(ManeuverParser.Icon.SLIGHT_LEFT, MapboxManeuver.toIcon("merge", "sharp left"))
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("fork", "straight"))
    }

    /**
     * Unknown input becomes STRAIGHT, never null. The maneuver is known to
     * exist at a known distance; only its shape is unfamiliar, so an
     * understated arrow with a correct street name and countdown beats a blank
     * panel. This is the opposite of ManeuverParser's rule and the difference
     * is deliberate.
     */
    @Test
    fun unknownInputUnderstatesRatherThanDisappears() {
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon(null, null))
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("", ""))
        assertEquals(ManeuverParser.Icon.STRAIGHT, MapboxManeuver.toIcon("teleport", "sideways"))
    }

    @Test
    fun caseAndWhitespaceDoNotMatter() {
        assertEquals(ManeuverParser.Icon.TURN_LEFT, MapboxManeuver.toIcon("Turn", "Left"))
        assertEquals(ManeuverParser.Icon.SHARP_RIGHT, MapboxManeuver.toIcon(" TURN ", " Sharp Right "))
    }

    /** Every icon this maps to must be one the firmware knows. */
    @Test
    fun everyIconIsInRange() {
        val types = listOf(
            "turn", "new name", "depart", "arrive", "merge", "on ramp", "off ramp",
            "fork", "end of road", "continue", "roundabout", "rotary",
            "roundabout turn", "notification", "exit roundabout", "exit rotary",
        )
        val modifiers = listOf(
            null, "uturn", "sharp right", "right", "slight right", "straight",
            "slight left", "left", "sharp left",
        )
        for (t in types) {
            for (m in modifiers) {
                val icon = MapboxManeuver.toIcon(t, m)
                assert(icon in ManeuverParser.Icon.NONE..ManeuverParser.Icon.ARRIVE) {
                    "($t, $m) produced out-of-range icon $icon"
                }
            }
        }
    }
}
