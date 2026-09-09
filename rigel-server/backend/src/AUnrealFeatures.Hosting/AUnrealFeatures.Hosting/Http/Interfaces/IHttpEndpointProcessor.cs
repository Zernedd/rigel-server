using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Interfaces
{
    public interface IHttpEndpointProcessor
    {
        Task<bool> Validate(IHttpContext httpContext);
    }
}
