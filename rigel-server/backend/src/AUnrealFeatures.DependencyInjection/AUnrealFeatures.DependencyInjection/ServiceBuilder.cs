using System;
using System.Collections.Generic;
using System.Formats.Asn1;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.DependencyInjection
{
    public interface IServiceBuilder
    {
        void AddSingleton<TClass, TInterface>([Optional] TClass? implementationObject) where TClass : class;
        void AddSingleton(Type classType, Type interfaceType, [Optional] object? implementationObject);
    }

    public abstract class ServiceBuilder : IServiceBuilder
    {
        public abstract IReadOnlyList<object> Services { get; }

        public abstract void AddSingleton<TClass, TInterface>([Optional] TClass? implementationObject) where TClass : class;
        public abstract void AddSingleton(Type classType, Type interfaceType, [Optional] object? implementationObject);
    }
}
