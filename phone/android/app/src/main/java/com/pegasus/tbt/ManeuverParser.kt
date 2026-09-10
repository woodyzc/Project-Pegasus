package com.pegasus.tbt

/**
 * Turns the text of a Google Maps navigation notification into a maneuver.
 *
 * This is the fragile heart of the whole approach and is therefore kept pure:
 * no Android types, no BLE, no I/O -- so it can be unit tested on the JVM
 * (see ManeuverParserTest) the same way the firmware keeps TbtParse.c free of
 * NimBLE so test/host can cover it.
 *
 * Why text at all: Maps carries the maneuver arrow as a *bitmap* in the
 * notification, not as any numeric field, so the turn type can only be
 * recovered from the wording. That makes this locale-specific -- these
 * patterns are English only, matching the phone's Maps language.
 *
 * Design rule, mirroring the firmware's frame parser: anything not clearly
 * understood returns null so the caller sends nothing and the head unit falls
 * back to "NO ROUTE". Showing a rider a confidently wrong turn is far worse
 * than showing them none.
 */
object ManeuverParser {

    /** Must match TBT_Icon_t in src/system/DataCenter.h. */
    object Icon {
        const val NONE = 0
        const val STRAIGHT = 1
        const val TURN_LEFT = 2
        const val TURN_RIGHT = 3
        const val SLIGHT_LEFT = 4
        const val SLIGHT_RIGHT = 5
        const val SHARP_LEFT = 6
        const val SHARP_RIGHT = 7
        const val UTURN = 8
        const val ROUNDABOUT = 9
        const val ARRIVE = 10
    }

    data class Maneuver(val iconId: Int, val distanceMetres: Int, val streetName: String)

    // Ordered most specific first: "slight left" must beat the bare "left",
    // and "roundabout" must beat any direction word inside the same phrase.
    private val ICON_PATTERNS: List<Pair<Regex, Int>> = listOf(
        Regex("""\bu[- ]?turn\b""", RegexOption.IGNORE_CASE) to Icon.UTURN,
        Regex("""\broundabout|rotary|traffic circle\b""", RegexOption.IGNORE_CASE) to Icon.ROUNDABOUT,
        Regex("""\bsharp(ly)?\s+left\b""", RegexOption.IGNORE_CASE) to Icon.SHARP_LEFT,
        Regex("""\bsharp(ly)?\s+right\b""", RegexOption.IGNORE_CASE) to Icon.SHARP_RIGHT,
        Regex("""\b(slight(ly)?|bear|keep)\s+left\b""", RegexOption.IGNORE_CASE) to Icon.SLIGHT_LEFT,
        Regex("""\b(slight(ly)?|bear|keep)\s+right\b""", RegexOption.IGNORE_CASE) to Icon.SLIGHT_RIGHT,
        Regex("""\b(turn|exit)\s+left\b|\bleft\s+turn\b""", RegexOption.IGNORE_CASE) to Icon.TURN_LEFT,
        Regex("""\b(turn|exit)\s+right\b|\bright\s+turn\b""", RegexOption.IGNORE_CASE) to Icon.TURN_RIGHT,
        Regex("""\barriv|\bdestination\b""", RegexOption.IGNORE_CASE) to Icon.ARRIVE,
        Regex("""\b(continue|straight|head)\b""", RegexOption.IGNORE_CASE) to Icon.STRAIGHT,
        // Last resort: a bare direction word with no verb around it.
        Regex("""\bleft\b""", RegexOption.IGNORE_CASE) to Icon.TURN_LEFT,
        Regex("""\bright\b""", RegexOption.IGNORE_CASE) to Icon.TURN_RIGHT,
    )

    // "350 m", "1.2 km", "500 ft", "0.3 mi" -- Maps uses whichever unit system
    // the phone is set to, so all four have to be understood.
    private val DISTANCE_PATTERN = Regex(
        """(\d+(?:[.,]\d+)?)\s*(km|m|mi|ft)\b""",
        RegexOption.IGNORE_CASE
    )

    private const val METRES_PER_MILE = 1609.344
    private const val METRES_PER_FOOT = 0.3048

    /**
     * @param title the notification's title line, usually the maneuver
     * @param text  the notification's body, usually the road being joined
     * @return null when no maneuver can be identified with confidence
     */
    fun parse(title: String?, text: String?): Maneuver? {
        val titleText = title.orEmpty().trim()
        val bodyText = text.orEmpty().trim()
        if (titleText.isEmpty() && bodyText.isEmpty()) return null

        val combined = "$titleText $bodyText"

        val iconId = ICON_PATTERNS.firstOrNull { it.first.containsMatchIn(combined) }?.second
            ?: return null

        // Distance may sit in either line depending on Maps version; arrival
        // notices often carry none at all, which is legitimate.
        val distance = parseDistanceMetres(combined) ?: if (iconId == Icon.ARRIVE) 0 else return null

        return Maneuver(iconId, distance, extractStreet(titleText, bodyText))
    }

    /** Exposed for tests; returns null when no distance is present. */
    fun parseDistanceMetres(input: String): Int? {
        val match = DISTANCE_PATTERN.find(input) ?: return null
        val value = match.groupValues[1].replace(',', '.').toDoubleOrNull() ?: return null
        val metres = when (match.groupValues[2].lowercase()) {
            "km" -> value * 1000.0
            "mi" -> value * METRES_PER_MILE
            "ft" -> value * METRES_PER_FOOT
            else -> value
        }
        if (metres < 0 || metres > Int.MAX_VALUE.toDouble()) return null
        return Math.round(metres).toInt()
    }

    // Only "onto" and "toward(s)" introduce the road. A bare "on" was tried
    // and removed: Maps frequently wraps an aside around the maneuver, e.g.
    //   "Turn left (traffic lights on the left) onto Jones Branch Dr"
    // and the "on" inside that aside matched first, yielding the road name
    // "the left) onto Jones Branch Dr" on a real route.
    private val ROAD_INTRO = Regex("""\b(?:onto|towards?)\s+""", RegexOption.IGNORE_CASE)

    /**
     * Best-effort road name. Maps usually phrases the body as "onto X" or
     * "toward X"; when it doesn't, the body line is generally the road itself.
     */
    fun extractStreet(title: String, text: String): String {
        val source = if (text.isNotEmpty()) text else title

        // The LAST introducer, not the first: the road being joined comes at
        // the end, while an aside earlier in the sentence may contain one too.
        // (Matching only the keyword and taking the remainder by index, rather
        // than capturing with (.+) -- a greedy capture consumes to the end of
        // the string, so findAll would only ever return one match.)
        ROAD_INTRO.findAll(source).lastOrNull()?.let { match ->
            return cleanUp(source.substring(match.range.last + 1))
        }

        // Drop a leading distance so "350 m - Main St" doesn't repeat what the
        // head unit already shows in its own distance field.
        return cleanUp(source.replace(DISTANCE_PATTERN, ""))
    }

    private fun cleanUp(value: String): String {
        var result = value.trim()

        // An aside can leave its closing bracket stranded at the front once
        // the text before it has been cut away.
        while (result.isNotEmpty() && (result[0] == ')' || result[0] == ']' ||
                result[0] == '-' || result[0] == '–' || result[0] == '·')
        ) {
            result = result.substring(1).trim()
        }

        return result.trim().trimEnd('.', ',', ' ')
    }
}
