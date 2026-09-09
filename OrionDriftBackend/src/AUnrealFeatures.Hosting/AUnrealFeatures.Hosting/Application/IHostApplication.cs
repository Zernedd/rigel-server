using AUnrealFeatures.DependencyInjection;
using AUnrealFeatures.Hosting.Http;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Application
{
    public interface IHostApplication
    {
        IHostApplication ConfigureServices(Action<IServiceBuilder> onConfigureServicesAction);
        IHostApplication AddServer<TInterface, TServer>() where TServer : class, IStartStopObject;
        object[] PopulateArguments(MethodInfo methodInfo, object[] args);

        Task RunAsync();
    }
}
