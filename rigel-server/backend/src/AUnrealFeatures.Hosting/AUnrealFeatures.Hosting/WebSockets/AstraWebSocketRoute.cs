using AUnrealFeatures.Hosting.WebSockets.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.WebSockets
{
    public sealed class AstraWebSocketRoute : IWebSocketRoute
    {
        public string EndpointName { get; internal set; } = "Unknown";
        public string RouteUri { get; internal set; } = "/";
        public IReadOnlyList<IWebSocketRouteProcessor> Processors { get; internal set; } = new List<IWebSocketRouteProcessor>();
        public MethodInfo MethodInfo { get; internal set; }

        public async Task<bool> Validate(IWebSocketClient socketClient)
        {
            if (Processors.Count == 0)
                return true;

            for (int i = 0; i < Processors.Count; i++)
            {
                if (await Processors[i].Validate(socketClient))
                    return false;
            }
            return true;
        }
    }
}
