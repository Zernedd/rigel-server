using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Controllers;
using AUnrealFeatures.Hosting.Http.Controllers.Attributes;
using AUnrealFeatures.Hosting.Http.Controllers.Interfaces;
using AUnrealFeatures.Hosting.Http.Interfaces;
using Serilog;
using Serilog.Core;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public abstract partial class AstraHttpServer : IHttpServer, IStartStopObject
    {
        private void ScanForServerBasedEndpoints(Type? targetType = null)
        {
            var endpointMethods = (targetType ?? GetType()).GetAllMethodsWithAttribute<HttpEndpointAttribute>();
            foreach (var endpointMethodInfo in endpointMethods)
            {
                var httpEndpointAttr = endpointMethodInfo.GetCustomAttribute<HttpEndpointAttribute>()
                    ?? throw new InvalidOperationException();

                _endpoints.Add(new AstraHttpEndpoint
                {
                    Method = httpEndpointAttr.Method,
                    EndpointName = endpointMethodInfo.Name,
                    RouteUri = string.Format("/{0}", httpEndpointAttr.Uri.Trim('/')),
                    Processors = endpointMethodInfo.GetCustomAttributes<HttpProcessorAttribute>().ToList(),
                    MethodInfo = endpointMethodInfo
                });
            }
        }

        private void ScanForControllerBasedEndpoints(Type? targetType = null)
        {
            var allControllerTypes = GetAllControllerTypes(targetType);
            foreach (var controllerType in allControllerTypes)
            {
                ProcessController(controllerType);
            }
        }

        private IEnumerable<Type> GetAllControllerTypes(Type? targetType)
        {
            var assemblyControllers = Assembly.GetEntryAssembly()?.GetAllTypesWithAttribute<HttpControllerAttribute>() ?? Enumerable.Empty<Type>();
            var nestedControllers = (targetType ?? GetType()).GetAllNestedTypesWithAttribute<HttpControllerAttribute>();
            return assemblyControllers.Concat(nestedControllers);
        }

        private void ProcessController(Type controllerType)
        {
            var httpControllerAttr = controllerType.GetCustomAttribute<HttpControllerAttribute>()
                ?? throw new InvalidOperationException($"HttpControllerAttribute not found on {controllerType.Name}");
            var httpParentToAttr = controllerType.GetCustomAttribute<HttpParentToAttribute>();

            if (httpParentToAttr != null && httpParentToAttr.Type != GetType()) return;

            var controllerEndpointMethods = controllerType.GetAllMethodsWithAttribute<HttpEndpointAttribute>();
            var logger = ModuleInitialization.InitializeLogger(controllerType.GetSafeName());
            var controllerInstance = CreateControllerInstance(controllerType, logger);

            foreach (var endpointMethodInfo in controllerEndpointMethods)
            {
                ProcessEndpoint(controllerType, httpControllerAttr, endpointMethodInfo, controllerInstance);
            }
        }

        private AstraHttpController CreateControllerInstance(Type controllerType, ILogger logger)
        {
            var controllerInstance = (AstraHttpController?)Activator.CreateInstance(controllerType)
                ?? throw new InvalidOperationException($"Could not instantiate '{controllerType.Name}'. Is there a problem with the constructor taking in an IHttpServer and ILogger?");
            controllerInstance.OnCreated(this, logger);
            return controllerInstance;
        }

        private void ProcessEndpoint(Type controllerType, HttpControllerAttribute httpControllerAttr, MethodInfo endpointMethodInfo, AstraHttpController controllerInstance)
        {
            var httpEndpointAttr = endpointMethodInfo.GetCustomAttribute<HttpEndpointAttribute>()
                ?? throw new InvalidOperationException($"HttpEndpointAttribute not found on {endpointMethodInfo.Name} in {controllerType.Name}");

            _endpoints.Add(new AstraHttpEndpoint
            {
                Method = httpEndpointAttr.Method,
                EndpointName = endpointMethodInfo.Name,
                RouteUri = BuildRouteUri(httpControllerAttr.Uri, httpEndpointAttr.Uri),
                Processors = endpointMethodInfo.GetCustomAttributes<HttpProcessorAttribute>().ToList(),
                MethodInfo = endpointMethodInfo,
                ControllerInstance = controllerInstance
            });
        }

        private string BuildRouteUri(string controllerUri, string endpointUri) => $"/{controllerUri.Trim('/')}/{endpointUri.Trim('/')}".Replace("//", "/");
    }
}
