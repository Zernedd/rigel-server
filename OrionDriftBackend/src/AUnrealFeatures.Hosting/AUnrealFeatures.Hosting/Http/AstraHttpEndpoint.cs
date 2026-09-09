using AUnrealFeatures.Hosting.Http.Controllers.Interfaces;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Hosting.Http.Preprocessors;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public sealed class AstraHttpEndpoint : IHttpEndpoint
    {
        public HttpMethod Method { get; internal set; } = HttpMethod.Get;
        public string EndpointName { get; set; } = "Unknown";
        public string RouteUri { get; internal set; } = "/";
        public IReadOnlyList<IHttpEndpointProcessor> Processors { get; set; } = new List<IHttpEndpointProcessor>();
        public MethodInfo MethodInfo { get; internal set; } = null!;
        public IHttpController? ControllerInstance { get; internal set; } = null;
        public IHttpRequestPreprocessor? PreprocessorInstance { get; set; } = null;

        public async Task<bool> Validate(IHttpContext httpContext)
        {
            if (Processors.Count == 0) 
                return true;

            for (int i = 0; i < Processors.Count; i++)
            {
                if (await Processors[i].Validate(httpContext))
                    return false;
            }    
            return true;
        }
    }
}
