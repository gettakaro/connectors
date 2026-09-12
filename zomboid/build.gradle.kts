plugins {
    java
}

val versionFromFile = rootProject.file("version.txt").takeIf { it.exists() }?.readText()?.trim()

allprojects {
    version = findProperty("version")?.toString()?.takeIf { it != "unspecified" }
        ?: versionFromFile
        ?: "1.0.0-SNAPSHOT"
}

subprojects {
    apply(plugin = "java")

    base {
        archivesName = "takaro-zomboid-${project.name}"
    }

    java {
        toolchain {
            languageVersion = JavaLanguageVersion.of(21)
        }
    }

    repositories {
        mavenCentral()
    }

    tasks.withType<JavaCompile> {
        options.encoding = "UTF-8"
        options.release = 21
    }
}
