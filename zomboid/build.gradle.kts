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
        // Project Zomboid B42 ships Zulu OpenJDK 25 and projectzomboid.jar is
        // compiled to class-file v69, which a JDK 21 javac cannot READ. The
        // build therefore runs on a JDK 25 toolchain. Bytecode target is still
        // 21 by default (core stays a portable sibling of minecraft/core); the
        // agent overrides this to 25 because it links against the v69 game jar.
        toolchain {
            languageVersion = JavaLanguageVersion.of(25)
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
