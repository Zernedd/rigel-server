using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;

namespace AUnrealFeatures.DependencyInjection
{
    public class DependencyService
    {
        public static DependencyService Create(Type classType, Type interfaceType, object? implementationObject)
        {
            return new DependencyService
            {
                _dependencyId = Guid.NewGuid().ToString(),
                _classType = classType,
                _interfaceType = interfaceType,
                _implementationObject = implementationObject
            };
        }

        static TimeSpan _timeout = TimeSpan.FromSeconds(2);
        public void TryInitialize(ServiceProvider serviceProvider)
        {
            if (_instance != null)
                return;

            var constructorInfo = _classType.GetConstructors()
                                            .OrderByDescending(c => c.GetParameters().Length)
                                            .FirstOrDefault();

            if (constructorInfo == null)
            {
                _instance = Activator.CreateInstance(_classType);
                return;
            }
            var constructorParams = constructorInfo.GetParameters();


            // attribute injection?
            if (constructorParams.Length == 0)
            {
                _instance = _implementationObject ?? Activator.CreateInstance(_classType);
                foreach (var field in _classType.GetRuntimeFields())
                {
                    InjectAttribute injectAttribute = null;
                    if ((injectAttribute = field.GetCustomAttribute<InjectAttribute>()!) == null)
                        continue;

                    object service = serviceProvider.GetService(field.FieldType);
                    if (service == null)
                        throw new InvalidOperationException($"Cannot resolve parameter '{field.Name}' of type '{field.FieldType}' for '{_classType.FullName}'.");
                    field.SetValue(_instance, service);
                }
            }
            else if (_implementationObject == null)
            {
                var resolvedParams = new List<object?>();
                foreach (var param in constructorParams)
                {
                    var resolvedParam = serviceProvider.GetService(param.ParameterType);
                    if (resolvedParam == null && !param.IsOptional)
                        throw new InvalidOperationException($"Cannot resolve parameter '{param.Name}' of type '{param.ParameterType}' for '{_classType.FullName}'.");

                    resolvedParams.Add(resolvedParam);
                }
                _instance = Activator.CreateInstance(_classType, resolvedParams.ToArray());
            }
        }

        public string Id => _dependencyId;
        public Type Class => _classType;
        public Type Interface => _interfaceType;
        public object Instance => _instance;

        private string _dependencyId;
        private Type _classType;
        private Type _interfaceType;
        private object _instance;
        private object? _implementationObject;
    }

    public abstract class DependencyServiceProviderBase : ServiceProvider
    {
    }

    public sealed class DependencyServiceProvider : DependencyServiceProviderBase
    {
        public DependencyServiceProvider(DependencyServiceBuilder builder, [Optional, DefaultParameterValue(null)] DependencyServiceProvider? parent)
        {
            _services = new List<DependencyService>(builder.Services);
            _parent = parent!;

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

            return (_parent.GetService<T>() ?? default)!;
        }

        public override object GetService(Type serviceType)
        {
            var service = _services.FirstOrDefault(s => s.Class == serviceType || s.Interface == serviceType);
            if (service != null)
            {
                service.TryInitialize(this);
                return service.Instance;
            }

            return _parent?.GetService(serviceType);
        }

        public DependencyServiceProvider Parent => _parent;

        private DependencyServiceProvider _parent;
        private List<DependencyService> _services = new List<DependencyService>();
    }
}