using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Interfaces
{
    public interface IHttpEndpointParameters
    {
        IHttpRequest Request { get; }
        IHttpResponse Response { get; }
        IHttpSession Session { get; }
    }
}
