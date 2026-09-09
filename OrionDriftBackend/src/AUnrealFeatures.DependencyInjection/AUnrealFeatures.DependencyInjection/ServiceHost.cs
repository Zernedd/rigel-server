using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.DependencyInjection
{
    public interface IServiceHost 
    {
        IServiceHost AddDependencies(Action<ServiceBuilder> options);
        ServiceProvider GetServiceProvider();
    }

    public abstract class ServiceHostBase : IServiceHost
    {
        public abstract IServiceHost AddDependencies(Action<ServiceBuilder> options);
        public abstract ServiceProvider GetServiceProvider();
    }

    public sealed class ServiceHost : ServiceHostBase, IServiceHost
    {
        private string _hostName;
        private ServiceHost(string hostName)
        {
            this._hostName = hostName;
        }
        public static ServiceHost Create(string hostName)
            => new ServiceHost(hostName);

        public override IServiceHost AddDependencies(Action<ServiceBuilder> options)
        {
            if (_depsBuilder == null)
                _depsBuilder = new DependencyServiceBuilder();

            options.Invoke(_depsBuilder);
            return this;
        }

        public override ServiceProvider GetServiceProvider() 
        {
            if (_serviceProvider != null)
                return _serviceProvider;
            return _serviceProvider = new CompositeServiceProvider(_depsBuilder);
        }

        private CompositeServiceProvider _serviceProvider;
        private DependencyServiceBuilder _depsBuilder;
    }
}
