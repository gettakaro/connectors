using System.Collections.Generic;
using System.Reflection;
using Takaro.Commands.Contract;

namespace Takaro.Services
{
    /// <summary>Logs <see cref="PrismaCoreDetection.Warning"/> once per process.</summary>
    public static class PrismaCoreConflict
    {
        private static bool _checked;

        public static void WarnIfLoaded()
        {
            if (_checked)
                return;
            _checked = true;

            foreach (Mod mod in ModManager.GetLoadedMods())
            {
                var assemblyNames = new List<string>();
                if (mod.AllAssemblies != null)
                {
                    foreach (Assembly assembly in mod.AllAssemblies)
                        assemblyNames.Add(assembly.GetName().Name);
                }
                if (PrismaCoreDetection.IsPrismaCore(mod.Name, mod.FolderName, assemblyNames))
                {
                    LogService.Instance.Warn($"{PrismaCoreDetection.Warning} (mod '{mod.Name}')");
                    return;
                }
            }
        }
    }
}
