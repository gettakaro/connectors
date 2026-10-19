plugins {
    `java-library`
}

// M0 placeholder. M1 lifts games/minecraft/mod/core here (package io.takaro.zomboid.core).
dependencies {
    implementation(libs.java.websocket)
    implementation(libs.gson)

    testImplementation(libs.junit.jupiter)
    testRuntimeOnly(libs.junit.platform.launcher)
}

// The connector version (games/zomboid/version.txt, or -Pversion) and the game icon ship as
// resources, so identify and game.profile never carry a hand-written value.
val generateConnectorResources by tasks.registering {
    description = "Writes the connector version resource and copies the game icon."
    val output = layout.buildDirectory.dir("generated/takaro-core")
    val connectorVersion = project.version.toString()
    val icon = rootProject.file("../assets/icon-256.png")
    inputs.property("connectorVersion", connectorVersion)
    inputs.file(icon)
    outputs.dir(output)
    doLast {
        val dir = output.get().dir("io/takaro/zomboid/core").asFile
        dir.mkdirs()
        dir.resolve("takaro-connector.properties").writeText("version=$connectorVersion\n")
        icon.copyTo(dir.resolve("icon-256.png"), overwrite = true)
    }
}

sourceSets.main {
    resources.srcDir(generateConnectorResources)
}

tasks.test {
    useJUnitPlatform()
    systemProperty("takaro.versionFile", rootProject.file("../version.txt").absolutePath)
    systemProperty("takaro.iconFile", rootProject.file("../assets/icon-256.png").absolutePath)
}
