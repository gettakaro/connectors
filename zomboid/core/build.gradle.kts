plugins {
    `java-library`
}

// M0 placeholder. M1 lifts minecraft/core here (package io.takaro.zomboid.core).
dependencies {
    implementation(libs.java.websocket)
    implementation(libs.gson)

    testImplementation(libs.junit.jupiter)
    testRuntimeOnly(libs.junit.platform.launcher)
}

tasks.test {
    useJUnitPlatform()
}
