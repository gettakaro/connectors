pluginManagement {
    repositories {
        gradlePluginPortal()
        maven("https://repo.papermc.io/repository/maven-public/")
        maven("https://maven.fabricmc.net/")
        maven("https://maven.neoforged.net/releases")
    }
}

rootProject.name = "takaro-minecraft"

include("core")
include("paper")
include("neoforge")
include("fabric")
