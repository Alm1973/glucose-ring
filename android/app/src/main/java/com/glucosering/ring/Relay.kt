package com.glucosering.ring

/**
 * Pure relay rules: what to send to the ring, and what to tell the user.
 *
 * No Android imports. The staleness rules are the safety-critical part of this
 * app and are unit-tested on the JVM, the way reading_parser.cpp is tested on
 * the host.
 *
 * The dangerous failure is not a crash; it is looking healthy while relaying
 * nothing. So "Relaying" is only ever shown for a fresh reading that the ring
 * itself has confirmed. Every other state says NOT relaying, and why.
 */

/** The phone-to-ring link, in the terms the user needs. */
enum class RingLink(val short: String, val sentence: String) {
    NO_PERMISSION("no Bluetooth permission", "Bluetooth permission was refused, so the ring can't be reached. Open the app to grant it."),
    BLUETOOTH_OFF("Bluetooth is off", "Bluetooth is off, so the ring is not connected."),
    NO_DEVICE("no ring chosen", "No ring chosen yet. Open the app and tap Find ring."),
    DISCONNECTED("ring not connected", "Ring not connected. Retrying."),
    CONNECTING("ring not connected", "Ring not connected. Connecting."),
    PAIRING("ring not paired", "Ring found, waiting for pairing to finish."),
    HANDSHAKE("ring not answering", "Ring connected, waiting for it to answer."),
    CONFIRMED("", "Ring connected."),
}

class Relay(private val configProblem: String?) {

    companion object {
        /** The ring's default stale window (`!GET STALE` -> 15). Older than this is not current. */
        const val STALE_MS = 15 * 60_000L
        /** Clock disagreement tolerated before a reading counts as dated in the future. */
        const val FUTURE_TOLERANCE_MS = 2 * 60_000L
        /** Polls are every 60 s; this long without one means polling itself has stalled. */
        const val POLL_OVERDUE_MS = 5 * 60_000L
        const val TOKEN_PLACEHOLDER = "PASTE_READ_ONLY_TOKEN_HERE"

        /** Why the build's Nightscout settings can't be used, or null if they can. */
        fun configProblem(url: String, token: String): String? = when {
            token.isBlank() || token == TOKEN_PLACEHOLDER ->
                "The Nightscout token is not set (nightscout.token in android/local.properties)."
            !url.startsWith("https://") ->
                "The Nightscout URL is missing or not https (nightscout.url in android/local.properties)."
            else -> null
        }
    }

    data class Status(val relaying: Boolean, val title: String, val text: String)

    /** Newest reading fetched, valid or not. Never synthesised or interpolated. */
    var latest: Entry? = null
        private set
    private var lastFetch: Fetch? = null
    private var lastFetchAt = 0L
    /** `date` of the newest reading written on the current ring link. */
    private var sentOnLink = 0L
    /** The ring's own `#STATUS` since our last write, or null. */
    private var ringStatus: RingProtocol.Parsed? = null
    private var ringHadWatchdogReset = false

    fun onFetch(f: Fetch, now: Long) {
        lastFetch = f
        lastFetchAt = now
        val e = (f as? Fetch.Ok)?.entry ?: return
        // Never keep a future-dated reading: the ring accepts only strictly
        // increasing timestamps, so sending one would lock out every real
        // reading until the clock caught up.
        if (e.dateMs > now + FUTURE_TOLERANCE_MS) return
        if (e.dateMs > (latest?.dateMs ?: 0L)) latest = e
    }

    fun isFresh(e: Entry, now: Long) = now - e.dateMs <= STALE_MS

    /**
     * The wire line to write now, or null. A non-null result is recorded as
     * sent on this link, so each reading is written at most once per link.
     *
     * Only a fresh reading is ever returned. The ring cannot always tell an
     * old reading from a new one on its own (PHONE_APP_REPORT.md,
     * discrepancy 6), so this check is what keeps stale data off the ring.
     */
    fun takeLine(now: Long, link: RingLink): String? {
        if (link != RingLink.CONFIRMED) return null
        val e = latest ?: return null
        if (!isFresh(e, now) || e.dateMs <= sentOnLink) return null
        val line = RingProtocol.formatReading(e.sgv.toDouble(), e.dateMs, e.direction) ?: return null
        sentOnLink = e.dateMs
        ringStatus = null
        return line
    }

    /**
     * A link was confirmed (#BOOT or #VER). The ring may have rebooted and
     * lost the reading, so the current one may be written once more. The ring
     * drops a duplicate without refreshing its staleness, so this cannot make
     * old data look new.
     */
    fun onLinkConfirmed() {
        sentOnLink = 0L
        ringStatus = null
    }

    fun onRingLine(p: RingProtocol.Parsed) {
        when (p.kind) {
            "STATUS" -> ringStatus = p
            "BOOT" -> ringHadWatchdogReset = p.fields["wdt_reset"] == "1"
        }
    }

    fun status(now: Long, link: RingLink): Status {
        val problem = fetchProblem(now)
        val e = latest
        val fresh = e != null && isFresh(e, now)
        val valid = e != null && RingProtocol.formatReading(e.sgv.toDouble(), e.dateMs, e.direction) != null
        val current = if (configProblem == null && fresh && valid) e else null
        val confirmed = current?.let { ringConfirms(it) }

        val text = ArrayList<String>()
        if (problem != null) text += problem.second
        if (e == null) {
            if (problem == null) text += "No reading received yet."
        } else if (!isFresh(e, now)) {
            text += "Newest reading (${e.sgv}) is ${age(now - e.dateMs)} old, too old to relay." +
                if (problem == null) " New CGM data is not reaching Nightscout." else ""
        } else if (!valid) {
            text += "Newest value from Nightscout (${e.sgv}) is not a usable glucose reading."
        } else {
            text += "Reading ${label(e)}, ${age(now - e.dateMs)} old."
        }
        text += ringSentence(link, current, confirmed)
        if (ringHadWatchdogReset) text += "The ring restarted itself after a firmware hang (watchdog reset)."

        val title = if (current == null) {
            "NOT relaying: " + when {
                problem != null -> problem.first
                e == null -> "no reading yet"
                !fresh -> "no current glucose data"
                else -> "not a usable reading"
            }
        } else when {
            link != RingLink.CONFIRMED -> "NOT relaying: ${link.short}"
            confirmed == null -> "Sending ${label(current)} to the ring"
            confirmed == false -> "NOT relaying: ring did not take the reading"
            problem != null -> "Relaying ${label(current)}, but ${problem.first}"
            else -> "Relaying ${label(current)}"
        }
        val relaying = current != null && link == RingLink.CONFIRMED && confirmed == true
        return Status(relaying, title, text.joinToString(" "))
    }

    /** (short, sentence) for why the last poll gave no usable news, or null if it was fine. */
    private fun fetchProblem(now: Long): Pair<String, String>? {
        configProblem?.let { return "not configured" to "$it Nothing can be fetched." }
        val f = lastFetch ?: return "starting" to "Checking Nightscout."
        if (now - lastFetchAt > POLL_OVERDUE_MS)
            return "Nightscout not checked" to "Nightscout has not been checked for ${age(now - lastFetchAt)}."
        return when (f) {
            is Fetch.Http ->
                if (f.code == 401 || f.code == 403)
                    "token rejected" to "Nightscout rejected the token (HTTP ${f.code}). It is wrong or has been revoked."
                else "Nightscout error" to "Nightscout returned an error (HTTP ${f.code})."
            is Fetch.Unreachable -> "can't reach Nightscout" to "Can't reach Nightscout (${f.why})."
            is Fetch.BadBody -> "unexpected reply" to "Nightscout's reply was not understood (${f.why})."
            is Fetch.Ok -> when {
                f.entry == null -> "no readings in Nightscout" to "Nightscout returned no glucose readings."
                f.entry.dateMs > lastFetchAt + FUTURE_TOLERANCE_MS ->
                    "reading dated in the future" to "Nightscout's newest reading is dated in the future. Check the phone's date and time."
                else -> null
            }
        }
    }

    private fun ringSentence(link: RingLink, current: Entry?, confirmed: Boolean?): String {
        if (link != RingLink.CONFIRMED || current == null) return link.sentence
        return when (confirmed) {
            null -> "Ring connected; waiting for it to confirm the reading."
            true -> "Ring connected and confirms ${current.sgv}."
            false -> "Ring connected but reports sgv=${ringStatus?.fields?.get("sgv")} " +
                "stale=${ringStatus?.fields?.get("stale")}, so the reading did not take."
        }
    }

    /** Null until the ring answers `!GET STATUS`; then whether it holds this reading as current. */
    private fun ringConfirms(e: Entry): Boolean? {
        val s = ringStatus ?: return null
        return s.fields["sgv"] == e.sgv.toString() && s.fields["stale"] == "0"
    }

    private fun label(e: Entry) =
        listOf(e.sgv.toString(), RingProtocol.sanitizeDirection(e.direction)).filter { it.isNotEmpty() }.joinToString(" ")

    private fun age(ms: Long): String {
        val m = ms / 60_000L
        return if (m < 1) "under a minute" else "$m min"
    }
}

/** Reconnect delay: 1 s, 2 s, 4 s ... capped at 60 s, with jitter (PHONE_APP.md section 7). */
class Backoff(private val random: () -> Double = { Math.random() }) {
    companion object {
        const val START_MS = 1_000L
        const val MAX_MS = 60_000L
    }

    private var attempt = 0

    /** 50-100 % of the current step, so many phones never retry in lockstep. */
    fun next(): Long {
        val base = minOf(MAX_MS, START_MS shl minOf(attempt, 6))
        attempt++
        return (base * (0.5 + 0.5 * random())).toLong()
    }

    fun reset() {
        attempt = 0
    }
}
