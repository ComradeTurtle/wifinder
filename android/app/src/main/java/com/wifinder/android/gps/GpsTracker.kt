package com.wifinder.android.gps

import android.annotation.SuppressLint
import android.content.Context
import android.location.Location
import android.os.Looper
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority

class GpsTracker(
  context: Context,
  private val onLocation: (Location) -> Unit,
  private val onError: (String) -> Unit,
) {
  private val fusedClient = LocationServices.getFusedLocationProviderClient(context)

  private var running = false

  private val callback =
    object : LocationCallback() {
      override fun onLocationResult(result: LocationResult) {
        for (location in result.locations) {
          onLocation(location)
        }
      }
    }

  @SuppressLint("MissingPermission")
  fun start() {
    if (running) {
      return
    }
    val request =
      LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, 1000L)
        .setMinUpdateIntervalMillis(500L)
        .setMaxUpdateDelayMillis(1500L)
        .build()

    try {
      fusedClient.requestLocationUpdates(request, callback, Looper.getMainLooper())
      running = true
    } catch (e: SecurityException) {
      onError("GPS start failed: missing location permission")
    } catch (e: Exception) {
      onError("GPS start failed: ${e.message}")
    }
  }

  fun stop() {
    if (!running) {
      return
    }
    fusedClient.removeLocationUpdates(callback)
    running = false
  }
}
