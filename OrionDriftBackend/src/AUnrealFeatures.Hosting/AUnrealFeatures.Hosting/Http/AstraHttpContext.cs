using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public sealed class AstraHttpContext : IHttpContext
    {
        private AstraHttpContext(IHttpRequest request, IHttpResponse response, IHttpSession session)
        {
            Request = request;
            Response = response;
            Session = session;
        }

        public static IHttpContext New(HttpListenerContext httpListenerContext)
        {
            var request = AstraHttpRequest.New(httpListenerContext.Request);
            var response = AstraHttpResponse.New(httpListenerContext.Response);
            var session = AstraHttpSession.New(
                Guid.NewGuid().ToString(),
                "Default",
                DateTime.UtcNow.AddMinutes(30),
                new List<string>(),
                new Dictionary<string, string>()
            );

            return new AstraHttpContext(request, response, session);
        }

        public IHttpRequest Request { get; internal set; } = null!;
        public IHttpResponse Response { get; internal set; } = null!;
        public IHttpSession Session { get; internal set; } = null!;
    }
}