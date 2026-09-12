package com.glucosering.ring

import org.junit.Assert.*
import org.junit.Test

/**
 * Nightscout body parsing. Like the firmware's parser tests: the real body,
 * wrong types, missing fields, hostile input, and every truncation of a good
 * body.
 */
class NightscoutTest {

    /** Verbatim body from the live instance, 2026-09-12 (PHONE_APP_REPORT.md). */
    private val LIVE = """[{"_id":"6aa4c87aa24de4b1501f5b01","type":"sgv","sysTime":"2026-09-12T03:35:03.083Z","date":1789184103083,"dateString":"2026-09-12T03:35:03.083Z","device":"nightscout-connect","direction":"FortyFiveDown","sgv":204,"trend":5,"utcOffset":0,"mills":1789184103083}]"""
    private val D = 1_789_184_103_083L

    private fun entry(body: String): Entry? {
        val f = Nightscout.parse(body)
        assertTrue("expected Ok for $body, got $f", f is Fetch.Ok)
        return (f as Fetch.Ok).entry
    }

    private fun bad(body: String) {
        val f = Nightscout.parse(body)
        assertTrue("expected BadBody for $body, got $f", f is Fetch.BadBody)
    }

    // ---- accepted ----
    @Test fun parsesTheLiveResponse() {
        assertEquals(Entry(204, "FortyFiveDown", D), entry(LIVE))
    }

    @Test fun liveResponseFormatsToTheDocumentedWireLine() {
        val e = entry(LIVE)!!
        assertEquals(
            "204,FortyFiveDown,1789184103083\n",
            RingProtocol.formatReading(e.sgv.toDouble(), e.dateMs, e.direction)
        )
    }

    @Test fun emptyListMeansNoReadings() {
        assertNull(entry("[]"))
    }

    @Test fun missingOrNullDirectionBecomesEmpty() {
        assertEquals("", entry("""[{"sgv":100,"date":$D}]""")!!.direction)
        assertEquals("", entry("""[{"sgv":100,"date":$D,"direction":null}]""")!!.direction)
    }

    @Test fun keepsDirectionsWithSpaces() {
        assertEquals("NOT COMPUTABLE", entry("""[{"sgv":100,"date":$D,"direction":"NOT COMPUTABLE"}]""")!!.direction)
    }

    @Test fun missingTypeIsAccepted() {
        assertEquals(100, entry("""[{"sgv":100,"date":$D}]""")!!.sgv)
    }

    @Test fun acceptsAWholeNumberWrittenAsDecimal() {
        assertEquals(204, entry("""[{"sgv":204.0,"date":$D}]""")!!.sgv)
    }

    @Test fun passesErrorCodesThroughForTheRelayToRefuse() {
        // Uploaders can post sensor-state codes as tiny sgv values. Parsing
        // keeps them; Relay/formatReading refuse to send them (see RelayTest).
        assertEquals(5, entry("""[{"sgv":5,"date":$D}]""")!!.sgv)
        assertNull(RingProtocol.formatReading(5.0, D, ""))
    }

    @Test fun hostileDirectionCannotBreakTheWireLine() {
        val e = entry("""[{"sgv":100,"date":$D,"direction":"Fla,t\n,x"}]""")!!
        val line = RingProtocol.formatReading(e.sgv.toDouble(), e.dateMs, e.direction)!!
        assertEquals(2, line.count { it == ',' })
        assertEquals(1, line.count { it == '\n' })
        assertTrue(line.endsWith("\n"))
    }

    // ---- refused, never coerced ----
    @Test fun rejectsSgvAsString() = bad("""[{"sgv":"204","date":$D}]""")
    @Test fun rejectsFractionalSgv() = bad("""[{"sgv":204.5,"date":$D}]""")
    @Test fun rejectsMissingSgv() = bad("""[{"date":$D}]""")
    @Test fun rejectsNullSgv() = bad("""[{"sgv":null,"date":$D}]""")
    @Test fun rejectsBooleanSgv() = bad("""[{"sgv":true,"date":$D}]""")
    @Test fun rejectsSgvThatOverflowsInt() = bad("""[{"sgv":4294967500,"date":$D}]""")
    @Test fun rejectsDateInSeconds() = bad("""[{"sgv":204,"date":1789184103}]""")
    @Test fun rejectsDateAsString() = bad("""[{"sgv":204,"date":"2026-09-12T03:35:03.083Z"}]""")
    @Test fun rejectsMissingDate() = bad("""[{"sgv":204}]""")
    @Test fun rejectsFractionalDate() = bad("""[{"sgv":204,"date":1789184103083.5}]""")
    @Test fun rejectsNonSgvEntries() = bad("""[{"type":"mbg","sgv":204,"date":$D}]""")

    @Test fun rejectsBodiesThatAreNotAList() {
        bad("")
        bad("{}")
        bad("null")
        bad("<html>502 Bad Gateway</html>")
        bad("[1]")
        bad("[null]")
        bad("[\"sgv\"]")
    }

    @Test fun everyTruncationOfTheLiveBodyIsRejected() {
        for (n in 0 until LIVE.length) {
            val f = Nightscout.parse(LIVE.substring(0, n))
            assertTrue("prefix of $n chars parsed as $f", f is Fetch.BadBody)
        }
    }
}
