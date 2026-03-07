package com.anatdx.icepatch.util

import android.app.AppOpsManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import androidx.core.graphics.scale
import androidx.core.net.toUri
import com.anatdx.icepatch.R
import com.anatdx.icepatch.ui.MainActivity
import com.anatdx.icepatch.ui.WebUIActivity
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream
import java.util.Locale

object ModuleShortcut {
    private const val TAG = "ModuleShortcut"

    fun createModuleActionShortcut(context: Context, moduleId: String, name: String, iconUri: String?) {
        val shortcutId = "module_action_$moduleId"
        val shortcutIntent = Intent(context, MainActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            putExtra("shortcut_type", "module_action")
            putExtra("module_id", moduleId)
            addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        }
        createModuleShortcut(context, shortcutId, name, iconUri, shortcutIntent)
    }

    fun createModuleWebUiShortcut(context: Context, moduleId: String, name: String, iconUri: String?) {
        val shortcutId = "module_webui_$moduleId"
        val shortcutIntent = Intent(context, WebUIActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            data = "apatch://webui/$moduleId".toUri()
            putExtra("id", moduleId)
            putExtra("name", name)
            putExtra("from_webui_shortcut", true)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        }
        createModuleShortcut(context, shortcutId, name, iconUri, shortcutIntent)
    }

    fun hasModuleActionShortcut(context: Context, moduleId: String): Boolean {
        return hasPinnedShortcut(context, "module_action_$moduleId")
    }

    fun hasModuleWebUiShortcut(context: Context, moduleId: String): Boolean {
        return hasPinnedShortcut(context, "module_webui_$moduleId")
    }

    fun deleteModuleActionShortcut(context: Context, moduleId: String) {
        deleteShortcut(context, "module_action_$moduleId")
    }

    fun deleteModuleWebUiShortcut(context: Context, moduleId: String) {
        deleteShortcut(context, "module_webui_$moduleId")
    }

    fun loadShortcutBitmap(context: Context, iconUri: String?): Bitmap? {
        if (iconUri.isNullOrBlank()) {
            return null
        }
        return try {
            val uri = iconUri.toUri()
            val rawBitmap = if (uri.scheme.equals("su", ignoreCase = true)) {
                val path = uri.path.orEmpty()
                if (path.isNotBlank()) {
                    val suFile = SuFile(path).apply { shell = getRootShell(true) }
                    SuFileInputStream.open(suFile).use(BitmapFactory::decodeStream)
                } else {
                    null
                }
            } else {
                context.contentResolver.openInputStream(uri)?.use(BitmapFactory::decodeStream)
            }
            if (rawBitmap != null) {
                val w = rawBitmap.width
                val h = rawBitmap.height
                val side = minOf(w, h)
                val square = try {
                    Bitmap.createBitmap(rawBitmap, (w - side) / 2, (h - side) / 2, side, side)
                } catch (_: Throwable) {
                    rawBitmap
                }
                if (square !== rawBitmap && !rawBitmap.isRecycled) {
                    rawBitmap.recycle()
                }
                if (side > 512) {
                    try {
                        val scaled = square.scale(512, 512)
                        if (scaled !== square && !square.isRecycled) {
                            square.recycle()
                        }
                        scaled
                    } catch (_: Throwable) {
                        square
                    }
                } else {
                    square
                }
            } else {
                null
            }
        } catch (t: Throwable) {
            Log.w(TAG, "loadShortcutBitmap failed: ${t.message}", t)
            null
        }
    }

    private fun createModuleShortcut(
        context: Context,
        shortcutId: String,
        name: String,
        iconUri: String?,
        shortcutIntent: Intent
    ) {
        val hasPinned = hasPinnedShortcut(context, shortcutId)
        val icon = loadShortcutBitmap(context, iconUri)?.let(IconCompat::createWithBitmap)
            ?: IconCompat.createWithResource(context, R.mipmap.ic_launcher)

        val shortcut = ShortcutInfoCompat.Builder(context, shortcutId)
            .setShortLabel(name)
            .setIntent(shortcutIntent)
            .setIcon(icon)
            .build()

        runCatching { ShortcutManagerCompat.pushDynamicShortcut(context, shortcut) }

        if (hasPinned) {
            Toast.makeText(context, context.getString(R.string.module_shortcut_updated), Toast.LENGTH_SHORT).show()
            return
        }

        val state = getShortcutPermissionState(context)
        val manufacturer = Build.MANUFACTURER.lowercase(Locale.ROOT)
        if (manufacturer.contains("xiaomi") && state != ShortcutPermissionState.Granted) {
            tryGrantMiuiShortcutPermissionByRoot(context)
            if (getShortcutPermissionState(context) != ShortcutPermissionState.Granted) {
                showShortcutPermissionHint(context)
                return
            }
        } else if (state == ShortcutPermissionState.Denied || state == ShortcutPermissionState.Ask) {
            showShortcutPermissionHint(context)
            return
        }

        if (!ShortcutManagerCompat.isRequestPinShortcutSupported(context)) {
            Toast.makeText(
                context,
                context.getString(R.string.module_shortcut_not_supported),
                Toast.LENGTH_LONG
            ).show()
            return
        }

        val pinned = runCatching {
            ShortcutManagerCompat.requestPinShortcut(context, shortcut, null)
        }.getOrDefault(false)
        if (pinned) {
            Toast.makeText(context, context.getString(R.string.module_shortcut_created), Toast.LENGTH_SHORT).show()
        } else {
            showShortcutPermissionHint(context)
        }
    }

    private fun hasPinnedShortcut(context: Context, id: String): Boolean {
        return runCatching {
            ShortcutManagerCompat.getShortcuts(context, ShortcutManagerCompat.FLAG_MATCH_PINNED)
                .any { it.id == id && it.isEnabled }
        }.getOrDefault(false)
    }

    private fun deleteShortcut(context: Context, id: String) {
        runCatching { ShortcutManagerCompat.removeDynamicShortcuts(context, listOf(id)) }
        runCatching { ShortcutManagerCompat.disableShortcuts(context, listOf(id), "") }
    }

    private enum class ShortcutPermissionState {
        Granted,
        Denied,
        Ask,
        Unknown
    }

    private fun checkMiuiShortcutPermission(context: Context): ShortcutPermissionState {
        return try {
            val appOps = context.getSystemService(Context.APP_OPS_SERVICE) as? AppOpsManager
                ?: return ShortcutPermissionState.Unknown
            val method = Class.forName(AppOpsManager::class.java.name).getDeclaredMethod(
                "checkOpNoThrow",
                Int::class.javaPrimitiveType,
                Int::class.javaPrimitiveType,
                String::class.java
            )
            when (method.invoke(appOps, 10017, context.applicationInfo.uid, context.packageName)?.toString()) {
                "0" -> ShortcutPermissionState.Granted
                "1" -> ShortcutPermissionState.Denied
                "5" -> ShortcutPermissionState.Ask
                else -> ShortcutPermissionState.Unknown
            }
        } catch (_: Throwable) {
            ShortcutPermissionState.Unknown
        }
    }

    private fun checkOppoShortcutPermission(context: Context): ShortcutPermissionState {
        return try {
            val cursor = context.contentResolver.query(
                Uri.parse("content://settings/secure/launcher_shortcut_permission_settings"),
                null, null, null, null
            ) ?: return ShortcutPermissionState.Unknown
            cursor.use {
                val idx = it.getColumnIndex("value")
                if (idx == -1) return ShortcutPermissionState.Unknown
                while (it.moveToNext()) {
                    val value = it.getString(idx).orEmpty()
                    if (value.contains("${context.packageName}, 1")) return ShortcutPermissionState.Granted
                    if (value.contains("${context.packageName}, 0")) return ShortcutPermissionState.Denied
                }
            }
            ShortcutPermissionState.Unknown
        } catch (_: Throwable) {
            ShortcutPermissionState.Unknown
        }
    }

    private fun tryGrantMiuiShortcutPermissionByRoot(context: Context): Boolean {
        return runCatching {
            val cmd = "appops set ${context.packageName} 10017 allow"
            getRootShell().newJob().add(cmd).exec().isSuccess
        }.getOrDefault(false)
    }

    private fun getShortcutPermissionState(context: Context): ShortcutPermissionState {
        val manufacturer = Build.MANUFACTURER.lowercase(Locale.ROOT)
        return when {
            manufacturer.contains("xiaomi") -> checkMiuiShortcutPermission(context)
            manufacturer.contains("oppo") -> checkOppoShortcutPermission(context)
            else -> ShortcutPermissionState.Unknown
        }
    }

    private fun showShortcutPermissionHint(context: Context) {
        val manufacturer = Build.MANUFACTURER.lowercase(Locale.ROOT)
        val messageRes = when {
            manufacturer.contains("xiaomi") -> R.string.module_shortcut_permission_tip_xiaomi
            manufacturer.contains("oppo") -> R.string.module_shortcut_permission_tip_oppo
            else -> R.string.module_shortcut_permission_tip_default
        }
        Toast.makeText(context, context.getString(messageRes), Toast.LENGTH_LONG).show()
        if (getShortcutPermissionState(context) != ShortcutPermissionState.Granted) {
            openAppDetailsSettings(context)
        }
    }

    private fun openAppDetailsSettings(context: Context) {
        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.fromParts("package", context.packageName, null)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        runCatching { context.startActivity(intent) }
    }
}
