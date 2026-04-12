plugins {
    alias(libs.plugins.shadow)
}

repositories {
    maven("https://repo.papermc.io/repository/maven-public/")
}

dependencies {
    implementation(project(":core"))
    compileOnly(libs.paper.api)
}

tasks.shadowJar {
    archiveClassifier.set("")
    relocate("org.java_websocket", "io.takaro.libs.websocket")
    relocate("com.google.gson", "io.takaro.libs.gson")
}

tasks.build {
    dependsOn(tasks.shadowJar)
}
