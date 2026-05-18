using System;
using System.Collections.Generic;
using Takaro.Interfaces;
using Takaro.Services;

namespace Takaro
{
    public static class ServiceRegistry
    {
        private static readonly Lazy<List<IService>> _services = new Lazy<List<IService>>(() => new List<IService>
        {
            // Register new Services here
            LogService.Instance,
        });
        private static IEnumerable<IService> Services => _services.Value;
        
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
    }

}