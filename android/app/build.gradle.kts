plugins {
  id("com.android.application")
  id("org.jetbrains.kotlin.android")
}

val repoVersion: String by lazy {
  val file = rootProject.projectDir.parentFile.resolve("VERSION")
  if (!file.exists()) {
    throw GradleException("Missing VERSION file at ${file.absolutePath}")
  }
  val value = file.readText().trim()
  if (value.isBlank()) {
    throw GradleException("VERSION file is empty")
  }
  value
}

fun parseSemVer(version: String): Triple<Int, Int, Int> {
  val parts = version.split('.')
  if (parts.size !in 2..3) {
    throw GradleException("VERSION must be MAJOR.MINOR or MAJOR.MINOR.PATCH, got '$version'")
  }
  val major = parts[0].toIntOrNull()
    ?: throw GradleException("Invalid major version in '$version'")
  val minor = parts[1].toIntOrNull()
    ?: throw GradleException("Invalid minor version in '$version'")
  val patch = when (parts.size) {
    3 -> parts[2].toIntOrNull()
      ?: throw GradleException("Invalid patch version in '$version'")
    else -> 0
  }
  if (major < 0 || minor < 0 || patch < 0) {
    throw GradleException("VERSION components must be non-negative, got '$version'")
  }
  return Triple(major, minor, patch)
}

val (versionMajor, versionMinor, versionPatch) = parseSemVer(repoVersion)
val computedVersionCode = (versionMajor * 10000) + (versionMinor * 100) + versionPatch

android {
  namespace = "com.wifinder.android"
  compileSdk = 34

  defaultConfig {
    applicationId = "com.wifinder.android"
    minSdk = 26
    targetSdk = 34
    versionCode = computedVersionCode
    versionName = repoVersion

    testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
  }

  buildTypes {
    release {
      isMinifyEnabled = false
      proguardFiles(
        getDefaultProguardFile("proguard-android-optimize.txt"),
        "proguard-rules.pro",
      )
    }
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
  }

  kotlinOptions {
    jvmTarget = "17"
  }

  buildFeatures {
    compose = true
  }

  composeOptions {
    kotlinCompilerExtensionVersion = "1.5.14"
  }

  packaging {
    resources {
      excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
  }
}

dependencies {
  val composeBom = platform("androidx.compose:compose-bom:2024.09.02")

  implementation("androidx.core:core-ktx:1.13.1")
  implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.5")
  implementation("androidx.activity:activity-compose:1.9.2")
  implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.5")
  implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.5")

  implementation(composeBom)
  androidTestImplementation(composeBom)

  implementation("androidx.compose.ui:ui")
  implementation("androidx.compose.ui:ui-tooling-preview")
  implementation("androidx.compose.material3:material3")
  implementation("androidx.compose.foundation:foundation")
  implementation("com.google.android.material:material:1.12.0")

  implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
  implementation("com.google.android.gms:play-services-location:21.3.0")

  testImplementation("junit:junit:4.13.2")
  testImplementation("org.jetbrains.kotlin:kotlin-test:1.9.24")

  androidTestImplementation("androidx.test.ext:junit:1.2.1")
  androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
  androidTestImplementation("androidx.compose.ui:ui-test-junit4")

  debugImplementation("androidx.compose.ui:ui-tooling")
  debugImplementation("androidx.compose.ui:ui-test-manifest")
}
