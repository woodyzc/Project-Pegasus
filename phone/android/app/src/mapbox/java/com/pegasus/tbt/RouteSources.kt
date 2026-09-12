package com.pegasus.tbt

import android.content.Context

/**
 * The route source for a build with the Mapbox SDK compiled in.
 *
 * Built when withMapbox is on; see app/build.gradle.kts. Its counterpart in
 * src/nomapbox returns null, and RouteSourceFactory keeps the two in step.
 */
object RouteSources : RouteSourceFactory {

    // Never shown in this build, since create() always returns a source.
    // Present because the contract requires it, which is what keeps the two
    // source sets from drifting apart.
    override val unavailable = "Routing is unavailable."

    override fun create(context: Context): RouteSource? = MapboxRouteSource(context)
}
