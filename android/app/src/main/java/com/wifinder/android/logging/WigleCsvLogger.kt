package com.wifinder.android.logging

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.SystemClock
import android.provider.MediaStore
import com.wifinder.android.model.WigleCsvFormatter
import com.wifinder.android.model.WigleWifiRow
import java.io.BufferedWriter
import java.io.File
import java.io.FileWriter
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class WigleCsvLogger(private val context: Context) {
  companion object {
    private const val FLUSH_INTERVAL_MS = 750L
    private const val FLUSH_BATCH_ROWS = 32
  }

  private val fileNameFormat = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)

  @Volatile
  private var writer: BufferedWriter? = null

  @Volatile
  private var currentFile: File? = null

  @Volatile
  private var currentUri: Uri? = null

  @Volatile
  private var currentDisplayPath: String? = null

  @Volatile
  private var pendingRows: Int = 0

  @Volatile
  private var lastFlushMs: Long = 0L

  val isActive: Boolean
    get() = writer != null

  fun currentPath(): String? = currentDisplayPath ?: currentFile?.absolutePath

  @Synchronized
  fun startSession(): CsvSessionInfo {
    stopSession()

    val fileName = "wigle-${fileNameFormat.format(Date())}.csv"
    val out: BufferedWriter
    val displayPath: String

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      val values =
        ContentValues().apply {
          put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
          put(MediaStore.MediaColumns.MIME_TYPE, "text/csv")
          put(
            MediaStore.MediaColumns.RELATIVE_PATH,
            Environment.DIRECTORY_DOWNLOADS + "/wifinder",
          )
          put(MediaStore.MediaColumns.IS_PENDING, 1)
        }

      val resolver = context.contentResolver
      val uri =
        resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
          ?: throw IllegalStateException("Unable to create CSV entry in Downloads")

      try {
        val stream =
          resolver.openOutputStream(uri, "w")
            ?: throw IllegalStateException("Unable to open CSV output stream")
        out = BufferedWriter(OutputStreamWriter(stream, Charsets.UTF_8))
      } catch (e: Exception) {
        resolver.delete(uri, null, null)
        throw e
      }

      currentUri = uri
      currentFile = null
      displayPath = "Downloads/wifinder/$fileName"
      currentDisplayPath = displayPath
    } else {
      @Suppress("DEPRECATION")
      val baseDir =
        File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "wifinder")
      if (!baseDir.exists()) {
        baseDir.mkdirs()
      }

      val file = File(baseDir, fileName)
      out = BufferedWriter(FileWriter(file, false))
      currentFile = file
      currentUri = null
      displayPath = file.absolutePath
      currentDisplayPath = displayPath
    }

    val header =
      WigleCsvFormatter.header(
        appRelease = "wifinder-android-1",
        model = Build.MODEL ?: "unknown",
        release = Build.VERSION.RELEASE ?: "unknown",
        device = Build.DEVICE ?: "unknown",
        display = Build.DISPLAY ?: "unknown",
        board = Build.BOARD ?: "unknown",
        brand = Build.BRAND ?: "unknown",
      )

    for (line in header) {
      out.write(line)
      out.newLine()
    }
    out.flush()

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      currentUri?.let { uri ->
        context.contentResolver.update(
          uri,
          ContentValues().apply {
            put(MediaStore.MediaColumns.IS_PENDING, 0)
          },
          null,
          null,
        )
      }
    }

    writer = out
    pendingRows = 0
    lastFlushMs = SystemClock.elapsedRealtime()
    return CsvSessionInfo(displayPath = displayPath, uri = currentUri)
  }

  @Synchronized
  fun append(row: WigleWifiRow) {
    val out = writer ?: return
    out.write(WigleCsvFormatter.formatRow(row))
    out.newLine()
    pendingRows += 1
    val nowMs = SystemClock.elapsedRealtime()
    if (pendingRows >= FLUSH_BATCH_ROWS || nowMs - lastFlushMs >= FLUSH_INTERVAL_MS) {
      out.flush()
      pendingRows = 0
      lastFlushMs = nowMs
    }
  }

  @Synchronized
  fun stopSession() {
    writer?.flush()
    writer?.close()
    writer = null
    pendingRows = 0
    lastFlushMs = 0L
    currentUri = null
    currentFile = null
    currentDisplayPath = null
  }
}

data class CsvSessionInfo(
  val displayPath: String,
  val uri: Uri?,
)
