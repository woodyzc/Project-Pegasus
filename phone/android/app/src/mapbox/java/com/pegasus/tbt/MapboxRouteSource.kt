package com.pegasus.tbt

import android.content.Context
import android.util.Log
import com.mapbox.api.directions.v5.DirectionsCriteria
import com.mapbox.api.directions.v5.models.DirectionsRoute
import com.mapbox.api.directions.v5.models.RouteOptions
import com.mapbox.geojson.Point as MapboxPoint
import com.mapbox.geojson.utils.PolylineUtils
import com.mapbox.common.MapboxOptions
import com.mapbox.navigation.base.ExperimentalPreviewMapboxNavigationAPI
import com.mapbox.navigation.base.options.NavigationOptions
import com.mapbox.navigation.base.route.NavigationRoute
import com.mapbox.navigation.base.route.NavigationRouterCallback
import com.mapbox.navigation.base.route.RouterFailure
import com.mapbox.navigation.core.MapboxNavigation
import com.mapbox.navigation.core.MapboxNavigationProvider
import com.mapbox.navigation.core.directions.session.RoutesExtra
import com.mapbox.navigation.core.directions.session.RoutesObserver
import com.mapbox.navigation.core.trip.session.LocationMatcherResult
import com.mapbox.navigation.core.trip.session.LocationObserver
import com.mapbox.navigation.core.trip.session.RouteProgressObserver

/**
 * The only file in this app that touches the Mapbox SDK.
 *
 * ---------------------------------------------------------------------------
 * COMPILED, AGAINST navigationcore 3.6.0. NOT YET RUN ON A ROUTE.
 * ---------------------------------------------------------------------------
 * It was written from the published API long before anyone could build it, and
 * it now compiles against the real SDK -- so the signatures below are checked,
 * and nothing about the behaviour is. What the compiler cannot tell you is
 * whether the maneuvers map sensibly, whether the reroute path fires when a
 * rider actually leaves the line, or whether the geometry precision is what
 * the route says it is.
 *
 * The Navigation SDK is served from Mapbox's own Maven repository, which
 * refuses anonymous access, so building it needs a secret token with the
 * DOWNLOADS:READ scope for Gradle. The public token the app routes with is not
 * a build input at all -- it is entered on the phone, see MapboxToken.
 *
 * Build it with:  ./gradlew -PwithMapbox=true assembleDebug
 *
 * Pin the version when reading this against the SDK docs: the shape changed at
 * v3 and will change again.
 *
 * Everything that could be written without the SDK was: the maneuver mapping
 * is in MapboxManeuver, the route model in PlannedRoute, the wire format in
 * RouteFrame, the upload in RouteTransfer. All four are unit tested. This file
 * is deliberately thin so the untested surface is as small as it can be.
 */
class MapboxRouteSource(private val context: Context) : RouteSource {

    companion object {
        private const val TAG = "PegasusMapbox"

        /**
         * Mapbox returns geometry as a polyline with either 5 or 6 decimal
         * places of precision, and which one depends on the request. The
         * mismatch decodes to coordinates off by a factor of ten -- a route
         * across the wrong continent -- so the request below pins it.
         */
        private const val GEOMETRY_PRECISION = DirectionsCriteria.GEOMETRY_POLYLINE6
    }

    override var onInstruction: ((NavigationInstruction) -> Unit)? = null

    override var onRoutePlanned: ((PlannedRoute) -> Unit)? = null

    private var navigation: MapboxNavigation? = null
    private var routeId = 0

    /**
     * The rider's last known position, kept as the origin for the next route
     * request.
     *
     * There is no separate location client. The trip session is already
     * receiving fixes in order to produce route progress, so taking the origin
     * from the same stream means one source of position rather than two that
     * can disagree.
     */
    @Volatile
    private var lastKnown: MapboxPoint? = null

    private val locationObserver = object : LocationObserver {
        override fun onNewRawLocation(rawLocation: com.mapbox.common.location.Location) {
            lastKnown = MapboxPoint.fromLngLat(rawLocation.longitude, rawLocation.latitude)
        }

        override fun onNewLocationMatcherResult(locationMatcherResult: LocationMatcherResult) {
            // The map-matched position, which is the raw fix snapped to the
            // road network. Better than raw for a route origin: it starts the
            // route on the road the rider is actually on rather than on a
            // parallel one a few metres away.
            val matched = locationMatcherResult.enhancedLocation
            lastKnown = MapboxPoint.fromLngLat(matched.longitude, matched.latitude)
        }
    }

    /**
     * The geometry of the route last handed to the head unit, so the same one
     * is not uploaded twice.
     *
     * The upload is a chunked BLE transfer of the whole polyline, slow enough
     * that MainActivity shows a percentage for it. The SDK is entitled to
     * announce a route set for reasons that do not change the line, and
     * re-sending several kilobytes over BLE each time would compete with the
     * turns for the same link.
     */
    private var uploadedGeometry: String? = null

    /**
     * Every change to the route the SDK is actually navigating, including the
     * ones it makes by itself.
     *
     * This is what keeps the head unit's cached route honest. The SDK reroutes
     * on its own when the rider leaves the line, and before this observer
     * existed nothing noticed: onRoutePlanned was wired only to the callback
     * of an explicit requestRoutes, so it fired once, for the route the rider
     * asked for, and never again. The head unit kept the original polyline for
     * the rest of the ride.
     *
     * That is worse than it sounds, because the head unit does not merely
     * display the cached route -- it navigates from it whenever the phone goes
     * quiet, and its off-route threshold is 50m. A reroute that runs near the
     * old line therefore lets it snap to the obsolete polyline and announce a
     * turn with no hint that anything is wrong. A red arrow would at least be
     * honest.
     *
     * The initial route comes through here too, which is why requestRoutes'
     * own callback no longer uploads: one path, exercised on every ride,
     * rather than a common one and a rare one that can drift apart.
     */
    private val routesObserver = RoutesObserver { result ->
        // REFRESH carries new traffic or annotations for the same geometry,
        // and ALTERNATIVE changes only the routes that were not chosen.
        // Neither moves the line the head unit is following.
        val reason = result.reason
        if (reason != RoutesExtra.ROUTES_UPDATE_REASON_NEW &&
            reason != RoutesExtra.ROUTES_UPDATE_REASON_REROUTE
        ) {
            return@RoutesObserver
        }

        // An empty list is the route being cleared. Nothing to upload, and the
        // head unit ages its turns out on its own; what matters here is
        // forgetting the geometry, so the same route re-planned later is sent
        // again rather than suppressed as a duplicate.
        val primary = result.navigationRoutes.firstOrNull()
        if (primary == null) {
            uploadedGeometry = null
            return@RoutesObserver
        }

        val route = primary.directionsRoute
        val geometry = route.geometry()
        if (geometry != null && geometry == uploadedGeometry) {
            return@RoutesObserver
        }
        uploadedGeometry = geometry

        Log.i(TAG, "uploading route to the head unit, reason=$reason")
        onRoutePlanned?.invoke(toPlannedRoute(route))
    }

    private val progressObserver = RouteProgressObserver { progress ->
        val legProgress = progress.currentLegProgress ?: return@RouteProgressObserver
        val stepProgress = legProgress.currentStepProgress ?: return@RouteProgressObserver
        val maneuver = stepProgress.step?.maneuver()

        val icon = MapboxManeuver.toIcon(maneuver?.type(), maneuver?.modifier())

        onInstruction?.invoke(
            NavigationInstruction(
                iconId = icon,
                // distanceRemaining is a Float of metres to the next maneuver,
                // which is exactly what the head unit counts down.
                distanceMetres = stepProgress.distanceRemaining.toInt().coerceAtLeast(0),
                streetName = stepProgress.step?.name().orEmpty(),
                exitNumber = maneuver?.exit()?.toInt() ?: 0,
                remainingMetres = progress.distanceRemaining.toInt(),
            )
        )
    }

    /**
     * Returns an error string if the app cannot route, or null if it can.
     *
     * Checked up front and reported in words, because the failure without this
     * is a routing request that silently returns nothing -- which looks
     * identical to being out of network, or to Mapbox being down, or to the
     * route simply not existing.
     */
    override fun unavailableReason(): String? {
        val token = MapboxToken.load(context)
        if (token.isEmpty()) {
            return "No Mapbox token. Enter the public pk. token on the main screen."
        }
        // Entry already refuses a malformed token, but a token stored by an
        // older build, or one revoked since, still has to be reported here
        // rather than becoming a silent routing failure.
        return MapboxToken.Rules.rejectionReason(token)
    }

    // startTripSessionWithPermissionCheck is still marked preview in 3.6.0.
    // Opting in here rather than on the class keeps the annotation next to the
    // one call it covers, so a future version that promotes or removes it
    // fails at this line instead of somewhere unrelated.
    @OptIn(ExperimentalPreviewMapboxNavigationAPI::class)
    override fun start() {
        unavailableReason()?.let {
            Log.e(TAG, it)
            return
        }
        // v3 takes the token globally rather than on NavigationOptions,
        // which is one of the things that moved at the major version.
        MapboxOptions.accessToken = MapboxToken.load(context)

        // MapboxNavigationProvider, not MapboxNavigationApp.
        //
        // MapboxNavigationApp builds its instance only while a LifecycleOwner
        // is attached, and hands back null until one is. Our owner is a
        // foreground Service, precisely because the phone spends a ride in a
        // pocket with no Activity alive -- so current() was always null and
        // every route request answered "Routing is not started." Provider
        // creates the instance outright and ties it to nothing.
        val nav = if (MapboxNavigationProvider.isCreated()) {
            MapboxNavigationProvider.retrieve()
        } else {
            MapboxNavigationProvider.create(NavigationOptions.Builder(context).build())
        }
        // Observers are registered once. start() is safe to call again --
        // and is, the moment location permission is granted -- because the
        // service comes up before the user has answered that dialog, and a
        // trip session refused for want of permission has to be retried by
        // something. Registering twice would deliver every turn twice.
        if (navigation == null) {
            navigation = nav
            nav.registerRouteProgressObserver(progressObserver)
            nav.registerLocationObserver(locationObserver)
            nav.registerRoutesObserver(routesObserver)
        }

        // Without this the SDK emits no location and no route progress at all,
        // so a route can be planned and then never produce a single turn. It
        // was the quiet half of "compiles but does nothing".
        //
        // The WithPermissionCheck variant refuses politely when location has
        // not been granted, where the plain one throws.
        runCatching { nav.startTripSessionWithPermissionCheck() }
            .onFailure { Log.e(TAG, "trip session refused: ${it.message}") }
    }

    override fun stop() {
        navigation?.let { nav ->
            nav.unregisterRouteProgressObserver(progressObserver)
            nav.unregisterLocationObserver(locationObserver)
            nav.unregisterRoutesObserver(routesObserver)
            nav.stopTripSession()
        }
        navigation = null
        lastKnown = null
        // Forgotten with the session. The head unit keeps nothing across a
        // restart of this service either -- its route lives in PSRAM -- so
        // remembering a geometry here would suppress the upload that the head
        // unit is waiting for.
        uploadedGeometry = null
        // Releases the native engine and the location subscription with it.
        if (MapboxNavigationProvider.isCreated()) {
            MapboxNavigationProvider.destroy()
        }
    }

    /**
     * Plans a cycling route from the rider's current position to [destination].
     *
     * The origin is not a parameter. Asking the caller for one meant either a
     * second location client or a screen that makes the rider type where they
     * already are; the trip session knows, so it supplies it.
     */
    override fun requestRouteTo(destination: Destination): String? {
        // unavailableReason first: a missing or bad token is a specific,
        // actionable answer, and reporting "not started" over the top of it
        // sends the reader looking in the wrong place.
        unavailableReason()?.let { return it }
        if (navigation == null) return "Routing is not started."
        val origin = lastKnown
            ?: return "No position yet. Wait for a GPS fix, or check location permission."
        requestRoute(
            from = Pair(origin.latitude(), origin.longitude()),
            to = Pair(destination.latitude, destination.longitude),
        )
        return null
    }

    /**
     * Plans a cycling route and starts guidance.
     *
     * PROFILE_CYCLING matters beyond politeness: the driving profile routes a
     * bike onto trunk roads and misses every path and cycleway, which is most
     * of what this head unit exists to follow.
     */
    private fun requestRoute(from: Pair<Double, Double>, to: Pair<Double, Double>) {
        val nav = navigation ?: return

        val options = RouteOptions.builder()
            .applyDefaultNavigationOptions(DirectionsCriteria.PROFILE_CYCLING)
            .coordinatesList(
                listOf(
                    MapboxPoint.fromLngLat(from.second, from.first),
                    MapboxPoint.fromLngLat(to.second, to.first),
                )
            )
            .geometries(GEOMETRY_PRECISION)
            // Without steps the response carries no maneuvers at all, and the
            // whole offline half of this feature has nothing to upload.
            .steps(true)
            .overview(DirectionsCriteria.OVERVIEW_FULL)
            .build()

        nav.requestRoutes(options, object : NavigationRouterCallback {
            override fun onRoutesReady(routes: List<NavigationRoute>, routerOrigin: String) {
                if (routes.isEmpty()) return
                // The upload is not done here. setNavigationRoutes fires the
                // RoutesObserver above, which is the single place a route
                // reaches the head unit -- the SDK's own reroutes arrive the
                // same way, and two upload paths would drift apart.
                nav.setNavigationRoutes(routes)
            }

            override fun onFailure(reasons: List<RouterFailure>, routeOptions: RouteOptions) {
                Log.w(TAG, "route request failed: $reasons")
            }

            override fun onCanceled(routeOptions: RouteOptions, routerOrigin: String) {
                Log.i(TAG, "route request cancelled")
            }
        })
    }

    /**
     * Flattens a Mapbox route into this app's own model.
     *
     * Legs are concatenated. The head unit has one polyline and one maneuver
     * list keyed to distance along it, and a leg boundary is not a thing a
     * rider experiences -- it is an artefact of having asked for waypoints.
     */
    private fun toPlannedRoute(route: DirectionsRoute): PlannedRoute {
        routeId++

        val points = decodeGeometry(route).map {
            RouteFrame.Point(it.latitude(), it.longitude())
        }

        val steps = ArrayList<PlannedRoute.Step>()
        for (leg in route.legs().orEmpty()) {
            for (step in leg.steps().orEmpty()) {
                val maneuver = step.maneuver()
                steps.add(
                    PlannedRoute.Step(
                        iconId = MapboxManeuver.toIcon(maneuver.type(), maneuver.modifier()),
                        streetName = step.name().orEmpty(),
                        // Length OF this step. PlannedRoute.fromSteps turns
                        // these into distances along the route, which is the
                        // conversion the head unit needs and the one that is
                        // invisible when wrong.
                        distanceMetres = step.distance().toInt(),
                        exitNumber = maneuver.exit()?.toInt() ?: 0,
                    )
                )
            }
        }

        return PlannedRoute.fromSteps(routeId, points, steps)
    }

    /**
     * Decodes the route's polyline at the precision the route was requested
     * with.
     *
     * Not hardcoded to 6, even though requestRoute always asks for polyline6.
     * Decoding a 5-decimal polyline as 6 does not fail -- it yields
     * coordinates a factor of ten out, which is a route on the wrong
     * continent -- and a route arriving from anywhere but requestRoute carries
     * its own setting. Reading it back off the route is the only way to be
     * sure the two agree.
     */
    private fun decodeGeometry(route: DirectionsRoute): List<MapboxPoint> {
        val geometry = route.geometry() ?: return emptyList()
        val precision = if (route.routeOptions()?.geometries() ==
            DirectionsCriteria.GEOMETRY_POLYLINE
        ) 5 else 6
        return PolylineUtils.decode(geometry, precision)
    }
}

/**
 * Mapbox's own defaults, minus the profile, which is the one that matters here.
 * Kept as an extension so the builder above reads as a single expression.
 */
private fun RouteOptions.Builder.applyDefaultNavigationOptions(
    profile: String,
): RouteOptions.Builder = this
    .baseUrl("https://api.mapbox.com")
    .user("mapbox")
    .profile(profile)
    .annotationsList(listOf(DirectionsCriteria.ANNOTATION_DISTANCE))
    .alternatives(false)
    .continueStraight(true)
    .roundaboutExits(true)
    .voiceInstructions(false)
    .bannerInstructions(true)
