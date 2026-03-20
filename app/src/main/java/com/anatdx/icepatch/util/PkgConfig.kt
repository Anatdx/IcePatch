package com.anatdx.icepatch.util

import android.os.Parcelable
import android.util.Log
import androidx.annotation.Keep
import androidx.compose.runtime.Immutable
import kotlinx.parcelize.Parcelize
import com.anatdx.icepatch.Natives
import kotlin.concurrent.thread

object PkgConfig {
    private const val TAG = "PkgConfig"

    private const val CSV_HEADER = "pkg,exclude,allow,uid,to_uid,sctx"

    @Immutable
    @Parcelize
    @Keep
    data class Config(
        var pkg: String = "", var exclude: Int = 0, var allow: Int = 0, var profile: Natives.Profile
    ) : Parcelable {
        companion object {
            fun fromLine(line: String): Config {
                val sp = line.split(",")
                val profile = Natives.Profile(sp[3].toInt(), sp[4].toInt(), sp[5])
                return Config(sp[0], sp[1].toInt(), sp[2].toInt(), profile)
            }
        }

        fun isDefault(): Boolean {
            return allow == 0 && exclude == 0
        }

        fun toLine(): String {
            return "${pkg},${exclude},${allow},${profile.uid},${profile.toUid},${profile.scontext}"
        }
    }

    fun readConfigs(): HashMap<Int, Config> {
        val configs = HashMap<Int, Config>()
        val result = runManagedApd("tool", "package-config", "list")
        if (!result.isSuccess) {
            Log.w(TAG, "readConfigs failed: ${result.lines.joinToString(" | ")}")
            return configs
        }
        result.lines.drop(1).filter { it.isNotEmpty() && it.contains(",") }.forEach {
            runCatching {
                Log.d(TAG, it)
                val p = Config.fromLine(it)
                if (!p.isDefault()) {
                    configs[p.profile.uid] = p
                }
            }.onFailure { error ->
                Log.w(TAG, "skip malformed config row: $it (${error.message})")
            }
        }
        return configs
    }

    private fun writeConfig(config: Config): Boolean {
        val result = runManagedApd(
            "tool",
            "package-config",
            "set",
            "--pkg",
            config.pkg,
            "--exclude",
            config.exclude.toString(),
            "--allow",
            config.allow.toString(),
            "--uid",
            config.profile.uid.toString(),
            "--to-uid",
            config.profile.toUid.toString(),
            "--sctx",
            config.profile.scontext,
        )
        if (!result.isSuccess) {
            Log.e(TAG, "writeConfig failed: ${result.lines.joinToString(" | ")}")
        }
        return result.isSuccess
    }

    fun changeConfig(config: Config) {
        val snapshot = config.copy(
            profile = Natives.Profile(
                uid = config.profile.uid,
                toUid = config.profile.toUid,
                scontext = config.profile.scontext,
            )
        )
        thread {
            synchronized(PkgConfig::class.java) {
                if (snapshot.allow == 1) {
                    snapshot.exclude = 0
                }
                Log.d(TAG, "change config: $snapshot")
                writeConfig(snapshot)
            }
        }
    }
}
