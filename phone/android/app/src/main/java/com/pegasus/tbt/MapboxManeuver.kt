package com.pegasus.tbt

/**
 * Maps Mapbox Directions maneuvers onto the head unit's ten icon codes.
 *
 * This replaces reading the turn out of Google Maps' notification text, which
 * was always the weak link: Maps carries the arrow as a bitmap, so the
 * maneuver could only be recovered from the wording, which made it English
 * only and left it guessing on anything phrased unusually. Mapbox states the
 * maneuver as structured fields, so this is a lookup rather than a parse.
 *
 * Kept pure -- no Android types, no Mapbox types, just the two strings the SDK
 * reports -- so it unit tests on the JVM without the SDK on the classpath.
 * That matters more than usual here, because the Mapbox dependency cannot be
 * resolved without an account token and CI has none.
 *
 * The (type, modifier) pairs are Mapbox's Directions API vocabulary:
 *   type      turn, new name, depart, arrive, merge, on ramp, off ramp, fork,
 *             end of road, continue, roundabout, rotary, roundabout turn,
 *             notification, exit roundabout, exit rotary
 *   modifier  uturn, sharp right, right, slight right, straight, slight left,
 *             left, sharp left
 *
 * Anything unrecognised becomes STRAIGHT rather than null. That is the
 * opposite of ManeuverParser's rule, and deliberately so: there, an
 * unrecognised string meant the parse had failed and the turn was unknown, so
 * showing nothing was right. Here the maneuver is known to exist at a known
 * distance and only its shape is unfamiliar, so an arrow that understates the
 * turn while the street name and countdown stay correct beats a blank panel.
 */
object MapboxManeuver {

    /**
     * @param type     Mapbox `StepManeuver.type()`
     * @param modifier Mapbox `StepManeuver.modifier()`, may be null
     */
    fun toIcon(type: String?, modifier: String?): Int {
        val t = type?.lowercase()?.trim().orEmpty()
        val m = modifier?.lowercase()?.trim().orEmpty()

        // Arrival wins over any modifier: "arrive left" is still an arrival,
        // and showing a left turn at the destination would send the rider past
        // it looking for a junction.
        if (t == "arrive") return ManeuverParser.Icon.ARRIVE

        // Every roundabout form collapses to one icon. The head unit has a
        // single roundabout arrow, and the exit number is carried as its own
        // field rather than being baked into the shape.
        if (t == "roundabout" || t == "rotary" || t == "roundabout turn" ||
            t == "exit roundabout" || t == "exit rotary"
        ) {
            return ManeuverParser.Icon.ROUNDABOUT
        }

        // A u-turn is a modifier, not a type, so it has to be caught before
        // the directional mapping below.
        if (m == "uturn") return ManeuverParser.Icon.UTURN

        // "depart" and "continue" with no turn in them are just "carry on".
        if (t == "depart" || t == "continue" || t == "new name" || t == "notification") {
            return fromModifier(m, ManeuverParser.Icon.STRAIGHT)
        }

        // Ramps, merges and forks are directional but gentle -- a slight turn
        // is a better description of leaving a carriageway than a square one.
        if (t == "on ramp" || t == "off ramp" || t == "merge" || t == "fork") {
            return when {
                m.contains("left") -> ManeuverParser.Icon.SLIGHT_LEFT
                m.contains("right") -> ManeuverParser.Icon.SLIGHT_RIGHT
                else -> ManeuverParser.Icon.STRAIGHT
            }
        }

        return fromModifier(m, ManeuverParser.Icon.STRAIGHT)
    }

    private fun fromModifier(modifier: String, fallback: Int): Int = when (modifier) {
        "sharp left" -> ManeuverParser.Icon.SHARP_LEFT
        "left" -> ManeuverParser.Icon.TURN_LEFT
        "slight left" -> ManeuverParser.Icon.SLIGHT_LEFT
        "slight right" -> ManeuverParser.Icon.SLIGHT_RIGHT
        "right" -> ManeuverParser.Icon.TURN_RIGHT
        "sharp right" -> ManeuverParser.Icon.SHARP_RIGHT
        "straight" -> ManeuverParser.Icon.STRAIGHT
        "uturn" -> ManeuverParser.Icon.UTURN
        else -> fallback
    }
}
