package com.pegasus.tbt

import android.content.Context
import android.util.Log
import com.mapbox.api.directions.v5.DirectionsCriteria
import com.mapbox.api.directions.v5.models.DirectionsRoute
import com.mapbox.api.directions.v5.models.RouteOptions
import com.mapbox.geojson.Point as MapboxPoint
import com.mapbox.navigation.base.options.NavigationOptions
import com.mapbox.navigation.base.route.NavigationRoute
import com.mapbox.navigation.base.route.NavigationRouterCallback
import com.mapbox.navigation.base.route.RouterFailure
import com.mapbox.navigation.core.MapboxNavigation
import com.mapbox.navigation.core.lifecycle.MapboxNavigationApp
import com.mapbox.navigation.core.trip.session.RouteProgressObserver

/**
 * The only file in this app that touches the Mapbox SDK.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE IS NOT COMPILED BY DEFAULT AND HAS NEVER BEEN COMPILED HERE.
 * ---------------------------------------------------------------------------
 * The Navigation SDK is served from Mapbox's own Maven repository, which
 * refuses anonymous access, so building it needs a Mapbox account: a secret
 * token with the DOWNLOADS:READ scope for Gradle, and a public token for the
 * app. Neither exists on the machine this was written on, so every SDK call
 * below is written from the published API and has not been checked by a
 * compiler, let alone run. Treat it as a starting point that will need
 * adjusting against whatever version you actually resolve -- the SDK's shape
 * changed at v3, and it will change again.
 *
 * Build it with:  ./gradlew -PwithMapbox=true assembleDebug
 *
 * Everything that could be written without the SDK was: the maneuver mapping
 * is in MapboxManeuver, the route model in PlannedRoute, the wire format in
 * RouteFrame, the upload in RouteTransfer. All four are unit tested. This file
 * is deliberately thin so the untested surface is as small as it can be.
 */
class MapboxRouteSource(private val context: Context) {

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

    /** Fires whenever the next turn changes. */
    var onInstruction: ((NavigationInstruction) -> Unit)? = null

    /** Fires once when a route is planned, with the whole thing to upload. */
    var onRoutePlanned: ((PlannedRoute) -> Unit)? = null

    private var navigation: MapboxNavigation? = null
    private var routeId = 0

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

    fun start() {
        if (!MapboxNavigationApp.isSetup()) {
            MapboxNavigationApp.setup(NavigationOptions.Builder(context).build())
        }
        navigation = MapboxNavigationApp.current()
        navigation?.registerRouteProgressObserver(progressObserver)
    }

    fun stop() {
        navigation?.unregisterRouteProgressObserver(progressObserver)
        navigation = null
    }

    /**
     * Plans a cycling route and starts guidance.
     *
     * PROFILE_CYCLING matters beyond politeness: the driving profile routes a
     * bike onto trunk roads and misses every path and cycleway, which is most
     * of what this head unit exists to follow.
     */
    fun requestRoute(from: Pair<Double, Double>, to: Pair<Double, Double>) {
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
                val first = routes.firstOrNull() ?: return
                nav.setNavigationRoutes(routes)
                onRoutePlanned?.invoke(toPlannedRoute(first.directionsRoute))
            }

            override fun onFailure(reasons: List<RouterFailure>, options: RouteOptions) {
                Log.w(TAG, "route request failed: $reasons")
            }

            override fun onCanceled(options: RouteOptions, routerOrigin: String) {
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

        val points = route.completeGeometryToPoints().map {
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
