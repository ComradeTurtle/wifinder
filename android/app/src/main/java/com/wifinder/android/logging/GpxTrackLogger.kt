package com.wifinder.android.logging

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.SystemClock
import android.provider.MediaStore
import com.wifinder.android.model.GpxTrackFormatter
import com.wifinder.android.model.GpxTrackSample
import java.io.BufferedWriter
import java.io.File
import java.io.FileWriter
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class GpxTrackLogger(private val context: Context) {
  companion object {
    private const val FLUSH_INTERVAL_MS = 1000L
    private const val FLUSH_BATCH_POINTS = 8
    private val RELATIVE_DIR = Environment.DIRECTORY_DOWNLOADS + "/wifinder"
  }

  private val fileNameFormat = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)

  @Volatile private var writer: BufferedWriter? = null
  @Volatile private var currentFile: File? = null
  @Volatile private var currentUri: Uri? = null
  @Volatile private var currentDisplayPath: String? = null
  @Volatile private var finalName: String = ""
  @Volatile private var tmpName: String = ""
  @Volatile private var pendingPoints: Int = 0
  @Volatile private var pointCount: Long = 0L
  @Volatile private var lastFlushMs: Long = 0L

  val isActive: Boolean
    get() = writer != null

  fun currentPath(): String? = currentDisplayPath ?: currentFile?.absolutePath

  fun currentPointCount(): Long = pointCount

  @Synchronized
  fun startSession(sessionId: Long, sourceTag: String): GpxSessionInfo {
    stopSession(finalize = true)
    cleanupStaleTemporaryFiles()

    val sid = java.lang.Long.toUnsignedString(sessionId, 16).uppercase(Locale.US)
    val stamp = fileNameFormat.format(Date())
    finalName = "track-$sourceTag-$sid-$stamp.gpx"
    tmpName = "$finalName.tmp"
    val trackName = "session-$sid-$sourceTag"
    val out: BufferedWriter
    val displayPath = "Downloads/wifinder/$finalName"

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      val values =
        ContentValues().apply {
          put(MediaStore.MediaColumns.DISPLAY_NAME, tmpName)
          put(MediaStore.MediaColumns.MIME_TYPE, "application/gpx+xml")
          put(MediaStore.MediaColumns.RELATIVE_PATH, "$RELATIVE_DIR/")
          put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
      val resolver = context.contentResolver
      val uri =
        resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
          ?: throw IllegalStateException("Unable to create GPX entry in Downloads")
      try {
        val stream =
          resolver.openOutputStream(uri, "w")
            ?: throw IllegalStateException("Unable to open GPX output stream")
        out = BufferedWriter(OutputStreamWriter(stream, Charsets.UTF_8))
      } catch (e: Exception) {
        resolver.delete(uri, null, null)
        throw e
      }
      currentUri = uri
      currentFile = null
      currentDisplayPath = displayPath
    } else {
      @Suppress("DEPRECATION")
      val baseDir =
        File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "wifinder")
      if (!baseDir.exists()) {
        baseDir.mkdirs()
      }
      val file = File(baseDir, tmpName)
      out = BufferedWriter(FileWriter(file, false))
      currentFile = file
      currentUri = null
      currentDisplayPath = displayPath
    }

    out.write(GpxTrackFormatter.header(trackName))
    out.newLine()
    out.flush()
    writer = out
    pendingPoints = 0
    pointCount = 0L
    lastFlushMs = SystemClock.elapsedRealtime()
    return GpxSessionInfo(displayPath = displayPath, uri = currentUri)
  }

  @Synchronized
  fun appendTrackPoint(
    sample: GpxTrackSample,
    altitudeMeters: Double? = null,
    hdop: Double? = null,
    satCount: Int? = null,
  ) {
    val out = writer ?: return
    out.write(
      GpxTrackFormatter.trackPoint(
        lat = sample.lat,
        lon = sample.lon,
        unixTimeMs = sample.unixTimeMs,
        altitudeMeters = altitudeMeters,
        hdop = hdop,
        satCount = satCount,
      ),
    )
    out.newLine()
    pendingPoints += 1
    pointCount += 1L
    val now = SystemClock.elapsedRealtime()
    if (pendingPoints >= FLUSH_BATCH_POINTS || now - lastFlushMs >= FLUSH_INTERVAL_MS) {
      out.flush()
      pendingPoints = 0
      lastFlushMs = now
    }
  }

  @Synchronized
  fun stopSession(finalize: Boolean) {
    val out = writer
    if (out == null) {
      clearState()
      return
    }
    try {
      if (finalize) {
        out.write(GpxTrackFormatter.footer())
        out.newLine()
      }
      out.flush()
    } finally {
      try {
        out.close()
      } catch (_: Exception) {}
      writer = null
    }

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      val uri = currentUri
      if (uri != null) {
        val resolver = context.contentResolver
        if (finalize) {
          resolver.update(
            uri,
            ContentValues().apply {
              put(MediaStore.MediaColumns.DISPLAY_NAME, finalName)
              put(MediaStore.MediaColumns.IS_PENDING, 0)
            },
            null,
            null,
          )
        } else {
          resolver.delete(uri, null, null)
        }
      }
    } else {
      val file = currentFile
      if (file != null) {
        if (finalize) {
          val dest = File(file.parentFile, finalName)
          if (dest.exists()) {
            dest.delete()
          }
          file.renameTo(dest)
        } else {
          file.delete()
        }
      }
    }
    clearState(clearPath = !finalize)
  }

  private fun clearState(clearPath: Boolean = false) {
    pendingPoints = 0
    pointCount = 0L
    lastFlushMs = 0L
    currentUri = null
    currentFile = null
    finalName = ""
    tmpName = ""
    if (clearPath) {
      currentDisplayPath = null
    }
  }

  @Synchronized
  private fun cleanupStaleTemporaryFiles() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      val resolver = context.contentResolver
      val projection = arrayOf(MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME)
      val selection = "${MediaStore.MediaColumns.RELATIVE_PATH}=? AND ${MediaStore.MediaColumns.DISPLAY_NAME} LIKE ?"
      val args = arrayOf("$RELATIVE_DIR/", "%.gpx.tmp")
      resolver.query(
        MediaStore.Downloads.EXTERNAL_CONTENT_URI,
        projection,
        selection,
        args,
        null,
      )?.use { cursor ->
        val idCol = cursor.getColumnIndexOrThrow(MediaStore.MediaColumns._ID)
        while (cursor.moveToNext()) {
          val id = cursor.getLong(idCol)
          val uri = Uri.withAppendedPath(MediaStore.Downloads.EXTERNAL_CONTENT_URI, id.toString())
          resolver.delete(uri, null, null)
        }
      }
      return
    }

    @Suppress("DEPRECATION")
    val baseDir =
      File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "wifinder")
    if (!baseDir.exists()) {
      return
    }
    val files = baseDir.listFiles() ?: return
    for (file in files) {
      if (file.name.endsWith(".gpx.tmp", ignoreCase = true)) {
        file.delete()
      }
    }
  }
}

data class GpxSessionInfo(
  val displayPath: String,
  val uri: Uri?,
)
