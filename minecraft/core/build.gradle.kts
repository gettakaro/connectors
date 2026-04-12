plugins {
    `java-library`
}

dependencies {
    implementation(libs.java.websocket)
    implementation(libs.gson)
    compileOnly(libs.log4j.api)

    testImplementation("org.junit.jupiter:junit-jupiter:5.11.4")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test {
    useJUnitPlatform()
}
