using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.DependencyInjection
{
    public interface ICompositeServiceProvider
    {

    }

    public abstract class CompositeServiceProviderBase : ServiceProvider, ICompositeServiceProvider
    {
    }

    public sealed class CompositeServiceProvider : CompositeServiceProviderBase,  ICompositeServiceProvider
    {
        public CompositeServiceProvider(params DependencyServiceBuilder[] serviceBuilders)
        {
            int count = 0;
            foreach (var builder in serviceBuilders)
                count += builder.Services.Count;

            _services = new List<DependencyService>();
            foreach (var builder in serviceBuilders)
                _services.AddRange(builder.Services);

            foreach (var service in _services)
                service.TryInitialize(this);
        }

        public override T GetService<T>()
        {
            var service = _services.FirstOrDefault(s => s.Class == typeof(T) || s.Interface == typeof(T));
            if (service != null)
            {
                service.TryInitialize(this);
                return (T)service.Instance;
            }

            return default!;
        }

        public override object GetService(Type serviceType)
        {
            var service = _services.FirstOrDefault(s => s.Class == serviceType || s.Interface == serviceType);
            if (service != null)
            {
                service.TryInitialize(this);
                return service.Instance;
            }

            return null;
        }

        private List<DependencyService> _services = new List<DependencyService>();
    }
}
