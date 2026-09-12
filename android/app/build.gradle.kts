import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Nightscout settings come from android/local.properties (gitignored) and are
// baked into BuildConfig, i.e. into the APK. Accepted for a sideloaded personal
// build with a read-only token (PHONE_APP_REPORT.md, decision B).
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
fun localString(key: String): String =
    "\"" + localProps.getProperty(key, "").replace("\\", "\\\\").replace("\"", "\\\"") + "\""

android {
    namespace = "com.glucosering.ring"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.glucosering.ring"
        minSdk = 26                 // runtime-registered receivers; BLE stable
        targetSdk = 34
        versionCode = 2
        versionName = "0.2.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("String", "NS_URL", localString("nightscout.url"))
        buildConfigField("String", "NS_TOKEN", localString("nightscout.token"))
    }

    buildFeatures { buildConfig = true }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    testImplementation("junit:junit:4.13.2")
    // Android's org.json is a stub under JVM unit tests; this is the real one.
    // Test classpath only -- never shipped in the APK.
    testImplementation("org.json:json:20240303")
}
