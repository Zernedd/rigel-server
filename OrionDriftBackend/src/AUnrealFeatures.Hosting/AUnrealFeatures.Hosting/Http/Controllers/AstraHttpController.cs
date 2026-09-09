using AUnrealFeatures.Hosting.Http.Controllers.Interfaces;
using AUnrealFeatures.Hosting.Http.Interfaces;
using Serilog;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Controllers
{
    public abstract class AstraHttpController : IHttpController, IHttpEndpointParameters
    {
        public IHttpServer HttpServer => _httpServer;
        public ILogger Logger => _logger;

        private AstraHttpServer _httpServer;
        private ILogger _logger;

        internal void OnCreated(AstraHttpServer httpServer, ILogger logger)
        {
            _httpServer = httpServer ?? throw new ArgumentNullException(nameof(httpServer));
            _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        }

        public virtual void OnLoaded()
        {
        }

        public IHttpRequest Request => _httpServer.Request;
        public IHttpResponse Response => _httpServer.Response;
        public IHttpSession Session => _httpServer.Session;
    }
}
