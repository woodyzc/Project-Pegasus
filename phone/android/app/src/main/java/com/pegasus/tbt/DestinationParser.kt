package com.pegasus.tbt

/**
 * Turns whatever the rider pastes into a destination.
 *
 * There is no map picker and no place search in this app, deliberately: a map
 * needs another large SDK and a search needs another billed API, and both
 * would be a lot of surface for a field used once at the start of a ride. What
 * a phone already gives you is a Google Maps link on the clipboard, so this
 * reads one of those, and plain coordinates for everything else.
 *
 * Pure -- no Android types, no network -- so every case below is a unit test
 * rather than something discovered at a trailhead.
 */
object DestinationParser {

    sealed class Result {
        data class Ok(val destination: Destination) : Result()
        data class Err(val reason: String) : Result()
    }

    /**
     * The pin's own coordinates, which Maps puts in the data segment as
     * !3d<lat>!4d<lon>.
     *
     * Tried before the @ form on purpose. In a place URL the @ is the CAMERA
     * position -- where the map was centred when the link was made -- and the
     * !3d!4d pair is the place itself. They are usually close and occasionally
     * are not, and the difference is a destination on the wrong side of a
     * river.
     */
    private val PIN = Regex("""!3d(-?\d+\.?\d*)!4d(-?\d+\.?\d*)""")

    /** The camera position in a /maps/@lat,lon,zoom URL. */
    private val AT = Regex("""@(-?\d+\.?\d*),(-?\d+\.?\d*)""")

    /** ?q=, ?query=, ?daddr= and friends, when they hold a coordinate pair. */
    private val QUERY = Regex(
        """[?&](?:q|query|daddr|destination|ll|sll)=(-?\d+\.?\d*),(-?\d+\.?\d*)""",
        RegexOption.IGNORE_CASE,
    )

    /** A geo: URI, which is what Android's own share sheet produces. */
    private val GEO = Regex("""^geo:(-?\d+\.?\d*),(-?\d+\.?\d*)""", RegexOption.IGNORE_CASE)

    /** Bare "lat, lon", the fallback and the thing you can always type. */
    private val PAIR = Regex("""^\s*(-?\d+\.?\d*)\s*,\s*(-?\d+\.?\d*)\s*$""")

    fun parse(raw: String): Result {
        val text = raw.trim()
        if (text.isEmpty()) return Result.Err("Nothing entered.")

        // Named before anything else is tried, because it is the one paste
        // that looks completely reasonable and cannot possibly work: a
        // shortened link is an opaque id, and the coordinates live only on
        // Google's server behind a redirect.
        if (text.contains("maps.app.goo.gl") || text.contains("goo.gl/maps")) {
            return Result.Err(
                "That is a shortened Maps link and carries no coordinates. " +
                    "Open it in Maps, then copy the full link from the address bar."
            )
        }

        for (pattern in listOf(PIN, AT, QUERY, GEO, PAIR)) {
            val match = pattern.find(text) ?: continue
            val lat = match.groupValues[1].toDoubleOrNull() ?: continue
            val lon = match.groupValues[2].toDoubleOrNull() ?: continue
            return validate(lat, lon)
        }

        return Result.Err(
            "Could not find coordinates in that. Paste a full Google Maps " +
                "link, or type \"latitude, longitude\"."
        )
    }

    /**
     * Ranges, and the swap.
     *
     * Latitude past 90 is the signature of a lon,lat pair typed the wrong way
     * round, which is easy to do and produces a route to somewhere plausible
     * rather than an error, so it is worth naming.
     */
    private fun validate(lat: Double, lon: Double): Result = when {
        lat < -90.0 || lat > 90.0 ->
            Result.Err(
                "Latitude $lat is out of range. If those are the right " +
                    "numbers, they are the wrong way round: latitude first."
            )
        lon < -180.0 || lon > 180.0 ->
            Result.Err("Longitude $lon is out of range.")
        else -> Result.Ok(Destination(lat, lon))
    }

    /** How a destination reads back on screen, so a bad paste is visible. */
    fun describe(destination: Destination): String =
        String.format("%.5f, %.5f", destination.latitude, destination.longitude)
}
