package org.amnezia.vpn

import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.VpnService
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.amnezia.vpn.protocol.ProtocolState.CONNECTED
import org.amnezia.vpn.util.Log

private const val TAG = "BootReceiver"

class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val isBoot = intent.action == Intent.ACTION_BOOT_COMPLETED
        if (!isBoot && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return

        val pendingResult = goAsync()
        CoroutineScope(Dispatchers.Default).launch {
            try {
                startVpn(context.applicationContext, isBoot)
            } finally {
                pendingResult.finish()
            }
        }
    }

    private suspend fun startVpn(context: Context, isBoot: Boolean) {
        val vpnState = VpnStateStore.getVpnState()
        val vpnProto = vpnState.vpnProto
        if (vpnState.serverName == null || vpnProto == null) {
            Log.d(TAG, "No saved VPN config to start")
            return
        }
        if (!isBoot && vpnState.protocolState != CONNECTED) {
            Log.d(TAG, "VPN was off before the app update")
            return
        }
        if (VpnService.prepare(context) != null) {
            Log.w(TAG, "VPN permission is not granted")
            return
        }
        try {
            ContextCompat.startForegroundService(context, Intent(context, vpnProto.serviceClass))
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start ${vpnProto.serviceClass.simpleName}: $e")
        }
    }

    companion object {
        private fun component(context: Context) = ComponentName(context, BootReceiver::class.java)

        fun isEnabled(context: Context): Boolean =
            context.packageManager.getComponentEnabledSetting(component(context)) ==
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED

        fun setEnabled(context: Context, enabled: Boolean) {
            val state = if (enabled) {
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED
            } else {
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED
            }
            context.packageManager.setComponentEnabledSetting(component(context), state, PackageManager.DONT_KILL_APP)
        }
    }
}
