using AUnrealFeatures.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Application
{
    public sealed class HostApplication : IHostApplication
    {
        public static HostApplication Instance { get; private set; }

        private readonly string _name;
        private readonly IServiceHost _serviceHost;

        private ServiceProvider _serviceProvider = null!;
        internal static ServiceProvider ServiceProvider => Instance._serviceProvider;

        private HostApplication(string name)
        {
            Instance = this;
            _name = name;
            _serviceHost = ServiceHost.Create(name);
        }
        public static IHostApplication New(string name)
            => new HostApplication(name);

        public IHostApplication ConfigureServices(Action<IServiceBuilder> onConfigureServicesAction)
        {
            _serviceHost.AddDependencies(onConfigureServicesAction);
            return this;
        }

        public IHostApplication AddServer<TInterface, TServer>() where TServer : class, IStartStopObject
        {
            _serviceHost.AddDependencies(options => options.AddSingleton<TServer, TInterface>());
            return this;
        }

        public object[] PopulateArguments(MethodInfo methodInfo, object[] args)
        {
            if (args.Length == 0) return args;
            var methodParameters = methodInfo.GetParameters();

            if (methodParameters.Length < args.Length) 
                return args;

            for (int i = 0; i < args.Length; i++)
            {
                var parameter = methodParameters[i];
                var service = _serviceProvider.GetService(parameter.ParameterType);

                if (service != null)
                    args[i] = service;
            }

            return args;
        }

        public async Task RunAsync()
        {
            _serviceProvider = _serviceHost.GetServiceProvider();
            await Task.Delay(-1);
        }
    }
}
