using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.WebSockets.Interfaces
{
    public interface IWebSocketRoute
    {
        Task<bool> Validate(IWebSocketClient socketClient);

        string EndpointName { get; }
        string RouteUri { get; }
        IReadOnlyList<IWebSocketRouteProcessor> Processors { get; }
        MethodInfo MethodInfo { get; }
    }
}
