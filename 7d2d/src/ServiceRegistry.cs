using System;
using System.Collections.Generic;
using Takaro.Interfaces;
using Takaro.Persistence;
using Takaro.Services;

namespace Takaro
{
    public static class ServiceRegistry
    {
        private static readonly Lazy<List<IService>> _services = new Lazy<List<IService>>(
            () =>
                new List<IService>
                {
                    // Init order matters: later services depend on earlier ones.
                    LogService.Instance,
                    Database.Instance,
                    DbWriter.Instance,
                    StateMirror.Instance,
                    MainThreadDispatcher.Instance,
                    PositionSampler.Instance,
                }
        );
        private static List<IService> Services => _services.Value;

        public static void InitServices()
        {
            foreach (IService service in Services)
                try
                {
                    service.OnInit();
                    LogService.Instance.Info($"{service.GetType().Name} Initialized");
                }
                catch (Exception ex)
                {
                    LogService.Instance.Error($"Error in {service.GetType().Name}.OnInit");
                    LogService.Instance.Error($"Message: {ex.Message}");
                }
        }

        public static void DestroyServices()
        {
            // Reverse order: DbWriter flushes its queue before Database closes.
            for (int i = Services.Count - 1; i >= 0; i--)
                try
                {
                    Services[i].OnDestroy();
                }
                catch (Exception ex)
                {
                    LogService.Instance.Error(
                        $"Error in {Services[i].GetType().Name}.OnDestroy: {ex.Message}"
                    );
                }
        }
    }
}
