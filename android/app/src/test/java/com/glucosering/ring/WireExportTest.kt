package com.glucosering.ring

import org.junit.Assert.assertNotNull
import org.junit.Test
import java.io.File

/**
 * Writes the formatter's exact output for a sweep of inputs to
 * app/build/wire/, so tests/run_wire_crosscheck.sh can feed the bytes through
 * the firmware's real rp_parse(). This test itself only checks that every
 * input formats.
 *
 *   lines.bin     every wire line concatenated, exactly as it would be written
 *   expected.tsv  sgv<TAB>direction<TAB>epoch_ms the ring should decode, one per line
 */
class WireExportTest {

    @Test fun exportsWireLines() {
        val directions = listOf(
            "", "NONE", "DoubleUp", "SingleUp", "FortyFiveUp", "Flat", "FortyFiveDown",
            "SingleDown", "DoubleDown", "NOT COMPUTABLE", "RATE OUT OF RANGE",
            "Fla,t", "x".repeat(40), "tab\there", "ünïcode", "cr\rlf\n",
        )
        val epochs = listOf(RingProtocol.EPOCH_MS_MIN, 1_789_184_103_083L, Long.MAX_VALUE)
        val dir = File("build/wire").apply { mkdirs() }
        val bytes = StringBuilder()
        val expected = StringBuilder()
        for (epoch in epochs) for (d in directions) for (sgv in RingProtocol.SGV_MIN..RingProtocol.SGV_MAX) {
            val line = RingProtocol.formatReading(sgv.toDouble(), epoch, d)
            assertNotNull("$sgv/$d/$epoch", line)
            bytes.append(line)
            expected.append("$sgv\t${RingProtocol.sanitizeDirection(d)}\t$epoch\n")
        }
        File(dir, "lines.bin").writeBytes(bytes.toString().toByteArray(Charsets.US_ASCII))
        File(dir, "expected.tsv").writeText(expected.toString(), Charsets.US_ASCII)
    }
}
