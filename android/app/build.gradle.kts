import java.util.Properties

plugins {
    id("com.android.application")
}

// Same version as the Windows browser: read from CMakeLists.txt ("VERSION x.y.z").
val ulbVersion: String = Regex("""VERSION\s+(\d+\.\d+\.\d+)""")
    .find(rootProject.file("../CMakeLists.txt").readText())!!.groupValues[1]
val ulbVersionCode: Int = ulbVersion.split(".").map { it.toInt() }.let { (a, b, c) -> a * 10000 + b * 100 + c }

val signing = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "net.freedom8964.ulb"
    compileSdk = 36

    defaultConfig {
        applicationId = "net.freedom8964.ulb"
        minSdk = 26
        targetSdk = 36
        versionCode = ulbVersionCode   // 2.9.0 -> 20900
        versionName = ulbVersion
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }

    signingConfigs {
        if (signing.containsKey("storeFile")) {
            create("release") {
                storeFile = file(signing.getProperty("storeFile"))
                storePassword = signing.getProperty("storePassword")
                keyAlias = signing.getProperty("keyAlias")
                keyPassword = signing.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }

    // The network helper (warp/, Go) ships as lib/<abi>/libulbwarp.so so Android
    // unpacks it into the app's native library folder, where it may be executed.
    packaging {
        jniLibs { useLegacyPackaging = true }
    }

    // One APK per ABI plus a universal one: the helper is ~18 MB per ABI.
    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a", "x86_64")
            isUniversalApk = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.webkit:webkit:1.18.0")
}
