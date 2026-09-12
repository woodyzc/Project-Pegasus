package com.pegasus.tbt

import android.content.Context

/** Where the rider is going. */
data class Destination(val latitude: Double, val longitude: Double)

/**
 * A thing that plans a route and reports turns along it.
 *
 * This interface exists so that TbtService, which lives in src/main, can own a
 * route source without src/main ever referring to a Mapbox type. The Mapbox
 * SDK is an opt-in dependency that most builds here cannot even resolve, so a
 * direct reference would make the default build -- and every unit test --
 * depend on a Mapbox account.
 *
 * The implementation is chosen at build time by RouteSources.create(), which
 * has one version in src/mapbox and one in src/nomapbox, and the build script
 * picks the source set. Callers get null when there is no SDK compiled in.
 */
interface RouteSource {

    /** Fires whenever the next turn changes. */
    var onInstruction: ((NavigationInstruction) -> Unit)?

    /** Fires once when a route is planned, with the whole thing to upload. */
    var onRoutePlanned: ((PlannedRoute) -> Unit)?

    /** Why routing cannot work right now, in words, or null if it can. */
    fun unavailableReason(): String?

    fun start()

    fun stop()

    /**
     * Plans a route from wherever the rider is to [destination].
     *
     * Returns a reason it could not be started, or null if a request went out.
     * Planning is asynchronous, so a null return means "asked", not "done" --
     * the route arrives on onRoutePlanned.
     */
    fun requestRouteTo(destination: Destination): String?
}

/**
 * Builds the route source this build actually contains.
 *
 * Two implementations, selected by the withMapbox build flag. Declared here as
 * documentation; the real bodies are in src/mapbox and src/nomapbox.
 */
interface RouteSourceFactory {
    /**
     * What to tell the user when [create] returns null.
     *
     * On the contract rather than on one implementation: a message that exists
     * in only one source set compiles in one build and breaks the other, which
     * is exactly what happened the first time this was written.
     */
    val unavailable: String

    fun create(context: Context): RouteSource?
}
