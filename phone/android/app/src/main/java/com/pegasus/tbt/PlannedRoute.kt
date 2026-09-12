package com.pegasus.tbt

/**
 * A planned route and the live instruction along it, in this app's own terms.
 *
 * Deliberately free of any Mapbox type. The SDK is a large, credentialed
 * dependency that cannot be resolved without a paid account token, so
 * everything that can be written without it is -- which is nearly all of it.
 * Only MapboxRouteSource touches the SDK, and it exists to fill these in.
 *
 * That seam also means the head unit's contract is testable on a machine with
 * no Mapbox credentials at all, which is the machine this project is built on.
 */

/**
 * One live turn, the shape Phase 1 of the plan asks for.
 *
 * The same three fields TbtFrame puts on the wire, plus the roundabout exit
 * that Google Maps' notification text could never reliably supply and Mapbox
 * states outright.
 */
data class NavigationInstruction(
    val iconId: Int,
    val distanceMetres: Int,
    val streetName: String,
    val exitNumber: Int = 0,
    /** Metres remaining on the whole route, for an ETA readout. Negative when unknown. */
    val remainingMetres: Int = -1,
) {
    fun toFrame(): ByteArray = TbtFrame.encode(iconId, distanceMetres, streetName)
}

/**
 * A whole planned route, ready to be encoded and uploaded.
 *
 * `maneuvers` must be in ascending `distanceAlongRouteM` order. The head unit's
 * RouteFollow_NextManeuver walks them in order and takes the first at or past
 * the rider's position, so an unsorted list navigates badly rather than
 * failing -- which is exactly the kind of fault that reaches a road. [sorted]
 * exists so the caller cannot forget.
 */
data class PlannedRoute(
    val routeId: Int,
    val points: List<RouteFrame.Point>,
    val maneuvers: List<RouteFrame.Maneuver>,
    val totalLengthM: Int,
) {
    /** The same route with its maneuvers in the order the head unit expects. */
    fun sorted(): PlannedRoute =
        copy(maneuvers = maneuvers.sortedBy { it.distanceAlongRouteM })

    fun encode(): RouteFrame.Encoded =
        RouteFrame.encode(routeId, points, sorted().maneuvers, totalLengthM)

    companion object {
        /**
         * Builds a route from the geometry and step list any directions API
         * returns, in the shape they all share: a polyline, and steps that
         * each carry a maneuver and the distance that step covers.
         *
         * The conversion that matters is the last argument. Directions APIs
         * report distance PER STEP -- how far this step runs before the next
         * maneuver -- while the head unit needs distance ALONG THE ROUTE from
         * the start, because that is what a GPS fix snapped to the polyline
         * can be compared against. Getting this wrong puts every maneuver at
         * the wrong place and is not visible until a rider is at a junction.
         */
        fun fromSteps(
            routeId: Int,
            points: List<RouteFrame.Point>,
            steps: List<Step>,
        ): PlannedRoute {
            val maneuvers = ArrayList<RouteFrame.Maneuver>(steps.size)
            var along = 0

            for (step in steps) {
                // A step's maneuver happens at its START, so the running total
                // is the distance to it. The step's own length then advances
                // the total for the next one.
                maneuvers.add(
                    RouteFrame.Maneuver(
                        iconId = step.iconId,
                        exitNumber = step.exitNumber,
                        distanceAlongRouteM = along,
                        streetName = step.streetName,
                    )
                )
                along += step.distanceMetres
            }

            return PlannedRoute(routeId, points, maneuvers, along)
        }
    }

    /** One step of a directions response, reduced to what the head unit uses. */
    data class Step(
        val iconId: Int,
        val streetName: String,
        /** Length of this step, NOT the distance from the route start. */
        val distanceMetres: Int,
        val exitNumber: Int = 0,
    )
}
