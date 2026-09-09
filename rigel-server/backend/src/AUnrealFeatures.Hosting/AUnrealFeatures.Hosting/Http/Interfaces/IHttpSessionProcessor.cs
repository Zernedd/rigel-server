using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Interfaces
{
    public interface IHttpSessionProcessor
    {
        Task TryValidateSession(IHttpContext httpContext);
    }
}
