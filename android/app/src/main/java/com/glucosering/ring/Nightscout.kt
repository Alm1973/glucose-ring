package com.glucosering.ring

import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/** One Nightscout sgv entry, reduced to the three fields the ring takes. */
data class Entry(val sgv: Int, val direction: String, val dateMs: Long)

/** Outcome of one poll. Pure data, so the relay rules are testable without a network. */
sealed class Fetch {
    /** HTTP 200 with a well-formed body. [entry] is null when Nightscout has no sgv entries. */
    data class Ok(val entry: Entry?) : Fetch()
    /** Any status other than 200, e.g. 401/403 for a wrong or revoked token. */
    data class Http(val code: Int) : Fetch()
    /** No HTTP response at all. [why] is fixed, token-free text. */
    data class Unreachable(val why: String) : Fetch()
    /** HTTP 200, but not the body Nightscout sends. */
    data class BadBody(val why: String) : Fetch()
}

/**
 * Parser for `GET /api/v1/entries/sgv.json?count=1`. No Android imports beyond
 * org.json (framework on the phone, the org.json artifact in JVM tests).
 */
object Nightscout {
    const val PATH = "/api/v1/entries/sgv.json"

    /**
     * Strict on every field that reaches the ring: `sgv` and `date` must be
     * whole JSON numbers and `date` must be epoch milliseconds. A string
     * "204", a fractional sgv or a seconds timestamp is refused, never
     * coerced -- a reinterpreted value is a fabricated reading.
     */
    fun parse(body: String): Fetch {
        val arr = try {
            JSONArray(body)
        } catch (e: JSONException) {
            return Fetch.BadBody("not a JSON list")
        }
        if (arr.length() == 0) return Fetch.Ok(null)
        val o = arr.optJSONObject(0) ?: return Fetch.BadBody("entry is not an object")

        val type = o.opt("type")
        if (type != null && type != JSONObject.NULL && type != "sgv") return Fetch.BadBody("entry is not an sgv")

        val sgv = wholeNumber(o.opt("sgv")) ?: return Fetch.BadBody("sgv missing or not a whole number")
        if (sgv < Int.MIN_VALUE || sgv > Int.MAX_VALUE) return Fetch.BadBody("sgv out of range")

        val date = wholeNumber(o.opt("date")) ?: return Fetch.BadBody("date missing or not a whole number")
        if (date < RingProtocol.EPOCH_MS_MIN) return Fetch.BadBody("date is not epoch milliseconds")

        // Nightscout allows direction to be absent; the ring accepts an empty one.
        val direction = o.opt("direction") as? String ?: ""
        return Fetch.Ok(Entry(sgv.toInt(), direction, date))
    }

    /** A JSON number with no fractional part, else null. 204.0 is 204; "204" and 204.5 are not. */
    private fun wholeNumber(v: Any?): Long? {
        if (v !is Number) return null
        val d = v.toDouble()
        if (d.isNaN() || d.isInfinite() || d != Math.floor(d) || Math.abs(d) > 9.0e15) return null
        return d.toLong()
    }
}
