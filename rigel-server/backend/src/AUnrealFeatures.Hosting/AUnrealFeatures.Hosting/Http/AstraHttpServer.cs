using AUnrealFeatures.Hosting.Application;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Binding;
using AUnrealFeatures.Hosting.Http.Controllers.Interfaces;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Hosting.Http.Preprocessors;
using AUnrealFeatures.Hosting.SDK;
using Serilog;
using Serilog.Core;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public abstract partial class AstraHttpServer : IHttpServer, IHttpEndpointParameters, IStartStopObject
    {
        private bool _initialized;
        private HttpListener _httpListener;

        private IHttpContext _httpContext;
        private IHttpSessionProcessor? _sessionProcessor = null!;
        private readonly List<IHttpRequestPreprocessor> _preprocessors;
        private readonly List<AstraHttpEndpoint> _endpoints;
        private Task _listenTask = null!;

        private readonly ILogger _logger;
        public ILogger Logger => _logger;

        public AstraHttpServer(string hostname, ushort port)
        {
            _logger = ModuleInitialization.InitializeLogger(GetType().Name);

            Hostname = hostname;
            Port = port;

            _preprocessors = new List<IHttpRequestPreprocessor>();
            _endpoints = new List<AstraHttpEndpoint>();

            _httpListener = new HttpListener();
            // Bind on all interfaces (not just loopback) so external clients can reach it.
            _httpListener.Prefixes.Add(string.Format("http://+:{0}/", Port));

            Start();
        }

        public void Start()
        {
            if (!_initialized)
            {
                ScanForServerBasedEndpoints();
                ScanForControllerBasedEndpoints();

                _sessionProcessor = (HttpSessionProcessorAttribute?)GetType().GetCustomAttribute(typeof(HttpSessionProcessorAttribute), true)
                    ?? null;
                _initialized = true;
            }

            if (!_httpListener.IsListening)
            {
                _httpListener.Start();
                _listenTask = Task.Factory.StartNew(ListenImpl, TaskCreationOptions.LongRunning);
                _logger.Information("Started HTTP server on '{Host}:{Port}'", Hostname, Port);
            }
            else _logger.Warning("Cannot start HTTP server when it is already listening!");
        }

        /// <summary>
        /// Override in subclasses to handle WebSocket upgrade requests.
        /// Return true if the request was consumed (do NOT close the response).
        /// Return false to fall through to the normal HTTP pipeline (not recommended for WS requests).
        /// </summary>
        protected virtual Task<bool> HandleWebSocketAsync(HttpListenerContext rawContext) => Task.FromResult(false);

        private async Task ListenImpl()
        {
            while (_httpListener.IsListening)
            {
                var rawHttpContext = await _httpListener.GetContextAsync();

                // Intercept WebSocket upgrade requests before the HTTP pipeline touches the response.
                if (rawHttpContext.Request.IsWebSocketRequest)
                {
                    _ = Task.Factory.StartNew(async () =>
                    {
                        try { await HandleWebSocketAsync(rawHttpContext); }
                        catch (Exception ex) { _logger.Error("WebSocket error: {Message}", ex.Message); }
                    }, TaskCreationOptions.LongRunning);
                    continue;
                }

                var context = AstraHttpContext.New(rawHttpContext);

                try
                {
                    _logger.Information("{IpAddress} {HttpMethod} {Uri}", context.Request.Remote, context.Request.Method, rawHttpContext.Request.Url!.PathAndQuery);

                    if (_sessionProcessor != null)
                        await _sessionProcessor.TryValidateSession(context);

                    bool continueWithRequest = true;
                    foreach (var preprocessor in _preprocessors)
                    {
                        var result = await preprocessor.TryPreprocessRequest(context.Request, context.Response);
                        continueWithRequest = !result.result.HasFlag(HttpPreprocessorResult.STOP_AFTER);

                        if (!continueWithRequest)
                            context.Response.ApplyToHttpListenerResponse(result.actionResult);
                    }

                    context.Response.EnableCors();

                    if (context.Request.Method.Method == "OPTIONS")
                    {
                        context.Response.ApplyToHttpListenerResponse(Results.NoContent());
                    }
                    else if (continueWithRequest)
                    {
                        IHttpActionResult actionResult;
                        using (var scopedReference = ScopedReference<IHttpContext>.New(ref _httpContext))
                        {
                            scopedReference.SetValue(context);
                            actionResult = await ProcessRequest(context);
                        }

                        context.Response.ApplyToHttpListenerResponse(actionResult);
                    }
                }
                catch (Exception ex)
                {
                    _logger.Error("Error processing request: {Message}", ex.Message);
                    context.Response.ApplyToHttpListenerResponse(
                        Results.HtmlDocument(
                            HttpStatusCode.InternalServerError,
                            HtmlDocumentCache.InternalServerErrorDocumentString.Replace("{0}", ex.Message)
                            ));
                }
                finally { rawHttpContext.Response.Close(); }

                await Task.Delay(1);
            }
        }

        private async Task<IHttpActionResult> ProcessRequest(IHttpContext context)
        {
            var endpoint = FindMatchingEndpoint(context.Request);

            if (endpoint == null) return Results.NotFound();
            if (endpoint.Method != context.Request.Method) return Results.MethodNotAllowed();

            try
            {
                foreach (var processor in endpoint.Processors)
                    if (!await processor.Validate(context))
                        return Results.ExpectationFailed(string.Format("The processor '{0}' rejected your request.", processor.GetType().GetSafeName()));

                var args = await BindableExtensions.BindParameters(endpoint.MethodInfo, context);
                if (HostApplication.Instance != null)
                {
                    args = HostApplication.Instance.PopulateArguments(endpoint.MethodInfo, args);
                }

                if (IsDynamicRoute(endpoint.RouteUri))
                {
                    var routeParams = ExtractRouteParameters(endpoint.RouteUri, context.Request.Uri);
                    var parameters = endpoint.MethodInfo.GetParameters();
                    for (int i = 0; i < parameters.Length; i++)
                    {
                        var param = parameters[i];
                        if (routeParams.ContainsKey(param.Name!))
                            args[i] = Convert.ChangeType(routeParams[param.Name!], param.ParameterType);
                    }
                }

                var result = (Task<IHttpActionResult>)endpoint.MethodInfo.Invoke(
                    endpoint.ControllerInstance ?? endpoint.PreprocessorInstance ?? (object)this, 
                    args)!;
                if (result is Task<IHttpActionResult> resultTask && resultTask != null)
                {
                    await resultTask;
                    return resultTask.Result;
                }

                if (result is IHttpActionResult actionResult && actionResult != null)
                    return actionResult;
                return Results.NoContent();
            }
            catch (Exception ex)
            {
                return Results.HtmlDocument(
                    HttpStatusCode.InternalServerError,
                    HtmlDocumentCache.InternalServerErrorDocumentString.Replace("{0}", string.Format("Error while processing {0}<br><br>{1}", endpoint.EndpointName, ex.ToString()))
                    );
            }
        }

        private bool IsDynamicRoute(string routeUri) => routeUri.Contains("{") && routeUri.Contains("}");

        private AstraHttpEndpoint? FindMatchingEndpoint(IHttpRequest request)
        {
            return _endpoints.FirstOrDefault(e => IsRouteMatch(e.RouteUri, request.Uri) && e.Method == request.Method, null);
        }

        private bool IsRouteMatch(string routePattern, string requestPath)
        {
            var pattern = "^" + Regex.Escape(routePattern)
                .Replace("\\{", "{")
                .Replace("}", "}")
                .Replace("{*}", "(.*)")
                .Replace("{", "(?<")
                .Replace("}", ">[^/]+)") + "$";
            return Regex.IsMatch(requestPath, pattern);
        }

        private Dictionary<string, string> ExtractRouteParameters(string routePattern, string requestPath)
        {
            var pattern = "^" + Regex.Escape(routePattern)
                .Replace("\\{", "{")
                .Replace("}", "}")
                .Replace("{*}", "(.*)")
                .Replace("{", "(?<")
                .Replace("}", ">[^/]+)") + "$";

            var match = Regex.Match(requestPath, pattern);
            var result = new Dictionary<string, string>();
            if (match.Success)
            {
                var groupNames = Regex.Matches(routePattern, "{([^}]+)}")
                    .Cast<Match>()
                    .Select(m => m.Groups[1].Value);

                foreach (var name in groupNames)
                    result[name] = match.Groups[name].Value;
            }
            return result;
        }

        public void Stop()
        {
            if (!_initialized)
                return;

            if (_httpListener.IsListening)
            {
                _httpListener.Stop();
                _listenTask = null!;
                _logger.Information("Stopped HTTP server");
            }
            else _logger.Warning("Cannot stop HTTP server when it is not listening!");
        }

        public void AddPreprocessor<TProcessor>(params object[] arguments) where TProcessor : IHttpRequestPreprocessor
        {
            if (!_preprocessors.Any(x => x.GetType() == typeof(TProcessor)))
                _preprocessors.Add((TProcessor)Activator.CreateInstance(typeof(TProcessor), arguments)!);
        }

        public void RemovePreprocessor<TProcessor>() where TProcessor : IHttpRequestPreprocessor
        {
            _preprocessors.RemoveAll(x => x.GetType() == typeof(TProcessor));
        }

        public IHttpEndpoint AddEndpoint(HttpMethod httpMethod, string endpoint, Delegate method, IHttpController? controllerInstance = null, IHttpRequestPreprocessor? preprocessorInstance = null)
        {
            var endpointInst = new AstraHttpEndpoint
            {
                Method = httpMethod,
                EndpointName = method.Method.Name,
                RouteUri = endpoint,
                MethodInfo = method.Method,
                ControllerInstance = controllerInstance,
                PreprocessorInstance = preprocessorInstance
            };

            _endpoints.Add(endpointInst);
            return endpointInst;
        }

        public IHttpRequest Request => _httpContext.Request;
        public IHttpResponse Response => _httpContext.Response;
        public IHttpSession Session => _httpContext.Session;

        public string Hostname { get; } = "localhost";
        public ushort Port { get; } = 80;
        public IReadOnlyList<IHttpEndpoint> Endpoints => _endpoints;
    }
}
