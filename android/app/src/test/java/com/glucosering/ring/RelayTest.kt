package com.glucosering.ring

import org.junit.Assert.*
import org.junit.Test

/**
 * Staleness, dedupe and user-facing status. The invariant under test: the app
 * never sends a reading that is not current, and never says "Relaying" unless
 * a fresh reading is on the ring and the ring has said so.
 */
class RelayTest {

    private val MIN = 60_000L
    private val NOW = 1_789_184_103_083L + MIN
    private val C = RingLink.CONFIRMED

    private fun entry(ageMs: Long, sgv: Int = 204, dir: String = "FortyFiveDown") = Entry(sgv, dir, NOW - ageMs)
    private fun ok(e: Entry?) = Fetch.Ok(e)
    private fun relay(vararg fetches: Fetch) = Relay(null).apply { fetches.forEach { onFetch(it, NOW) } }
    private fun ringSays(r: Relay, line: String) = r.onRingLine(RingProtocol.parseRingLine(line)!!)
    private val RING_HAS_204 = "#STATUS sgv=204 dir=FortyFiveDown age_s=1 stale=0 alert=none up_s=9"
    private val RING_HAS_NOTHING = "#STATUS sgv=none dir= age_s=0 stale=1 alert=stale up_s=42"

    // ---- sending ----
    @Test fun sendsAFreshReadingOnceTheRingHasAnswered() {
        assertEquals("204,FortyFiveDown,${NOW - MIN}\n", relay(ok(entry(MIN))).takeLine(NOW, C))
    }

    @Test fun neverSendsBeforeTheRingHasAnswered() {
        val r = relay(ok(entry(MIN)))
        for (l in RingLink.values().filter { it != C }) assertNull(l.name, r.takeLine(NOW, l))
        assertNotNull("still pending once the ring answers", r.takeLine(NOW, C))
    }

    @Test fun dedupesOnDate() {
        val r = relay(ok(entry(MIN)))
        assertNotNull(r.takeLine(NOW, C))
        r.onFetch(ok(entry(MIN)), NOW + MIN)   // same reading, polled again
        assertNull(r.takeLine(NOW + MIN, C))
    }

    @Test fun sendsTheNextNewerReading() {
        val r = relay(ok(entry(6 * MIN)))
        assertNotNull(r.takeLine(NOW, C))
        r.onFetch(ok(entry(MIN, sgv = 190)), NOW)
        assertEquals("190,FortyFiveDown,${NOW - MIN}\n", r.takeLine(NOW, C))
    }

    @Test fun anOlderReadingNeverReplacesANewerOne() {
        val r = relay(ok(entry(MIN)), ok(entry(6 * MIN)))
        assertEquals(NOW - MIN, r.latest!!.dateMs)
    }

    @Test fun resendsTheCurrentReadingOnceAfterReconnect() {
        val r = relay(ok(entry(MIN)))
        assertNotNull(r.takeLine(NOW, C))
        r.onLinkConfirmed()
        assertNotNull(r.takeLine(NOW, C))
        assertNull(r.takeLine(NOW, C))
    }

    // ---- staleness ----
    @Test fun staleBoundaryIsFifteenMinutes() {
        val e = Entry(204, "Flat", NOW - Relay.STALE_MS)
        assertNotNull(relay(ok(e)).takeLine(NOW, C))
        assertNull(Relay(null).apply { onFetch(ok(e), NOW) }.takeLine(NOW + 1, C))
    }

    @Test fun neverSendsAStaleReading() {
        assertNull(relay(ok(entry(16 * MIN))).takeLine(NOW, C))
    }

    @Test fun aPendingReadingThatGoesStaleWhileDisconnectedIsNeverSent() {
        val r = relay(ok(entry(MIN)))
        assertNull(r.takeLine(NOW, RingLink.DISCONNECTED))
        assertNull(r.takeLine(NOW + 15 * MIN, C))   // ring back 15 min later: reading now 16 min old
    }

    @Test fun lastGoodReadingIsNotResentWhenNightscoutGoesDown() {
        val r = relay(ok(entry(MIN)))
        assertNotNull(r.takeLine(NOW, C))
        r.onFetch(Fetch.Unreachable("timed out"), NOW + MIN)
        assertNull(r.takeLine(NOW + MIN, C))
        r.onLinkConfirmed()                            // even across a reconnect, once it is stale
        r.onFetch(Fetch.Unreachable("timed out"), NOW + 15 * MIN)
        assertNull(r.takeLine(NOW + 15 * MIN, C))
    }

    @Test fun refusesFutureDatedReadings() {
        val r = relay(ok(Entry(204, "Flat", NOW + Relay.FUTURE_TOLERANCE_MS + 1)))
        assertNull(r.latest)
        assertNull(r.takeLine(NOW, C))
        assertEquals("NOT relaying: reading dated in the future", r.status(NOW, C).title)
    }

    @Test fun smallClockSkewIsTolerated() {
        assertNotNull(relay(ok(Entry(204, "Flat", NOW + MIN))).takeLine(NOW, C))
    }

    @Test fun aFutureDatedReadingDoesNotBlockRealOnes() {
        val r = relay(ok(Entry(204, "Flat", NOW + 3_600_000L)), ok(entry(MIN)))
        assertNotNull(r.takeLine(NOW, C))
    }

    @Test fun neverSendsSensorErrorCodes() {
        val r = relay(ok(entry(MIN, sgv = 5)))
        assertNull(r.takeLine(NOW, C))
        assertEquals("NOT relaying: not a usable reading", r.status(NOW, C).title)
    }

    @Test fun anErrorCodeSupersedesAnOlderGoodReading() {
        val r = relay(ok(entry(6 * MIN)), ok(entry(MIN, sgv = 5)))
        assertNull(r.takeLine(NOW, C))
    }

    // ---- what the notification says ----
    @Test fun healthyOnlyOnceTheRingConfirms() {
        val r = relay(ok(entry(MIN)))
        r.takeLine(NOW, C)
        assertEquals("Sending 204 FortyFiveDown to the ring", r.status(NOW, C).title)
        assertFalse(r.status(NOW, C).relaying)
        ringSays(r, RING_HAS_204)
        val s = r.status(NOW, C)
        assertTrue(s.relaying)
        assertEquals("Relaying 204 FortyFiveDown", s.title)
        assertTrue(s.text, s.text.contains("confirms 204"))
    }

    @Test fun ringHoldingSomethingElseIsNotRelaying() {
        val r = relay(ok(entry(MIN)))
        r.takeLine(NOW, C)
        ringSays(r, RING_HAS_NOTHING)
        val s = r.status(NOW, C)
        assertFalse(s.relaying)
        assertEquals("NOT relaying: ring did not take the reading", s.title)
    }

    @Test fun tokenRejectedSaysSo() {
        for (code in listOf(401, 403)) {
            val s = relay(Fetch.Http(code)).status(NOW, C)
            assertFalse(s.relaying)
            assertEquals("NOT relaying: token rejected", s.title)
            assertTrue(s.text, s.text.contains("HTTP $code"))
        }
    }

    @Test fun serverErrorSaysSo() {
        assertEquals("NOT relaying: Nightscout error", relay(Fetch.Http(500)).status(NOW, C).title)
    }

    @Test fun unreachableSaysSo() {
        val s = relay(Fetch.Unreachable("timed out")).status(NOW, C)
        assertEquals("NOT relaying: can't reach Nightscout", s.title)
        assertTrue(s.text, s.text.contains("timed out"))
    }

    @Test fun unreachableWithAnOldReadingSaysBoth() {
        val r = relay(ok(entry(MIN)))
        r.onFetch(Fetch.Unreachable("timed out"), NOW + 20 * MIN)
        val s = r.status(NOW + 20 * MIN, C)
        assertEquals("NOT relaying: can't reach Nightscout", s.title)
        assertTrue(s.text, s.text.contains("21 min old, too old to relay"))
    }

    @Test fun staleDataSaysSoInPlainWords() {
        val s = relay(ok(entry(23 * MIN))).status(NOW, C)
        assertEquals("NOT relaying: no current glucose data", s.title)
        assertTrue(s.text, s.text.contains("23 min old"))
        assertTrue(s.text, s.text.contains("not reaching Nightscout"))
    }

    @Test fun emptyNightscoutSaysSo() {
        assertEquals("NOT relaying: no readings in Nightscout", relay(ok(null)).status(NOW, C).title)
    }

    @Test fun ringProblemsSaySo() {
        val r = relay(ok(entry(MIN)))
        assertEquals("NOT relaying: ring not connected", r.status(NOW, RingLink.DISCONNECTED).title)
        assertEquals("NOT relaying: no Bluetooth permission", r.status(NOW, RingLink.NO_PERMISSION).title)
        assertEquals("NOT relaying: Bluetooth is off", r.status(NOW, RingLink.BLUETOOTH_OFF).title)
        assertEquals("NOT relaying: no ring chosen", r.status(NOW, RingLink.NO_DEVICE).title)
        assertEquals("NOT relaying: ring not paired", r.status(NOW, RingLink.PAIRING).title)
    }

    @Test fun stalledPollingIsReported() {
        val s = relay(ok(entry(MIN))).status(NOW + 6 * MIN, C)
        assertTrue(s.text, s.text.contains("has not been checked for 6 min"))
    }

    @Test fun beforeTheFirstPoll() {
        assertEquals("NOT relaying: starting", Relay(null).status(NOW, C).title)
    }

    @Test fun unconfiguredBuildSaysSoAndNeverSends() {
        val r = Relay(Relay.configProblem("https://x", Relay.TOKEN_PLACEHOLDER))
        r.onFetch(ok(entry(MIN)), NOW)   // cannot happen in the app; the rule holds anyway
        assertEquals("NOT relaying: not configured", r.status(NOW, C).title)
    }

    @Test fun configProblems() {
        assertNotNull(Relay.configProblem("https://x", Relay.TOKEN_PLACEHOLDER))
        assertNotNull(Relay.configProblem("https://x", ""))
        assertNotNull(Relay.configProblem("https://x", "   "))
        assertNotNull(Relay.configProblem("http://x", "tok"))
        assertNotNull(Relay.configProblem("", "tok"))
        assertNull(Relay.configProblem("https://x", "tok"))
    }

    @Test fun watchdogResetIsSurfaced() {
        val r = relay(ok(entry(MIN)))
        ringSays(r, "#BOOT ver=2.0.0 proto=2 wdt_reset=1 up_s=3")
        assertTrue(r.status(NOW, C).text.contains("watchdog"))
        ringSays(r, "#BOOT ver=2.0.0 proto=2 wdt_reset=0 up_s=3")
        assertFalse(r.status(NOW, C).text.contains("watchdog"))
    }

    /** Exhaustive: "Relaying" appears for exactly one combination and no other. */
    @Test fun relayingOnlyForFreshDataConfirmedByTheRing() {
        val fetches = mapOf(
            "fresh" to ok(entry(MIN)),
            "stale" to ok(entry(20 * MIN)),
            "empty" to ok(null),
            "errorcode" to ok(entry(MIN, sgv = 5)),
            "401" to Fetch.Http(401),
            "403" to Fetch.Http(403),
            "500" to Fetch.Http(500),
            "unreachable" to Fetch.Unreachable("timed out"),
            "badbody" to Fetch.BadBody("x"),
        )
        val ringReplies = listOf(null, RING_HAS_204, RING_HAS_NOTHING)
        for ((name, f) in fetches) for (link in RingLink.values()) for (reply in ringReplies) {
            val r = relay(f)
            r.takeLine(NOW, link)
            if (reply != null) ringSays(r, reply)
            val s = r.status(NOW, link)
            val expected = name == "fresh" && link == C && reply == RING_HAS_204
            val where = "$name / $link / $reply -> ${s.title}"
            assertEquals(where, expected, s.relaying)
            assertEquals(where, expected, s.title.startsWith("Relaying"))
        }
    }

    // ---- reconnect backoff ----
    @Test fun backoffDoublesToASixtySecondCap() {
        val b = Backoff { 1.0 }
        assertEquals(listOf(1_000L, 2_000L, 4_000L, 8_000L, 16_000L, 32_000L, 60_000L, 60_000L), List(8) { b.next() })
        b.reset()
        assertEquals(1_000L, b.next())
    }

    @Test fun backoffJitterStaysWithinHalfToFullStep() {
        assertEquals(500L, Backoff { 0.0 }.next())
    }
}
