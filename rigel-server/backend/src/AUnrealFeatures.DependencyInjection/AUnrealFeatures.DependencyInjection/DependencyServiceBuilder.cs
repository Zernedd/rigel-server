using System;
using System.Collections.Generic;
using System.Formats.Asn1;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.DependencyInjection
{
    public abstract class DependencyServiceBuilderBase : ServiceBuilder
    {
        public abstract DependencyServiceProvider Build();
    }

    public sealed class DependencyServiceBuilder : DependencyServiceBuilderBase
    {
        private List<DependencyService> _services;
        private DependencyServiceProvider _parent;

        public override IReadOnlyList<DependencyService> Services => _services;

        public DependencyServiceBuilder([Optional, DefaultParameterValue(null)] DependencyServiceProvider? parent)
        {
            _services = new List<DependencyService>();
            _parent = parent!;
        }

        private void InternalAddSingleton(Type classType, Type interfaceType, [Optional] object? implementationObject)
        {
            if (_services.Any(x => x.Class == classType))
                return;

            _services.Add(DependencyService.Create(classType, interfaceType, implementationObject));
        }

        public override void AddSingleton<TClass, TInterface>([Optional] TClass? implementationObject) where TClass : class
        {
            if (!typeof(TClass).IsClass || !typeof(TInterface).IsInterface)
                return;
            InternalAddSingleton(typeof(TClass), typeof(TInterface), implementationObject);
        }

        public override void AddSingleton(Type classType, Type interfaceType, [Optional] object? implementationObject)
        {
            if (!classType.IsClass || !interfaceType.IsInterface)
                return;
            InternalAddSingleton(classType, interfaceType, implementationObject);
        }

        public override DependencyServiceProvider Build()
            => new DependencyServiceProvider(this, _parent);
    }
}
