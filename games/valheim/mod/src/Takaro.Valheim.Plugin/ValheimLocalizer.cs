#if TAKARO_VALHEIM_PLUGIN
using System.Reflection;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// Resolves Valheim "$token" names to English display names through the game's own
/// Localization singleton (assembly_guiutils), found by reflection so the plugin does not need
/// a compile-time reference. Results are cached; a missing translation falls back to a readable
/// form of the code.
/// </summary>
internal static class ValheimLocalizer
{
    private static readonly Dictionary<string, string> Cache = new(StringComparer.Ordinal);
    private static readonly object SyncRoot = new();
    private static MethodInfo? localize;
    private static Func<object?>? instance;
    private static bool resolved;

    public static string Localize(string? token, string fallback)
    {
        if (string.IsNullOrWhiteSpace(token))
        {
            return ValheimDisplayName.FromCode(fallback);
        }

        var key = token!.Trim();
        if (!key.StartsWith("$", StringComparison.Ordinal))
        {
            // Some prefabs carry their bare code as the name (TrainingDummy); that is no
            // translation either.
            if (string.Equals(key, fallback, StringComparison.Ordinal))
            {
                return ValheimDisplayName.FromCode(key);
            }

            // Free text can embed tokens ("Big $item_snowball"); the game translates each one.
            var text = key.Contains('$') ? TryLocalize(key) : key;
            return text is null || text.Contains('$')
                ? ValheimDisplayName.FromCode(fallback)
                : ValheimDisplayName.StripRichText(text, ValheimDisplayName.FromCode(fallback));
        }

        lock (SyncRoot)
        {
            if (Cache.TryGetValue(key, out var cached))
            {
                return cached;
            }
        }

        var value = TryLocalize(key);
        var result = value is null || string.IsNullOrWhiteSpace(value) || value.Contains('$') || value.Contains("MISSING") || value.StartsWith("[", StringComparison.Ordinal)
            ? ValheimDisplayName.FromToken(key, ValheimDisplayName.FromCode(fallback))
            : ValheimDisplayName.StripRichText(value, ValheimDisplayName.FromCode(fallback));

        lock (SyncRoot)
        {
            Cache[key] = result;
        }

        return result;
    }

    private static string? TryLocalize(string token)
    {
        try
        {
            if (!resolved)
            {
                resolved = true;
                var type = AppDomain.CurrentDomain.GetAssemblies()
                    .Select(assembly => assembly.GetType("Localization", throwOnError: false))
                    .FirstOrDefault(candidate => candidate is not null);
                var property = type?.GetProperty("instance", BindingFlags.Public | BindingFlags.Static);
                localize = type?.GetMethod("Localize", BindingFlags.Public | BindingFlags.Instance, null, new[] { typeof(string) }, null);
                if (property is not null)
                {
                    instance = () => property.GetValue(null);
                }
            }

            var target = instance?.Invoke();
            return target is null || localize is null ? null : localize.Invoke(target, new object[] { token }) as string;
        }
        catch
        {
            return null;
        }
    }
}
#endif
