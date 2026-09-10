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
    fun `an aside before the road does not hijack the street name`() {
        // Regression: observed on a real route in McLean, VA. An earlier
        // version accepted a bare "on" as the road introducer, so the "on"
        // inside the parenthetical matched first and the extracted name came
        // out as "the left) onto Jones Branch Dr".
        assertEquals(
            "Jones Branch Dr",
            ManeuverParser.extractStreet("Turn left", "Turn left (traffic lights on the left) onto Jones Branch Dr")
        )
        assertEquals(
            "Westpark Dr",
            ManeuverParser.extractStreet("", "Keep right (the bank is on the right) onto Westpark Dr")
        )
        // Last introducer wins even when the aside contains one of its own.
        assertEquals(
            "Elm St",
            ManeuverParser.extractStreet("", "Continue (merges onto the ramp) onto Elm St")
        )
    }

    @Test
    fun `real capture - whole instruction in the title, EXTRA_TEXT null`() {
        // Verified on a Samsung phone, Maps in English with imperial units:
        // EXTRA_TEXT comes back null and the entire instruction arrives as one
        // title line separated by a middle dot. Worth pinning, because it means
        // the parser must cope with a null body and cannot assume the road name
        // lives in its own field.
        val m = ManeuverParser.parse("200 ft · Turn left toward Jones Branch Dr", null)
        assertEquals(ManeuverParser.Icon.TURN_LEFT, m?.iconId)
        assertEquals(61, m?.distanceMetres) // 200 ft = 60.96 m
        assertEquals("Jones Branch Dr", m?.streetName)
    }

    @Test
    fun `motorway exits parse - the case that produced parsed=0 on the road`() {
        // Verbatim from a drive in Maryland. Before exit/ramp/merge patterns
        // existed, every notification on that road failed: seen 31, used 31,
        // parsed 0. The patterns were written for turns and never for the
        // manoeuvres a highway actually produces.
        val m = ManeuverParser.parse(
            "7.2 mi \u00b7 Take the MD-117/MD-124 exit toward Clopper Rd/W. Diamond Ave/" +
                "Mont. Village Ave/Quince Orch. Rd",
            null
        )
        assertEquals(ManeuverParser.Icon.SLIGHT_RIGHT, m?.iconId)
        assertEquals(11587, m?.distanceMetres) // 7.2 mi
        // The full destination list cannot fit in 31 bytes, and truncating it
        // yields a run-on ending mid-word. The first road is the one on the sign.
        assertEquals("Clopper Rd", m?.streetName)
    }

    @Test
    fun `exit side is taken from the wording when Maps gives it`() {
        assertEquals(
            ManeuverParser.Icon.SLIGHT_LEFT,
            ManeuverParser.parse("1.2 mi \u00b7 Take the I-495 exit on the left toward Silver Spring", null)?.iconId
        )
        assertEquals(
            ManeuverParser.Icon.SLIGHT_RIGHT,
            ManeuverParser.parse("500 ft \u00b7 Take exit 12 toward Rockville", null)?.iconId
        )
        assertEquals(
            ManeuverParser.Icon.SLIGHT_RIGHT,
            ManeuverParser.parse("0.4 mi \u00b7 Take the ramp onto I-270 N", null)?.iconId
        )
    }

    @Test
    fun `merges parse, with a side when stated`() {
        assertEquals(
            ManeuverParser.Icon.SLIGHT_RIGHT,
            ManeuverParser.parse("800 ft \u00b7 Merge right onto I-370", null)?.iconId
        )
        assertEquals(
            ManeuverParser.Icon.SLIGHT_LEFT,
            ManeuverParser.parse("800 ft \u00b7 Merge left onto I-370", null)?.iconId
        )
        // No side given: a merge is a continuation, not a turn.
        assertEquals(
            ManeuverParser.Icon.STRAIGHT,
            ManeuverParser.parse("800 ft \u00b7 Merge onto I-370", null)?.iconId
        )
    }

    @Test
    fun `a road number keeps its slash, a destination list does not`() {
        // "MD-117/MD-124" is one name; splitting it would be wrong. It is also
        // short enough to fit, so it is never split.
        assertEquals("MD-117/MD-124", ManeuverParser.extractStreet("", "onto MD-117/MD-124"))
        // A long list is cut at the first road.
        assertEquals(
            "Clopper Rd",
            ManeuverParser.extractStreet("", "toward Clopper Rd/W. Diamond Ave/Mont. Village Ave")
        )
    }

    @Test
    fun `imperial distances round the way Maps shows them`() {
        // Real capture: Maps displayed "0.1 mi" and the head unit should show
        // the metric equivalent, not the raw number.
        assertEquals(161, ManeuverParser.parseDistanceMetres("0.1 mi"))
        val m = ManeuverParser.parse("Turn left", "0.1 mi onto Jones Branch Dr")
        assertEquals(ManeuverParser.Icon.TURN_LEFT, m?.iconId)
        assertEquals(161, m?.distanceMetres)
        assertEquals("Jones Branch Dr", m?.streetName)
    }

    @Test
    fun `unrecognised text yields nothing rather than a guess`() {
        assertNull(ManeuverParser.parse("Rate your trip", "How was traffic?"))
        assertNull(ManeuverParser.parse(null, null))
        assertNull(ManeuverParser.parse("", ""))
        // A maneuver with no distance used to be rejected here. It is now
        // carried with DISTANCE_UNKNOWN instead -- see the housing-estate
        // capture above, where that rule cost 38 of 109 maneuvers. What must
        // still be refused is a *weak* match with no distance, which is the
        // subject of its own test.
        val noDistance = ManeuverParser.parse("Turn right", "onto Main St")
        assertEquals(ManeuverParser.Icon.TURN_RIGHT, noDistance?.iconId)
        assertEquals(ManeuverParser.DISTANCE_UNKNOWN, noDistance?.distanceMetres)
    }

    @Test
    fun `an unparsed phrasing keys on the wording, not the counting-down distance`() {
        // A single unrecognised manoeuvre arrives once every few seconds with a
        // smaller number, so without this the six remembered slots fill with
        // one manoeuvre and a drive reporting 101 failures teaches nothing.
        val a = ManeuverParser.unparsedKey("0.4 mi \u00b7 Take the second exit")
        val b = ManeuverParser.unparsedKey("600 ft \u00b7 Take the second exit")
        assertEquals(a, b)
        assertEquals("Take the second exit", a)

        // Regression: hand-written as (km|m|mi|ft) with no word boundary, "mi"
        // matched as "m" and left the key as "i \u00b7 Take the second exit".
        assertEquals("Take the second exit", ManeuverParser.unparsedKey("200 mi - Take the second exit"))
        assertEquals("Take the second exit", ManeuverParser.unparsedKey("1.2 km \u00b7 Take the second exit"))

        // A distance inside the sentence is part of the wording and stays.
        assertEquals("In 500 ft, use the middle lane",
            ManeuverParser.unparsedKey("In 500 ft, use the middle lane"))

        // Never key on nothing: an empty or distance-only title must stay
        // distinguishable rather than collapsing into one blank slot.
        assertEquals("(no title or text)", ManeuverParser.unparsedKey(null))
        assertEquals("(no title or text)", ManeuverParser.unparsedKey("   "))
        assertEquals("0.4 mi", ManeuverParser.unparsedKey("0.4 mi"))
    }

    @Test
    fun `spelled-out units carry a distance just as the abbreviations do`() {
        // Regression, from a drive reporting parsed 8 of 33. Only "m/km/mi/ft"
        // were accepted, so a notification phrased "500 feet" lost its distance
        // and the maneuver was discarded whole -- "Turn right" showed up in the
        // unparsed list while Maps was plainly displaying a distance for it.
        assertEquals(152, ManeuverParser.parseDistanceMetres("500 feet"))
        assertEquals(161, ManeuverParser.parseDistanceMetres("0.1 miles"))
        assertEquals(1200, ManeuverParser.parseDistanceMetres("1.2 kilometers"))
        assertEquals(350, ManeuverParser.parseDistanceMetres("350 meters"))
        assertEquals(350, ManeuverParser.parseDistanceMetres("350 metres"))
        assertEquals(183, ManeuverParser.parseDistanceMetres("200 yards"))
        assertEquals(183, ManeuverParser.parseDistanceMetres("200 yd"))

        // "mi" must win over "m" inside "miles"; the word boundary is what
        // makes that reliable rather than a happy accident of ordering.
        assertEquals(1609, ManeuverParser.parseDistanceMetres("1 mile"))
        assertEquals(1, ManeuverParser.parseDistanceMetres("1 m"))

        val m = ManeuverParser.parse("Turn right", "500 feet onto Bureau Dr")
        assertEquals(ManeuverParser.Icon.TURN_RIGHT, m?.iconId)
        assertEquals(152, m?.distanceMetres)
        assertEquals("Bureau Dr", m?.streetName)
    }

    @Test
    fun `leaving a car park is a maneuver`() {
        // Verbatim from a drive: every exit pattern required the verb "take",
        // so this matched nothing at all.
        val m = ManeuverParser.parse("Exit the parking lot toward Game Preserve Rd", "300 feet")
        // STRAIGHT, not SLIGHT_RIGHT: there is no turn direction in this
        // instruction and guessing one would be inventing it.
        assertEquals(ManeuverParser.Icon.STRAIGHT, m?.iconId)
        assertEquals("Game Preserve Rd", m?.streetName)
    }

    @Test
    fun `status notifications are skipped, not counted as parse failures`() {
        assertEquals(ManeuverParser.Skip.TRANSIENT, ManeuverParser.skipReason("Starting navigation…", ""))
        assertEquals(ManeuverParser.Skip.TRANSIENT, ManeuverParser.skipReason("Rerouting…", ""))
        assertEquals(ManeuverParser.Skip.TRANSIENT, ManeuverParser.skipReason("Maps", ""))
        assertEquals(ManeuverParser.Skip.TRANSIENT, ManeuverParser.skipReason(null, null))

        // The phone blanked the content before the listener ever saw it. This
        // is not a parser fault and the status screen says so.
        assertEquals(
            ManeuverParser.Skip.REDACTED,
            ManeuverParser.skipReason("Maps", "Sensitive notification content hidden")
        )

        // A real maneuver is never skipped -- including one whose road name
        // happens to contain a skip word.
        assertNull(ManeuverParser.skipReason("200 ft · Turn left onto Main St", null))
        assertNull(ManeuverParser.skipReason("500 ft · Turn right onto Starting Gate Rd", null))
    }

    @Test
    fun `an unparsed key keeps the body text, not just the title`() {
        // The title-only key could not distinguish "Turn right" with a distance
        // the parser missed from "Turn right" with no distance at all, which is
        // exactly the question the previous drive left open.
        assertEquals(
            "Turn right | onto Bureau Dr",
            ManeuverParser.unparsedKey("0.2 mi · Turn right", "300 feet onto Bureau Dr")
        )
        assertEquals("Turn right", ManeuverParser.unparsedKey("Turn right", null))
        assertEquals("(no title) | Rerouting", ManeuverParser.unparsedKey("", "Rerouting"))
        assertEquals("(no title or text)", ManeuverParser.unparsedKey(null, null))

        // A destination list must not be truncated in a diagnostic key: hiding
        // half the evidence is the bug this exists to avoid.
        assertEquals(
            "Take the exit toward Clopper Rd/W. Diamond Ave/Mont. Village Ave",
            ManeuverParser.unparsedKey("7.2 mi · Take the exit toward Clopper Rd/W. Diamond Ave/Mont. Village Ave")
        )
    }

    @Test
    fun `a named turn with no distance still travels`() {
        // Verbatim from a drive through a housing estate: 38 of 109 maneuvers
        // arrived as a bare instruction with no distance in either line, and
        // every one was discarded -- arrow and street name with it.
        val m = ManeuverParser.parse("Turn right onto Richter Farm Rd", "")
        assertEquals(ManeuverParser.Icon.TURN_RIGHT, m?.iconId)
        assertEquals(ManeuverParser.DISTANCE_UNKNOWN, m?.distanceMetres)
        assertEquals(false, m?.hasDistance)
        assertEquals("Richter Farm Rd", m?.streetName)

        val straight = ManeuverParser.parse("Continue straight to stay on Rockingham Ct", "")
        assertEquals(ManeuverParser.Icon.STRAIGHT, straight?.iconId)
        assertEquals(ManeuverParser.DISTANCE_UNKNOWN, straight?.distanceMetres)
    }

    @Test
    fun `a bare direction word is still refused without a distance`() {
        // The last-resort patterns match any sentence mentioning "left" or
        // "right". A distance is what corroborates that such a sentence is a
        // maneuver, so dropping the distance requirement for them would
        // forward any passing notification that happened to use the word.
        assertNull(ManeuverParser.parse("Traffic is heavy on the right", ""))
        assertNull(ManeuverParser.parse("You have 2 messages left", ""))
        // With a distance, the same fallback is allowed to speak.
        assertEquals(
            ManeuverParser.Icon.TURN_RIGHT,
            ManeuverParser.parse("300 ft · the right lane", "")?.iconId
        )
        // Genuine junk stays refused either way.
        assertNull(ManeuverParser.parse("Rate your trip", "How was traffic?"))
    }

    @Test
    fun `arrival needs no distance`() {
        val arrival = ManeuverParser.parse("Arrive at destination", "")
        assertEquals(ManeuverParser.Icon.ARRIVE, arrival?.iconId)
        assertEquals(0, arrival?.distanceMetres)
    }
}
