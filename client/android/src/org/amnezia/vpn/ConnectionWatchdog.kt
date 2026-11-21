package org.amnezia.vpn

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.amnezia.vpn.protocol.ProtocolState
import org.amnezia.vpn.protocol.ProtocolState.CONNECTED
import org.amnezia.vpn.protocol.ProtocolState.CONNECTING
import org.amnezia.vpn.protocol.ProtocolState.RECONNECTING
import org.amnezia.vpn.protocol.Statistics
import org.amnezia.vpn.util.Log

private const val TAG = "ConnectionWatchdog"
private const val CONNECT_TIMEOUT_MS = 30_000L
private const val TRAFFIC_CHECK_INTERVAL_MS = 60_000L
private const val SILENT_CHECKS_TO_GIVE_UP = 5
private const val MS_IN_MINUTE = 60_000L
private val RETRY_DELAYS_MINUTES = longArrayOf(1, 2, 4, 8, 15, 30)

class ConnectionWatchdog(
    private val scope: CoroutineScope,
    private val statistics: () -> Statistics,
    private val turnOff: () -> Unit,
    private val turnOn: () -> Unit
) {
    private var watchJob: Job? = null
    private var retryJob: Job? = null
    private var failedAttempts = 0

    val isRetryPending: Boolean
        get() = retryJob?.isActive == true

    fun onStateChanged(state: ProtocolState) {
        watchJob?.cancel()
        watchJob = when (state) {
            CONNECTING, RECONNECTING -> scope.launch {
                delay(CONNECT_TIMEOUT_MS)
                giveUp("no connection in ${CONNECT_TIMEOUT_MS / 1000} s")
            }

            CONNECTED -> {
                failedAttempts = 0
                scope.launch { watchTraffic() }
            }

            else -> null
        }
    }

    fun reset() {
        watchJob?.cancel()
        retryJob?.cancel()
        failedAttempts = 0
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
        Log.w(TAG, "The VPN server does not answer: $reason. Plain internet until a retry in $delayMinutes min")
        retryJob?.cancel()
        retryJob = scope.launch {
            delay(delayMinutes * MS_IN_MINUTE)
            turnOn()
        }
        turnOff()
    }
}
