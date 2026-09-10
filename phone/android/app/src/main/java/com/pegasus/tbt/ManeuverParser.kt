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

    /**
     * Distance for a maneuver Maps named without saying how far away it is.
     * Encoded on the wire as TBT_DISTANCE_UNKNOWN (0xFFFFFFFF); the head unit
     * shows a dash rather than a number. Negative here so it can never be
     * confused with a real metre count.
     */
    const val DISTANCE_UNKNOWN = -1

    data class Maneuver(val iconId: Int, val distanceMetres: Int, val streetName: String) {
        val hasDistance: Boolean get() = distanceMetres != DISTANCE_UNKNOWN
    }

    /**
     * Why a notification carries no maneuver, when that is not a parser fault.
     *
     * Without this every one of these counted as a parse failure, which made
     * the on-screen success rate meaningless: a drive reporting "parsed 8 of
     * 33" was mixing genuine misses with notifications that never contained a
     * maneuver in the first place, and there was no way to tell the two apart.
     */
    enum class Skip {
        /** A status message: rerouting, acquiring GPS, and so on. */
        TRANSIENT,

        /**
         * Android replaced the content with "Sensitive notification content
         * hidden". Nothing here can recover it -- the text never reaches the
         * listener. The user has to turn off the phone's sensitive-notification
         * setting, so this is called out separately on the status screen.
         */
        REDACTED,
    }

    private val TRANSIENT_PATTERNS = listOf(
        Regex("""\bstarting navigation\b|\bstarting\b\s*$""", RegexOption.IGNORE_CASE),
        Regex("""\brerouting\b|\bre-routing\b""", RegexOption.IGNORE_CASE),
        Regex("""\bsearching for gps\b|\bgps signal lost\b|\bwaiting for gps\b""",
              RegexOption.IGNORE_CASE),
        Regex("""\bfinding a (faster|better) route\b""", RegexOption.IGNORE_CASE),
        Regex("""\bnavigation (ended|stopped)\b""", RegexOption.IGNORE_CASE),
    )

    private val REDACTED_PATTERN =
        Regex("""\bcontent hidden\b|\bsensitive notification\b""", RegexOption.IGNORE_CASE)

    /** Null when the notification should have held a maneuver. */
    fun skipReason(title: String?, text: String?): Skip? {
        val titleText = title.orEmpty().trim()
        val combined = "$titleText ${text.orEmpty()}".trim()
        if (combined.isEmpty()) return Skip.TRANSIENT

        if (REDACTED_PATTERN.containsMatchIn(combined)) return Skip.REDACTED
        if (TRANSIENT_PATTERNS.any { it.containsMatchIn(combined) }) return Skip.TRANSIENT
        // The bare app name is the collapsed/summary notification; it never
        // carries a maneuver.
        if (titleText.equals("Maps", ignoreCase = true) ||
            titleText.equals("Google Maps", ignoreCase = true)
        ) {
            return Skip.TRANSIENT
        }
        return null
    }

    private val BARE_LEFT = Regex("""\bleft\b""", RegexOption.IGNORE_CASE)
    private val BARE_RIGHT = Regex("""\bright\b""", RegexOption.IGNORE_CASE)

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

        // Motorway exits, ramps and merges. These were missing entirely, and
        // on a real drive they are most of what Maps says -- a capture in
        // Maryland produced "Take the MD-117/MD-124 exit toward Clopper Rd"
        // and parsed as nothing at all.
        //
        // There is no exit glyph in TBT_Icon_t, and a slight divergence is
        // what an exit actually is, so they map to SLIGHT_LEFT/SLIGHT_RIGHT.
        // When Maps does not say which side, this assumes right, which holds
        // for right-hand traffic and is wrong in the UK, Japan and Australia.
        // Maps is explicit ("exit on the left") whenever it is the unusual
        // side, so the assumption only applies where it is also the default.
        Regex("""\bexit\b.*\bon the left\b|\bexit\s+left\b""", RegexOption.IGNORE_CASE)
            to Icon.SLIGHT_LEFT,
        Regex("""\btake\s+(the\s+)?.{0,40}?\bexit\b|\btake\s+exit\b|\bramp\b""",
              RegexOption.IGNORE_CASE) to Icon.SLIGHT_RIGHT,
        Regex("""\bmerge\b.*\bleft\b""", RegexOption.IGNORE_CASE) to Icon.SLIGHT_LEFT,
        Regex("""\bmerge\b.*\bright\b""", RegexOption.IGNORE_CASE) to Icon.SLIGHT_RIGHT,
        Regex("""\bmerge\b""", RegexOption.IGNORE_CASE) to Icon.STRAIGHT,

        // A bare "exit" with no "take" in front of it. Observed on a real
        // drive as "Exit the parking lot toward Game Preserve Rd", which every
        // pattern above misses because they all require the verb.
        //
        // STRAIGHT rather than SLIGHT_RIGHT: leaving a car park implies no
        // turn direction at all, and the side-guess that is reasonable for a
        // motorway slip road would here be inventing one.
        Regex("""\bexit\b""", RegexOption.IGNORE_CASE) to Icon.STRAIGHT,
        Regex("""\barriv|\bdestination\b""", RegexOption.IGNORE_CASE) to Icon.ARRIVE,
        Regex("""\b(continue|straight|head)\b""", RegexOption.IGNORE_CASE) to Icon.STRAIGHT,
        // Last resort: a bare direction word with no verb around it.
        BARE_LEFT to Icon.TURN_LEFT,
        BARE_RIGHT to Icon.TURN_RIGHT,
    )

    // The two last-resort patterns, named so they can be recognised again
    // rather than found by position. They match any sentence containing the
    // word "left" or "right" anywhere -- fine as a fallback when a distance
    // corroborates that this is a maneuver, but far too loose to accept on
    // their own. See the distance handling in parse().
    private val WEAK_PATTERNS = setOf(BARE_LEFT, BARE_RIGHT)

    private fun isStrong(pattern: Regex) = pattern !in WEAK_PATTERNS

    // "350 m", "1.2 km", "500 ft", "0.3 mi" -- Maps uses whichever unit system
    // the phone is set to, so all four have to be understood.
    //
    // The spelled-out forms are here because the abbreviations alone were not
    // enough on the road: a capture showed "Turn right" failing to parse while
    // Maps plainly displayed a distance for it. Only the shorthand was
    // accepted, so any notification phrased "500 feet" or "0.2 miles" lost its
    // distance and the whole maneuver was discarded as unreadable.
    //
    // Longest alternative first so "mi" is preferred over "m" inside "miles".
    // The trailing \b makes that robust rather than merely lucky -- "m" can
    // still match the start of "miles", but then fails the boundary and the
    // engine backtracks into the longer alternative.
    private val DISTANCE_PATTERN = Regex(
        """(\d+(?:[.,]\d+)?)\s*(kilometers?|kilometres?|km|miles?|mi|meters?|metres?|m|feet|foot|ft|yards?|yds?)\b""",
        RegexOption.IGNORE_CASE
    )

    private const val METRES_PER_MILE = 1609.344
    private const val METRES_PER_FOOT = 0.3048
    private const val METRES_PER_YARD = 0.9144

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

        val rule = ICON_PATTERNS.firstOrNull { it.first.containsMatchIn(combined) } ?: return null
        val iconId = rule.second

        // Distance may sit in either line depending on Maps version; arrival
        // notices often carry none at all, which is legitimate.
        //
        // A missing distance used to discard the maneuver outright. On a drive
        // through a housing estate that cost 38 of 109 maneuvers: Maps posts
        // the step as a bare instruction ("Turn right onto Richter Farm Rd")
        // with no distance in either line, and the arrow and street name went
        // in the bin along with the absent number.
        //
        // So a maneuver may now travel without one -- but only when a real
        // instruction was recognised. The two bare direction words match any
        // sentence mentioning "left" or "right", and a distance is what
        // corroborates that such a sentence is a maneuver at all; without one
        // they would forward any passing notification that happened to say
        // "right".
        val distance = parseDistanceMetres(combined) ?: when {
            iconId == Icon.ARRIVE -> 0
            isStrong(rule.first) -> DISTANCE_UNKNOWN
            else -> return null
        }

        return Maneuver(iconId, distance, extractStreet(titleText, bodyText))
    }

    /** Exposed for tests; returns null when no distance is present. */
    fun parseDistanceMetres(input: String): Int? {
        val match = DISTANCE_PATTERN.find(input) ?: return null
        val value = match.groupValues[1].replace(',', '.').toDoubleOrNull() ?: return null
        val unit = match.groupValues[2].lowercase()
        val metres = when {
            unit.startsWith("km") || unit.startsWith("kilomet") -> value * 1000.0
            unit.startsWith("mi") -> value * METRES_PER_MILE
            unit.startsWith("f") -> value * METRES_PER_FOOT // ft, feet, foot
            unit.startsWith("y") -> value * METRES_PER_YARD // yd, yds, yard(s)
            else -> value // m, meter(s), metre(s)
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
        // Whichever line names the road wins, rather than always the body.
        // Preferring a non-empty body unconditionally lost the road on
        // "Exit the parking lot toward Game Preserve Rd" / "300 feet": the
        // body was non-empty, so it was chosen, and it held only a distance --
        // the street came back blank while the title said it plainly.
        val sources = listOf(text, title).filter { it.isNotEmpty() }

        // The LAST introducer, not the first: the road being joined comes at
        // the end, while an aside earlier in the sentence may contain one too.
        // (Matching only the keyword and taking the remainder by index, rather
        // than capturing with (.+) -- a greedy capture consumes to the end of
        // the string, so findAll would only ever return one match.)
        for (source in sources) {
            ROAD_INTRO.findAll(source).lastOrNull()?.let { match ->
                return cleanUp(source.substring(match.range.last + 1))
            }
        }

        // No explicit introducer: the line itself is generally the road. Drop
        // a leading distance so "350 m - Main St" doesn't repeat what the head
        // unit already shows in its own distance field, and fall through to
        // the other line when that leaves nothing behind.
        for (source in sources) {
            val cleaned = cleanUp(source.replace(DISTANCE_PATTERN, ""))
            if (cleaned.isNotEmpty()) return cleaned
        }
        return ""
    }

    /**
     * Maps often names every road an exit serves:
     *   "toward Clopper Rd/W. Diamond Ave/Mont. Village Ave/Quince Orch. Rd"
     * Truncating that to 31 bytes yields a run-on ending mid-word. The first
     * alternative is the one a rider is looking for on a sign, so cut there
     * when the whole list will not fit anyway.
     */
    private fun firstAlternative(value: String): String {
        if (value.length <= 31 || !value.contains('/')) {
            return value
        }
        val head = value.substringBefore('/').trim()
        // A road number like "MD-117/MD-124" is one name, not a list: keep it
        // whole when the head alone is implausibly short.
        return if (head.length >= 4) head else value
    }

    /**
     * The key under which a failed phrasing is remembered, so that one wording
     * occupies one slot however many times it recurs.
     *
     * Maps counts the distance down continuously, so a single unrecognised
     * manoeuvre arrives dozens of times as "0.4 mi ...", "0.3 mi ...", "600 ft
     * ..." -- all the same missing pattern. Stripping the leading distance is
     * what makes a handful of remembered samples cover a whole drive.
     *
     * Reuses DISTANCE_PATTERN rather than restating the units: written out
     * again by hand, the alternation read (km|m|mi|ft) with no word boundary,
     * so "200 mi" matched as "200 m" and left a stray "i" at the front of the
     * key -- which would have split one wording back across several slots.
     */
    fun unparsedKey(title: String?, text: String? = null): String {
        val head = stripLeadingDistance(title.orEmpty().trim())
        val body = stripLeadingDistance(text.orEmpty().trim())

        // Both halves, because the title alone was not enough to diagnose the
        // one that mattered: "Turn right" was recorded as the failing sample
        // with no way to see whether Maps had put a distance in the body in a
        // wording the parser did not accept. A key that hides half the
        // evidence costs another drive to find out.
        return when {
            head.isEmpty() && body.isEmpty() -> "(no title or text)"
            body.isEmpty() -> head
            head.isEmpty() -> "(no title) | $body"
            else -> "$head | $body"
        }
    }

    /**
     * Drops a distance only where it leads, which is where Maps puts the
     * counting-down one. A distance inside the sentence ("In 500 ft, use the
     * middle lane") is part of the phrasing and is kept.
     *
     * Deliberately not cleanUp(): that truncates at the first "/", which is
     * right for a road name shown on a 240px panel and wrong for a diagnostic
     * key, where hiding half the string is the whole problem being fixed.
     */
    private fun stripLeadingDistance(raw: String): String {
        if (raw.isEmpty()) return raw
        val match = DISTANCE_PATTERN.find(raw)
        if (match == null || match.range.first > 0) return raw

        var rest = raw.substring(match.range.last + 1).trimStart()
        while (rest.isNotEmpty() && (rest[0] == '·' || rest[0] == '-' || rest[0] == '–' ||
                rest[0] == ',' || rest[0] == ':')
        ) {
            rest = rest.substring(1).trimStart()
        }
        // A title that was *only* a distance keeps its original text rather
        // than collapsing to an empty key.
        return rest.ifEmpty { raw }
    }

    private fun cleanUp(value: String): String {
        var result = firstAlternative(value.trim())

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
