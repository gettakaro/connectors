using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Newtonsoft.Json;

namespace Takaro.Services
{
    /// <summary>One getItemIcons answer entry: a PNG and the hash it was requested under.</summary>
    public class ItemIconEntry
    {
        [JsonProperty("hash")]
        public string Hash { get; set; }

        [JsonProperty("data")]
        public string Data { get; set; }
    }

    /// <summary>
    /// The PNG icons the dedicated server holds on disk, by the icon name items report
    /// (ItemClass.GetIconName). Holds hashes and paths only, never image bytes, and reads
    /// files only from the threads that call Build and Serve, neither of them the game thread.
    /// </summary>
    public sealed class ItemIconCatalog
    {
        public const int MaxBatchHashes = 100;
        public const int MaxResponseBytes = 1024 * 1024;

        private const int HashHexLength = 64;

        private static readonly byte[] PngSignature = { 137, 80, 78, 71, 13, 10, 26, 10 };

        private static volatile ItemIconCatalog _current = new ItemIconCatalog(
            new Dictionary<string, Icon>(StringComparer.OrdinalIgnoreCase)
        );

        /// <summary>The catalogue requests read; empty until the startup build replaces it.</summary>
        public static ItemIconCatalog Current
        {
            get => _current;
            set => _current = value;
        }

        private sealed class Icon
        {
            public string Hash;
            public string Path;
        }

        private readonly Dictionary<string, Icon> _iconsByName;
        private readonly Dictionary<string, string> _pathsByHash;

        private ItemIconCatalog(Dictionary<string, Icon> iconsByName)
        {
            _iconsByName = iconsByName;
            _pathsByHash = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (Icon icon in iconsByName.Values)
                _pathsByHash[icon.Hash] = icon.Path;
        }

        public int IconCount => _iconsByName.Count;

        /// <summary>
        /// The folders the game itself reads item art from, lowest precedence first:
        /// the vanilla icons, then each loaded mod in load order, its UI atlas last
        /// because that is the picture players see.
        /// </summary>
        public static List<string> IconDirectories(
            string gameDirectory,
            IEnumerable<string> modPaths
        )
        {
            var directories = new List<string> { Path.Combine(gameDirectory, "Data", "ItemIcons") };
            foreach (string modPath in modPaths)
            {
                directories.Add(Path.Combine(modPath, "ItemIcons"));
                directories.Add(Path.Combine(modPath, "UIAtlases", "ItemIconAtlas"));
            }
            return directories;
        }

        /// <summary>
        /// Hashes every usable PNG directly inside the folders; a later folder replaces an
        /// earlier icon of the same name. A file that is not a PNG or cannot be served within
        /// the response bound gets no hash, because Takaro would ask for it on every run.
        /// </summary>
        public static ItemIconCatalog Build(IEnumerable<string> directories)
        {
            var icons = new Dictionary<string, Icon>(StringComparer.OrdinalIgnoreCase);
            using (SHA256 sha256 = SHA256.Create())
            {
                foreach (string directory in directories)
                {
                    foreach (string path in PngsIn(directory))
                    {
                        byte[] bytes = ReadUsablePng(path);
                        if (bytes != null)
                            icons[Path.GetFileNameWithoutExtension(path)] = new Icon
                            {
                                Hash = HashHex(sha256, bytes),
                                Path = path,
                            };
                    }
                }
            }
            return new ItemIconCatalog(icons);
        }

        /// <summary>The SHA-256 of the art of this icon name, or null when the server has none.</summary>
        public string HashFor(string iconName)
        {
            if (string.IsNullOrEmpty(iconName))
                return null;
            return _iconsByName.TryGetValue(iconName, out Icon icon) ? icon.Hash : null;
        }

        /// <summary>
        /// Walks the hashes in request order, skipping what cannot be served and stopping
        /// before the entry that would break the response bound; Takaro asks again for the rest.
        /// </summary>
        public List<ItemIconEntry> Serve(IEnumerable<string> hashes)
        {
            var entries = new List<ItemIconEntry>();
            long total = 0;
            using (SHA256 sha256 = SHA256.Create())
            {
                foreach (string hash in hashes)
                {
                    if (!_pathsByHash.TryGetValue(hash, out string path))
                        continue;

                    byte[] bytes = ReadUsablePng(path);
                    // A file replaced since the build hashes differently, and Takaro would
                    // remember the mismatch for good.
                    if (bytes == null || HashHex(sha256, bytes) != hash)
                        continue;
                    if (total + bytes.Length > MaxResponseBytes)
                        break;

                    total += bytes.Length;
                    entries.Add(
                        new ItemIconEntry { Hash = hash, Data = Convert.ToBase64String(bytes) }
                    );
                }
            }
            return entries;
        }

        /// <summary>Null when the request is valid, otherwise why it is refused.</summary>
        public static string ValidateRequest(IList<string> hashes)
        {
            if (hashes == null || hashes.Count < 1 || hashes.Count > MaxBatchHashes)
                return $"getItemIcons hashes must hold 1 to {MaxBatchHashes} entries";
            if (!hashes.All(IsHash))
                return "getItemIcons hashes must be 64 lowercase hex characters";
            return null;
        }

        private static bool IsHash(string value) =>
            value != null
            && value.Length == HashHexLength
            && value.All(c => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));

        private static string[] PngsIn(string directory)
        {
            try
            {
                return Directory.Exists(directory)
                    ? Directory.GetFiles(directory, "*.png")
                    : new string[0];
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                return new string[0];
            }
        }

        private static byte[] ReadUsablePng(string path)
        {
            try
            {
                if (new FileInfo(path).Length > MaxResponseBytes)
                    return null;
                byte[] bytes = File.ReadAllBytes(path);
                return
                    bytes.Length <= MaxResponseBytes
                    && bytes.Take(PngSignature.Length).SequenceEqual(PngSignature)
                    ? bytes
                    : null;
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                return null;
            }
        }

        private static string HashHex(SHA256 sha256, byte[] bytes)
        {
            var hex = new StringBuilder(HashHexLength);
            foreach (byte value in sha256.ComputeHash(bytes))
                hex.Append(value.ToString("x2"));
            return hex.ToString();
        }
    }
}
