package org.amnezia.vpn

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import org.amnezia.vpn.util.Log
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

private const val TAG = "ManagedConfigRefresher"
private const val SERVICE_INFO_KEY = "homevpn_managed"
private const val AWG_CONFIG_DATA_KEY = "awg_config_data"
private const val REQUEST_TIMEOUT_MS = 10_000
private val AWG_PARAMETERS = listOf(
    "Jc", "Jmin", "Jmax", "S1", "S2", "S3", "S4", "H1", "H2", "H3", "H4", "I1", "I2", "I3", "I4", "I5",
    "HeaderProtectionKey", "ContentPaddingAddition", "RekeyAfterTime", "RekeyTimeout", "RejectAfterTime",
    "KeepaliveTimeout", "MaxHandshakeAttempts", "RandomTrailers", "DisableCookies"
)
private val IPV4_ADDRESS = Regex("""(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}""")

private sealed interface BackendReply {
    object Unchanged : BackendReply
    object Failed : BackendReply
    class Fresh(val body: String) : BackendReply
}

object ManagedConfigRefresher {

    fun fresh(savedConfig: String, isAfterFailure: Boolean = false): String? {
        val config = try {
            JSONObject(savedConfig)
        } catch (e: JSONException) {
            null
        } ?: return null
        val info = config.optJSONObject(SERVICE_INFO_KEY) ?: return null
        val urls = info.optJSONArray("urls") ?: return null
        val failed = if (isAfterFailure) endpointOf(config) else null
        for (index in 0 until urls.length()) {
            val url = urls.optString(index)
            if (!url.startsWith("https://")) continue
            when (val reply = request(withFailedEndpoint(url, failed), info)) {
                BackendReply.Unchanged -> return null
                BackendReply.Failed -> continue
                is BackendReply.Fresh -> return patchedConfig(savedConfig, reply.body)
            }
        }
        return null
    }

    fun patchedConfig(savedConfig: String, reply: String): String? = try {
        val config = JSONObject(savedConfig)
        val info = config.getJSONObject(SERVICE_INFO_KEY)
        val answer = JSONObject(reply)
        val mainId = answer.getString("main")
        val servers = answer.getJSONArray("servers").let { list -> (0 until list.length()).map(list::getJSONObject) }
        val wantedId = if (mainId != info.optString("main")) mainId else info.optString("server")
        val server = servers.firstOrNull { it.optString("id") == wantedId }
            ?: servers.firstOrNull { it.optString("id") == mainId }
        val isSameServer = server?.optString("id") == info.optString("server")
        if (server == null || (isSameServer && server.optString("revision") == info.optString("revision"))) {
            null
        } else {
            applyConf(config, server.getString("config"))
            if (!isSameServer && server.optString("name").isNotEmpty()) config.put("description", server.optString("name"))
            info.put("server", server.optString("id"))
                .put("main", mainId)
                .put("revision", server.optString("revision"))
                .put("list", answer.optString("revision"))
            config.toString()
        }
    } catch (e: JSONException) {
        Log.w(TAG, "The backend answer is not the expected JSON")
        null
    } catch (e: IllegalArgumentException) {
        Log.w(TAG, "The backend config does not fit: ${e.message}")
        null
    }

    private fun endpointOf(config: JSONObject): String? {
        val data = config.optJSONObject(AWG_CONFIG_DATA_KEY) ?: return null
        val host = data.optString("hostName").ifEmpty { config.optString("hostName") }
        val port = data.optString("port")
        if (host.isEmpty() || port.isEmpty()) return null
        return if (host.contains(':')) "[$host]:$port" else "$host:$port"
    }

    private fun withFailedEndpoint(url: String, failed: String?): String {
        if (failed == null) return url
        return url + (if (url.contains('?')) "&" else "?") + "failed=" + URLEncoder.encode(failed, "UTF-8")
    }

    private fun request(url: String, info: JSONObject): BackendReply {
        var connection: HttpURLConnection? = null
        return try {
            connection = URL(url).openConnection() as HttpURLConnection
            connection.connectTimeout = REQUEST_TIMEOUT_MS
            connection.readTimeout = REQUEST_TIMEOUT_MS
            connection.setRequestProperty("Authorization", "Bearer ${info.optString("key")}")
            info.optString("list").takeIf { it.isNotEmpty() }?.let { connection.setRequestProperty("If-None-Match", "\"$it\"") }
            when (connection.responseCode) {
                HttpURLConnection.HTTP_OK -> BackendReply.Fresh(connection.inputStream.bufferedReader().use { it.readText() })
                HttpURLConnection.HTTP_NOT_MODIFIED -> BackendReply.Unchanged
                else -> BackendReply.Failed
            }
        } catch (e: IOException) {
            Log.w(TAG, "The backend did not answer: ${e.message}")
            BackendReply.Failed
        } finally {
            connection?.disconnect()
        }
    }

    private fun applyConf(config: JSONObject, text: String) {
        val sections = sectionsOf(text)
        val face = requireNotNull(sections["Interface"]) { "no [Interface]" }
        val peer = requireNotNull(sections["Peer"]) { "no [Peer]" }
        val (host, port) = requireNotNull(endpointOf(peer["Endpoint"].orEmpty())) { "no Endpoint" }
        val data = config.getJSONObject(AWG_CONFIG_DATA_KEY)
        data.put("config", text)
        data.put("hostName", host)
        data.put("port", port)
        data.put("client_ip", requireNotNull(face["Address"]) { "no Address" })
        data.put("client_priv_key", requireNotNull(face["PrivateKey"]) { "no PrivateKey" })
        data.put("server_pub_key", requireNotNull(peer["PublicKey"]) { "no PublicKey" })
        data.putOrRemove("psk_key", peer["PresharedKey"])
        data.put("allowed_ips", JSONArray(peer["AllowedIPs"].orEmpty().split(",").map(String::trim).filter(String::isNotEmpty)))
        data.putOrRemove("persistent_keep_alive", peer["PersistentKeepalive"])
        face["MTU"]?.let { data.put("mtu", it) }
        AWG_PARAMETERS.forEach { data.putOrRemove(it, face[it]) }
        config.put("hostName", host)
        val dns = face["DNS"].orEmpty().split(",").map(String::trim).filter(IPV4_ADDRESS::matches)
        dns.getOrNull(0)?.let { config.put("dns1", it) }
        dns.getOrNull(1)?.let { config.put("dns2", it) }
    }

    private fun sectionsOf(text: String): Map<String, Map<String, String>> {
        val sections = mutableMapOf<String, MutableMap<String, String>>()
        var current: MutableMap<String, String>? = null
        text.lineSequence().map(String::trim).filter { it.isNotEmpty() && !it.startsWith("#") }.forEach { line ->
            if (line.startsWith("[") && line.endsWith("]")) {
                current = sections.getOrPut(line.substring(1, line.length - 1).trim()) { mutableMapOf() }
            } else {
                val separator = line.indexOf('=')
                if (separator > 0) current?.put(line.substring(0, separator).trim(), line.substring(separator + 1).trim())
            }
        }
        return sections
    }

    private fun endpointOf(value: String): Pair<String, Int>? {
        val separator = value.lastIndexOf(':')
        if (separator <= 0) return null
        val port = value.substring(separator + 1).trim().toIntOrNull() ?: return null
        return value.substring(0, separator).trim().removePrefix("[").removeSuffix("]") to port
    }

    private fun JSONObject.putOrRemove(key: String, value: String?) {
        if (value.isNullOrEmpty()) remove(key) else put(key, value)
    }
}
