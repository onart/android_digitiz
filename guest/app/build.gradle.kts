plugins {
    id("com.android.application")
}

android {
    namespace = "com.onart.digitiz"
    compileSdk = 35

    // Pinned so the build is reproducible; see docs/ARCHITECTURE.md §11.
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "com.onart.digitiz"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // Android 15+ can run with 16 KB memory pages. Without this
                    // the platform shows a compatibility warning dialog over
                    // the app, which also steals focus.
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                )
            }
        }

        // Only what the test device needs. Add more before shipping.
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
}

dependencies {
    implementation("androidx.games:games-activity:3.0.5")

    // GameActivity extends AppCompatActivity but its POM declares no
    // dependencies, so this has to be requested explicitly. It also forces the
    // app theme to be an AppCompat descendant; see res/values/themes.xml.
    implementation("androidx.appcompat:appcompat:1.7.0")

    // games-activity drags in kotlin-stdlib-jdk8:1.6.21 while appcompat wants
    // kotlin-stdlib:1.8.22. Kotlin 1.8 folded the jdk7/jdk8 artifacts into the
    // main one, so the two overlap and dexing fails on duplicate classes.
    // Raising the jdk7/jdk8 artifacts past 1.8 makes them empty shims.
    constraints {
        implementation("org.jetbrains.kotlin:kotlin-stdlib-jdk7:1.9.24")
        implementation("org.jetbrains.kotlin:kotlin-stdlib-jdk8:1.9.24")
    }
}
