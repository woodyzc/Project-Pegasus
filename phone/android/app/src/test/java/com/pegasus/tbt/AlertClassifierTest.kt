package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The rules that decide what reaches the rider.
 *
 * Both directions matter and they fail differently. A rule that is too loose
 * puts a banner over the metric cells every few minutes for a shopping app's
 * promotion; a rule that is too tight silences a call. Each case below is one
 * or the other.
 */
class AlertClassifierTest {

    private companion object {
        const val WECHAT = "com.tencent.mm"
        const val MESSAGES = "com.google.android.apps.messaging"
        const val DIALER = "com.google.android.dialer"
        const val MAPS = "com.google.android.apps.maps"
    }

    @Test
    fun `a wechat message names the conversation`() {
        val alert = AlertClassifier.classify(WECHAT, "msg", 0, "张三")
        assertEquals(AlertFrame.KIND_CHAT, alert?.kind)
        assertEquals("张三", alert?.name)
    }

    @Test
    fun `a group chat is named by the group, not the speaker`() {
        // WeChat puts the group in the title and "speaker: message" in the
        // text. The group is what the rider recognises, and the text is the
        // message body, which is deliberately never read.
        val alert = AlertClassifier.classify(WECHAT, "msg", 0, "周末骑行群")
        assertEquals(AlertFrame.KIND_CHAT, alert?.kind)
        assertEquals("周末骑行群", alert?.name)
    }

    @Test
    fun `wechat's permanent running notice is not news`() {
        // The notification that made an ongoing filter necessary: WeChat keeps
        // one posted for as long as it is alive. Without this the head unit
        // would banner it on every update, forever.
        val alert = AlertClassifier.classify(
            WECHAT, null, AlertClassifier.FLAG_ONGOING_EVENT, "WeChat is running"
        )
        assertNull(alert)
    }

    @Test
    fun `a group summary is dropped so messages are not doubled`() {
        // Posted alongside the individual notifications it summarises, so
        // honouring both means two banners per message.
        val alert = AlertClassifier.classify(
            WECHAT, "msg", AlertClassifier.FLAG_GROUP_SUMMARY, "3 new messages"
        )
        assertNull(alert)
    }

    @Test
    fun `an incoming call survives the ongoing filter`() {
        // The exception that the whole ongoing rule has to carve out. A dialer
        // marks a ringing call ongoing because it is live rather than past --
        // so the obvious filter silences the one alert worth stopping for.
        val alert = AlertClassifier.classify(
            DIALER, "call", AlertClassifier.FLAG_ONGOING_EVENT, "Mum"
        )
        assertEquals(AlertFrame.KIND_CALL, alert?.kind)
        assertEquals("Mum", alert?.name)
    }

    @Test
    fun `a call is recognised by category even from an unknown dialer`() {
        // Vendor dialers are not all in the package list and never will be.
        val alert = AlertClassifier.classify(
            "com.example.someoem.phone", "call", AlertClassifier.FLAG_ONGOING_EVENT, "Alex"
        )
        assertEquals(AlertFrame.KIND_CALL, alert?.kind)
    }

    @Test
    fun `a call is recognised by package even with no category`() {
        val alert = AlertClassifier.classify(DIALER, null, 0, "Alex")
        assertEquals(AlertFrame.KIND_CALL, alert?.kind)
    }

    @Test
    fun `a call from an unknown number has no name and is still a call`() {
        val alert = AlertClassifier.classify(DIALER, "call", 0, null)
        assertNotNull(alert)
        assertEquals(AlertFrame.KIND_CALL, alert?.kind)
        assertEquals("", alert?.name)
    }

    @Test
    fun `a text message is its own kind`() {
        val alert = AlertClassifier.classify(MESSAGES, "msg", 0, "Alex Whitfield")
        assertEquals(AlertFrame.KIND_SMS, alert?.kind)
        assertEquals("Alex Whitfield", alert?.name)
    }

    @Test
    fun `maps navigation is not an alert`() {
        // The listener's original and still primary job. It must not also
        // banner every turn.
        assertNull(AlertClassifier.classify(MAPS, null, AlertClassifier.FLAG_ONGOING_EVENT, "Turn right"))
        assertNull(AlertClassifier.classify(MAPS, null, 0, "Turn right"))
    }

    @Test
    fun `an app nobody asked for stays silent`() {
        // Email, Slack, and every shop that files its promotions under
        // CATEGORY_MESSAGE. The allowlist is what keeps the panel quiet.
        assertNull(AlertClassifier.classify("com.example.shop", "msg", 0, "50% off!"))
        assertNull(AlertClassifier.classify("com.android.email", "msg", 0, "Invoice"))
        assertNull(AlertClassifier.classify("com.slack", "msg", 0, "#general"))
    }

    @Test
    fun `whitespace around a name is not sent`() {
        // It would spend bytes of a 48-byte budget and push the real name
        // off-centre in the banner.
        val alert = AlertClassifier.classify(WECHAT, "msg", 0, "  Alex  ")
        assertEquals("Alex", alert?.name)
    }

    @Test
    fun `a summary flag beats everything, including a call`() {
        // Checked before the call exception on purpose: a summary is a
        // duplicate whatever it summarises, so two missed-call notifications
        // must not produce three banners.
        assertNull(
            AlertClassifier.classify(
                DIALER, "call", AlertClassifier.FLAG_GROUP_SUMMARY, "2 missed calls"
            )
        )
    }
}
