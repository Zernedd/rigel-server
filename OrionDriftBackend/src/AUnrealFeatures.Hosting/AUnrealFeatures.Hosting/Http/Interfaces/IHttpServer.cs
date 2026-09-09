using AUnrealFeatures.Hosting.Http.Controllers.Interfaces;
using AUnrealFeatures.Hosting.Http.Preprocessors;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Interfaces
{
    public interface IHttpServer : IStartStopObject
    {
        void Start();
        void Stop();

        void AddPreprocessor<TProcessor>(params object[] arguments) where TProcessor : IHttpRequestPreprocessor;
        void RemovePreprocessor<TProcessor>() where TProcessor : IHttpRequestPreprocessor;

        IHttpEndpoint AddEndpoint(HttpMethod httpMethod, string endpoint, Delegate method, IHttpController? controllerInstance = null, IHttpRequestPreprocessor? preprocessorInstance = null);

        string Hostname { get; }
        ushort Port { get; }
        IReadOnlyList<IHttpEndpoint> Endpoints { get; }
    }
}
