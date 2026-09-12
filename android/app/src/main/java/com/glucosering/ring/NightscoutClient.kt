package com.glucosering.ring

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import java.net.URLEncoder
import java.net.UnknownHostException
import javax.net.ssl.SSLException

/**
 * Blocking GET of the newest sgv entry. Call off the main thread. Read-only:
 * this app never writes to Nightscout.
 *
 * The token travels as `?token=`, the form verified against this server
 * (PHONE_APP_REPORT.md). It must never reach a log. Nothing here logs, and
 * exceptions -- whose messages can carry the full URL, e.g.
 * FileNotFoundException on a 4xx -- are reduced to fixed, token-free text
 * before they leave this class.
 */
class NightscoutClient(private val baseUrl: String, private val token: String) {

    companion object {
        private const val TIMEOUT_MS = 15_000
        private const val MAX_BODY = 64 * 1024   // count=1 is ~300 bytes
    }

    fun fetch(): Fetch {
        var conn: HttpURLConnection? = null
        return try {
            val url = URL(
                baseUrl.trimEnd('/') + Nightscout.PATH + "?count=1&token=" + URLEncoder.encode(token, "UTF-8")
            )
            conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = TIMEOUT_MS
            conn.readTimeout = TIMEOUT_MS
            conn.useCaches = false
            conn.setRequestProperty("Accept", "application/json")
            val code = conn.responseCode
            if (code != HttpURLConnection.HTTP_OK) Fetch.Http(code)
            else readCapped(conn)?.let { Nightscout.parse(it) } ?: Fetch.BadBody("reply too large")
        } catch (e: SocketTimeoutException) {
            Fetch.Unreachable("timed out")
        } catch (e: UnknownHostException) {
            Fetch.Unreachable("no network, or the server name did not resolve")
        } catch (e: SSLException) {
            Fetch.Unreachable("secure connection failed")
        } catch (e: IOException) {
            Fetch.Unreachable("network error")
        } catch (e: RuntimeException) {
            Fetch.Unreachable("request failed")
        } finally {
            conn?.disconnect()
        }
    }

    private fun readCapped(conn: HttpURLConnection): String? {
        val out = ByteArrayOutputStream()
        conn.inputStream.use { ins ->
            val buf = ByteArray(4096)
            while (true) {
                val n = ins.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
                if (out.size() > MAX_BODY) return null
            }
        }
        return out.toString("UTF-8")
    }
}
