using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Attributes
{
    public abstract class HttpEndpointAttribute : Attribute
    {
        public HttpEndpointAttribute(HttpMethod httpMethod, string uri)
        {
            Method = httpMethod;
            Uri = uri;
        }

        public HttpMethod Method { get; } = HttpMethod.Get;
        public string Uri { get; } = "/";
    }

    public sealed class HttpGetAttribute : HttpEndpointAttribute
    {
        public HttpGetAttribute(string uri = "/") : base(HttpMethod.Get, uri) { }
    }

    public sealed class HttpPostAttribute : HttpEndpointAttribute
    {
        public HttpPostAttribute(string uri = "/") : base(HttpMethod.Post, uri) { }
    }

    public sealed class HttpPutAttribute : HttpEndpointAttribute
    {
        public HttpPutAttribute(string uri = "/") : base(HttpMethod.Put, uri) { }
    }

    public sealed class HttpDeleteAttribute : HttpEndpointAttribute
    {
        public HttpDeleteAttribute(string uri = "/") : base(HttpMethod.Delete, uri) { }
    }

    public sealed class HttpHeadAttribute : HttpEndpointAttribute
    {
        public HttpHeadAttribute(string uri = "/") : base(HttpMethod.Head, uri) { }
    }

    public sealed class HttpPatchAttribute : HttpEndpointAttribute
    {
        public HttpPatchAttribute(string uri = "/") : base(HttpMethod.Patch, uri) { }
    }
}
