using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Attributes
{
    public abstract class HttpSessionProcessorAttribute : Attribute, IHttpSessionProcessor
    {
        public abstract Task TryValidateSession(IHttpContext httpContext);
    }
}
