package com.glucosering.ring

/**
 * Pure protocol logic -- no Android imports, so it is unit-testable on the JVM.
 * Mirrors PROTOCOL.md; the vectors in RingProtocolTest come straight from
 * PROTOCOL.md section 8, which are the same vectors the firmware's own host
 * tests assert.
 */
object RingProtocol {

    const val SGV_MIN = 20
    const val SGV_MAX = 600
    const val EPOCH_MS_MIN = 1_600_000_000_000L
    const val DIRECTION_MAX = 23

    /**
     * Format one reading for the wire, or null if it must not be sent.
     *
     * Returning null rather than a best-effort string is deliberate: the ring
     * drops malformed lines silently and without buzzing, so a formatting bug
     * would be indistinguishable from a dead link. Better to not send.
     */
    fun formatReading(mgdl: Double, epochMs: Long, slopeName: String?): String? {
        if (mgdl.isNaN() || mgdl.isInfinite()) return null
        val sgv = Math.round(mgdl).toInt()
        if (sgv < SGV_MIN || sgv > SGV_MAX) return null
        if (epochMs < EPOCH_MS_MIN) return null
        return "$sgv,${sanitizeDirection(slopeName)},$epochMs\n"
    }

    /**
     * A comma here would create a fourth field and the ring would reject the
     * whole reading. Non-printable bytes are rejected by the ring's parser
     * too, and 24+ characters is an error rather than a truncation -- so trim
     * to 23 here rather than letting the ring throw the reading away.
     */
    fun sanitizeDirection(raw: String?): String {
        if (raw == null) return ""
        return raw.filter { it.code in 0x20..0x7E && it != ',' }.take(DIRECTION_MAX)
    }

    /** Ring -> phone lines all start with '#'. */
    fun isRingMessage(line: String): Boolean = line.startsWith("#")

    data class Parsed(val kind: String, val fields: Map<String, String>, val raw: String)

    /**
     * Split "#STATUS sgv=112 dir=Flat age_s=42" into kind + key/value map.
     * Bare tokens (no '=') are kept under their own name with an empty value,
     * so "#OK SET THRESH" yields kind=OK with "SET" and "THRESH" present.
     */
    fun parseRingLine(line: String): Parsed? {
        if (!isRingMessage(line)) return null
        val body = line.substring(1).trim()
        if (body.isEmpty()) return null
        val parts = body.split(" ").filter { it.isNotEmpty() }
        if (parts.isEmpty()) return null
        val fields = LinkedHashMap<String, String>()
        for (p in parts.drop(1)) {
            val i = p.indexOf('=')
            if (i > 0) fields[p.substring(0, i)] = p.substring(i + 1) else fields[p] = ""
        }
        return Parsed(parts[0], fields, line)
    }

    // ---- command builders ----
    fun cmdVersion() = "!VER\n"
    fun cmdGetThresholds() = "!GET THRESH\n"
    fun cmdGetStatus() = "!GET STATUS\n"
    fun cmdGetBattery() = "!GET BATT\n"
    fun cmdSetThresholds(low: Int?, high: Int?) =
        "!SET THRESH ${low?.toString() ?: "unset"} ${high?.toString() ?: "unset"}\n"
    fun cmdBuzz(pattern: String) = "!BUZZ $pattern\n"
    fun cmdSave() = "!SAVE\n"

    /**
     * Reassembles newline-delimited lines from arbitrarily chunked notification
     * payloads. The ring's replies arrive in MTU-sized pieces, so a line can
     * span several notifications.
     */
    class LineAssembler(private val limit: Int = 512) {
        private val buf = StringBuilder()

        fun feed(chunk: String): List<String> {
            val out = ArrayList<String>()
            buf.append(chunk)
            while (true) {
                val i = buf.indexOf("\n")
                if (i < 0) break
                val line = buf.substring(0, i).trimEnd('\r')
                buf.delete(0, i + 1)
                if (line.isNotEmpty()) out.add(line)
            }
            // A wedged link must not grow this without bound.
            if (buf.length > limit) buf.setLength(0)
            return out
        }
    }
}
