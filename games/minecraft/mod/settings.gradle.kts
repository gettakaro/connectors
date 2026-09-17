pluginManagement {
    repositories {
        gradlePluginPortal()
        maven("https://repo.papermc.io/repository/maven-public/")
        maven("https://maven.fabricmc.net/")
        maven("https://maven.neoforged.net/releases")
    }
}

// NeoForge's ModDevGradle pins its tooling JVM to the Minecraft version's Java
// level (21 for 1.21.x) while the build itself now runs on JDK 25 for Fabric /
// Minecraft 26.2. Let Gradle provision the missing JDK instead of requiring
// both to be installed.
plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "1.0.0"
}

rootProject.name = "takaro-minecraft"

include("core")

// Every directory under targets/ that carries a build file is a catalog target.
// Adding a target is adding its JSON record and its one-line build file, nothing here.
file("targets").listFiles()
    ?.filter { File(it, "build.gradle.kts").exists() }
    ?.sortedBy { it.name }
    ?.forEach {
        include(it.name)
        project(":${it.name}").projectDir = it
    }
