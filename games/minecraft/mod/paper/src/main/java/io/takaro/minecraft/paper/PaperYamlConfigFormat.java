package io.takaro.minecraft.paper;

import io.takaro.minecraft.core.TakaroConfig;
import io.takaro.minecraft.core.config.ConfigFormat;
import io.takaro.minecraft.core.config.YamlText;
import org.bukkit.configuration.InvalidConfigurationException;
import org.bukkit.configuration.file.YamlConfiguration;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Paper: {@code plugins/TakaroMinecraft/config.yml}, defaults from the config.yml in the jar. */
final class PaperYamlConfigFormat implements ConfigFormat {

    @Override
    public TakaroConfig parse(String text) throws InvalidConfigurationException {
        YamlConfiguration yaml = new YamlConfiguration();
        yaml.loadFromString(text);
        if (!yaml.contains("takaro.authentication.registration_token")) {
            throw new InvalidConfigurationException("takaro.authentication.registration_token is missing");
        }
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl(yaml.getString("takaro.websocket.url", ""));
        config.setIdentityToken(yaml.getString("takaro.authentication.identity_token", ""));
        config.setRegistrationToken(yaml.getString("takaro.authentication.registration_token", ""));
        config.setReconnectEnabled(yaml.getBoolean("takaro.reconnect.enabled", true));
        config.setReconnectDelay(yaml.getLong("takaro.reconnect.delay", 5000));
        config.setMaxReconnectDelay(yaml.getLong("takaro.reconnect.max_delay", 300000));
        config.setBackoffMultiplier(yaml.getDouble("takaro.reconnect.backoff_multiplier", 1.5));
        config.setDebugEnabled(yaml.getBoolean("takaro.debug", false));
        return config;
    }

    @Override
    public String defaultText() {
        try (InputStream in = PaperYamlConfigFormat.class.getResourceAsStream("/config.yml")) {
            if (in == null) {
                throw new IllegalStateException("config.yml is missing from the plugin jar");
            }
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            throw new IllegalStateException("cannot read config.yml from the plugin jar", e);
        }
    }

    @Override
    public String withIdentity(String text, String identity) throws InvalidConfigurationException {
        String edited = YamlText.withIdentity(text, identity);
        if (edited != null) {
            return edited;
        }
        YamlConfiguration yaml = new YamlConfiguration();
        yaml.loadFromString(text);
        yaml.set("takaro.authentication.identity_token", identity);
        return yaml.saveToString();
    }
}
