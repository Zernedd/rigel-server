using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Controllers.Interfaces
{
    public interface IHttpController : IHttpEndpointParameters
    {
        void OnLoaded();
    }
}
