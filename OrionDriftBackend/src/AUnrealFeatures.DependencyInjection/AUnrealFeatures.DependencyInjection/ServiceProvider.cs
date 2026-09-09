using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.DependencyInjection
{
    public interface IServiceProvider
    {

    }

    public abstract class ServiceProvider : IServiceProvider
    {
        public abstract T GetService<T>();
        public abstract object GetService(Type serviceType);
    }
}
