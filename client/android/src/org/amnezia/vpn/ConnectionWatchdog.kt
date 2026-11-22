package org.amnezia.vpn

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.amnezia.vpn.protocol.ProtocolState
import org.amnezia.vpn.protocol.ProtocolState.CONNECTED
import org.amnezia.vpn.protocol.ProtocolState.CONNECTING
import org.amnezia.vpn.protocol.ProtocolState.DISCONNECTED
import org.amnezia.vpn.protocol.ProtocolState.RECONNECTING
import org.amnezia.vpn.protocol.ProtocolState.UNKNOWN
import org.amnezia.vpn.protocol.Statistics
import org.amnezia.vpn.util.Log

private const val TAG = "ConnectionWatchdog"
private const val CONNECT_TIMEOUT_MS = 30_000L
private const val TUNNEL_DOWN_TIMEOUT_MS = 10_000L
private const val TRAFFIC_CHECK_INTERVAL_MS = 60_000L
private const val SILENT_CHECKS_TO_GIVE_UP = 5
private const val MS_IN_MINUTE = 60_000L
private val RETRY_DELAYS_MINUTES = longArrayOf(1, 2, 4, 8, 15, 30)

class ConnectionWatchdog(
    private val scope: CoroutineScope,
    private val statistics: () -> Statistics,
    private val turnOff: () -> Unit,
    private val turnOn: (String?) -> Unit,
    private val savedConfig: () -> String
) {
    private var watchJob: Job? = null
    private var retryJob: Job? = null
    private var failedAttempts = 0
    private var isOwnAttempt = false
    private val state = MutableStateFlow(UNKNOWN)

    val isRetryPending: Boolean
        get() = retryJob?.isActive == true

    fun onStateChanged(newState: ProtocolState) {
        state.value = newState
        watchJob?.cancel()
        watchJob = when (newState) {
            CONNECTING, RECONNECTING -> scope.launch {
                delay(CONNECT_TIMEOUT_MS)
                giveUp("no connection in ${CONNECT_TIMEOUT_MS / 1000} s")
            }

            CONNECTED -> {
                failedAttempts = 0
                isOwnAttempt = false
                scope.launch { watchTraffic() }
            }

            else -> null
        }
        if (newState == DISCONNECTED && isOwnAttempt) giveUp("the connection attempt failed")
    }

    fun reset() {
        watchJob?.cancel()
        retryJob?.cancel()
        failedAttempts = 0
        isOwnAttempt = false
    }

    private suspend fun watchTraffic() {
        var previous = statistics()
        var silentChecks = 0
        while (silentChecks < SILENT_CHECKS_TO_GIVE_UP) {
            delay(TRAFFIC_CHECK_INTERVAL_MS)
            val current = statistics()
            silentChecks = when {
                current.rxBytes > previous.rxBytes -> 0
                current.txBytes > previous.txBytes -> silentChecks + 1
                else -> silentChecks
            }
            previous = current
        }
        giveUp("nothing received for $SILENT_CHECKS_TO_GIVE_UP min")
    }

    private fun giveUp(reason: String) {
        val delayMinutes = RETRY_DELAYS_MINUTES[failedAttempts.coerceAtMost(RETRY_DELAYS_MINUTES.lastIndex)]
        failedAttempts++
        isOwnAttempt = false
        Log.w(TAG, "The VPN server does not answer: $reason. Plain internet until a retry in $delayMinutes min")
        retryJob?.cancel()
        retryJob = scope.launch {
            withTimeoutOrNull(TUNNEL_DOWN_TIMEOUT_MS) { state.first { it == DISCONNECTED } }
            if (connectWithFreshConfig()) return@launch
            delay(delayMinutes * MS_IN_MINUTE)
            if (connectWithFreshConfig()) return@launch
            isOwnAttempt = true
            turnOn(null)
        }
        turnOff()
    }

    private suspend fun connectWithFreshConfig(): Boolean {
        val saved = savedConfig()
        val fresh = withContext(Dispatchers.IO) { ManagedConfigRefresher.fresh(saved) } ?: return false
        Log.i(TAG, "The backend gave a new config, connecting with it")
        isOwnAttempt = true
        turnOn(fresh)
        return true
    }
}
