using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Controllers.Attributes
{
    public sealed class HttpControllerAttribute : Attribute
    {
        public HttpControllerAttribute(string uri = "/")
        {
            Uri = uri;
        }

        public string Uri { get; } = "/";
    }
}
