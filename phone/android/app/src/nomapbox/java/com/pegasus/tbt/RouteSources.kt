package com.pegasus.tbt

import android.content.Context

/**
 * The route source for a build without the Mapbox SDK, which is the default.
 *
 * Returns null rather than a stub that fails later. A caller that has to
 * handle "no routing compiled in" explicitly cannot forget to, and the app's
 * other half -- Google Maps notifications -- does not need this at all.
 *
 * Built when withMapbox is off; see app/build.gradle.kts. Its counterpart is
 * src/mapbox/java/com/pegasus/tbt/RouteSources.kt, and the two must keep the
 * same shape, which RouteSourceFactory is there to enforce.
 */
object RouteSources : RouteSourceFactory {

    override val unavailable =
        "Built without the Mapbox SDK. Rebuild with -PwithMapbox=true to plan routes."

    override fun create(context: Context): RouteSource? = null
}
