package com.glucosering.ring

import org.junit.Assert.*
import org.junit.Test

/**
 * JVM unit tests. Vectors are lifted from PROTOCOL.md section 8 -- the same
 * table the firmware's host tests assert, so both ends of the link are checked
 * against one specification rather than two readings of it.
 */
class RingProtocolTest {

    private val T = 1_789_238_400_000L

    // ---- accepted ----
    @Test fun formatsATypicalReading() {
        assertEquals("112,Flat,$T\n", RingProtocol.formatReading(112.0, T, "Flat"))
    }

    @Test fun roundsToNearestInteger() {
        assertEquals("113,Flat,$T\n", RingProtocol.formatReading(112.6, T, "Flat"))
        assertEquals("112,Flat,$T\n", RingProtocol.formatReading(112.4, T, "Flat"))
    }

    @Test fun acceptsBoundaryValues() {
        assertNotNull(RingProtocol.formatReading(20.0, RingProtocol.EPOCH_MS_MIN, "Flat"))
        assertNotNull(RingProtocol.formatReading(600.0, RingProtocol.EPOCH_MS_MIN, "Flat"))
    }

    @Test fun allowsEmptyDirection() {
        assertEquals("112,,$T\n", RingProtocol.formatReading(112.0, T, null))
        assertEquals("112,,$T\n", RingProtocol.formatReading(112.0, T, ""))
    }

    @Test fun keepsSpacesInDirection() {
        assertEquals(
            "95,RATE OUT OF RANGE,$T\n",
            RingProtocol.formatReading(95.0, T, "RATE OUT OF RANGE")
        )
    }

    // ---- rejected ----
    @Test fun rejectsOutOfRangeGlucose() {
        assertNull(RingProtocol.formatReading(19.0, T, "Flat"))
        assertNull(RingProtocol.formatReading(601.0, T, "Flat"))
        assertNull(RingProtocol.formatReading(0.0, T, "Flat"))
        assertNull(RingProtocol.formatReading(-5.0, T, "Flat"))
    }

    @Test fun rejectsBadTimestamps() {
        assertNull(RingProtocol.formatReading(112.0, 0L, "Flat"))
        assertNull(RingProtocol.formatReading(112.0, RingProtocol.EPOCH_MS_MIN - 1, "Flat"))
        // seconds where the protocol requires milliseconds
        assertNull(RingProtocol.formatReading(112.0, 1_789_238_400L, "Flat"))
    }

    @Test fun rejectsNonFiniteGlucose() {
        assertNull(RingProtocol.formatReading(Double.NaN, T, "Flat"))
        assertNull(RingProtocol.formatReading(Double.POSITIVE_INFINITY, T, "Flat"))
    }

    // ---- direction sanitising: the silent-data-loss trap ----
    @Test fun stripsCommasFromDirection() {
        // A comma would create a fourth field and the ring would drop the whole
        // reading silently, without buzzing.
        val line = RingProtocol.formatReading(112.0, T, "Fla,t")!!
        assertEquals(2, line.count { it == ',' })
        assertEquals("112,Flat,$T\n", line)
    }

    @Test fun stripsControlAndHighBytes() {
        assertEquals("Flat", RingProtocol.sanitizeDirection("Fla\u0009t"))  // tab
        assertEquals("Flat", RingProtocol.sanitizeDirection("Fla\u0001t"))  // control byte
        assertEquals("Flat", RingProtocol.sanitizeDirection("Fla\u00FFt"))  // high byte
        assertEquals("Flat", RingProtocol.sanitizeDirection("Fla\u007Ft"))  // DEL
        assertEquals("Flat", RingProtocol.sanitizeDirection("Fla\nt"))      // newline
    }

    @Test fun trimsOverlongDirection() {
        val long = "x".repeat(40)
        assertEquals(RingProtocol.DIRECTION_MAX, RingProtocol.sanitizeDirection(long).length)
    }

    // Duplicate suppression moved to Relay; see RelayTest.dedupesOnDate.

    // ---- response parsing ----
    @Test fun parsesStatus() {
        val p = RingProtocol.parseRingLine(
            "#STATUS sgv=112 dir=Flat age_s=42 stale=0 alert=none up_s=3600"
        )!!
        assertEquals("STATUS", p.kind)
        assertEquals("112", p.fields["sgv"])
        assertEquals("Flat", p.fields["dir"])
        assertEquals("none", p.fields["alert"])
    }

    @Test fun parsesBootWithWatchdogFlag() {
        val p = RingProtocol.parseRingLine("#BOOT ver=2.0.0 proto=2 wdt_reset=1 up_s=3")!!
        assertEquals("BOOT", p.kind)
        assertEquals("1", p.fields["wdt_reset"])
    }

    @Test fun parsesErrorAndBareTokens() {
        val p = RingProtocol.parseRingLine("#ERR SET low must be below high")!!
        assertEquals("ERR", p.kind)
        assertTrue(p.fields.containsKey("SET"))
    }

    @Test fun parsesStaleGiveup() {
        val p = RingProtocol.parseRingLine("#ALERT stale_giveup")!!
        assertEquals("ALERT", p.kind)
        assertTrue(p.fields.containsKey("stale_giveup"))
    }

    @Test fun rejectsNonRingLines() {
        assertNull(RingProtocol.parseRingLine("112,Flat,$T"))
        assertNull(RingProtocol.parseRingLine(""))
        assertNull(RingProtocol.parseRingLine("#"))
    }

    // ---- chunked reassembly: the normal case at the default 23-byte MTU ----
    @Test fun reassemblesLineSplitAcrossChunks() {
        val a = RingProtocol.LineAssembler()
        assertTrue(a.feed("#STATUS sgv=1").isEmpty())
        assertTrue(a.feed("12 dir=Flat").isEmpty())
        val out = a.feed(" alert=none\n")
        assertEquals(1, out.size)
        assertEquals("#STATUS sgv=112 dir=Flat alert=none", out[0])
    }

    @Test fun splitsSeveralLinesInOneChunk() {
        val a = RingProtocol.LineAssembler()
        assertEquals(listOf("#VER x", "#OK SAVE"), a.feed("#VER x\n#OK SAVE\n"))
    }

    @Test fun toleratesCrlf() {
        val a = RingProtocol.LineAssembler()
        assertEquals(listOf("#OK SAVE"), a.feed("#OK SAVE\r\n"))
    }

    @Test fun discardsUnboundedGarbage() {
        val a = RingProtocol.LineAssembler(limit = 64)
        a.feed("x".repeat(200))            // a newline that never arrives
        assertEquals(listOf("#VER ok"), a.feed("#VER ok\n"))
    }

    @Test fun commandBuildersMatchProtocol() {
        assertEquals("!SET THRESH 70 200\n", RingProtocol.cmdSetThresholds(70, 200))
        assertEquals("!SET THRESH unset unset\n", RingProtocol.cmdSetThresholds(null, null))
        assertEquals("!BUZZ low\n", RingProtocol.cmdBuzz("low"))
    }
}
