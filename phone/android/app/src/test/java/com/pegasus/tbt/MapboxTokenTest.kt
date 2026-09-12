package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Covers the token rules, which are the part that has to be right: they are
 * the only thing standing between pasting the wrong token and discovering it
 * mid-ride.
 */
class MapboxTokenTest {

    // Shaped like a real one -- "pk." and a long opaque body -- without being
    // one. Nothing in this repo should ever contain a live token.
    private val valid = "pk.eyJ1IjoicGVnYXN1cyIsImEiOiJjbTAwMDAwMDAwMDAwMCJ9.AbCdEfGh"

    @Test
    fun `accepts a well formed public token`() {
        assertNull(MapboxToken.Rules.rejectionReason(valid))
    }

    @Test
    fun `trims surrounding whitespace rather than refusing`() {
        // Pasting from a browser routinely brings a trailing newline.
        assertNull(MapboxToken.Rules.rejectionReason("  $valid\n"))
    }

    @Test
    fun `refuses the secret token by name`() {
        val reason = MapboxToken.Rules.rejectionReason("sk.eyJ1IjoicGVnYXN1cyJ9.SecretSecret")
        assertNotNull(reason)
        // The message has to say WHICH token this is, because "invalid token"
        // would leave the user pasting the same thing again.
        assertTrue(reason!!.contains("secret"))
        assertTrue(reason.contains("pk."))
    }

    @Test
    fun `refuses something that is neither`() {
        assertNotNull(MapboxToken.Rules.rejectionReason("eyJ1IjoicGVnYXN1cyJ9.NoPrefixHere"))
    }

    @Test
    fun `refuses empty and blank input`() {
        assertNotNull(MapboxToken.Rules.rejectionReason(""))
        assertNotNull(MapboxToken.Rules.rejectionReason("   "))
    }

    @Test
    fun `refuses a token with a break inside it`() {
        // A partial selection that grabbed two lines of the dashboard.
        val reason = MapboxToken.Rules.rejectionReason("pk.eyJ1Ijoi\npegasus0000")
        assertNotNull(reason)
    }

    @Test
    fun `refuses an obviously truncated token`() {
        assertNotNull(MapboxToken.Rules.rejectionReason("pk.eyJ1"))
    }

    @Test
    fun `redaction shows the ends and hides the middle`() {
        val shown = MapboxToken.Rules.redact(valid)
        assertTrue(shown.startsWith("pk.eyJ1"))
        assertTrue(shown.endsWith(valid.takeLast(4)))
        // The point of redaction: a screenshot of this must not be usable.
        assertTrue(shown.length < valid.length)
    }

    @Test
    fun `redaction of nothing says nothing`() {
        assertEquals("none", MapboxToken.Rules.redact(""))
        assertEquals("none", MapboxToken.Rules.redact("   "))
    }

    @Test
    fun `redaction never echoes a short string whole`() {
        // Guards the branch that would otherwise print a stub in full.
        assertEquals("pk.…", MapboxToken.Rules.redact("pk.short"))
    }
}
