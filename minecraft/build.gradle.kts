plugins {
    java
}

allprojects {
    version = findProperty("version")?.toString()?.takeIf { it != "unspecified" } ?: "1.0.0-SNAPSHOT"
}

subprojects {
    apply(plugin = "java")

    base {
        archivesName = "takaro-${project.name}"
    }

    java {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    repositories {
        mavenCentral()
    }

    tasks.withType<JavaCompile> {
        options.encoding = "UTF-8"
    }

    tasks.withType<ProcessResources> {
        filesMatching(listOf("plugin.yml", "fabric.mod.json", "META-INF/neoforge.mods.toml")) {
            expand("version" to project.version)
        }
    }
}
