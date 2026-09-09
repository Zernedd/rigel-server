using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;

namespace AUnrealFeatures.Hosting
{
    public static class TypeExtensions
    {
        public static string GetSafeName(this Type type)
        {
            string typeName = type.Name;
            string genericParameters = string.Empty;

            if (type.IsGenericType)
            {
                typeName = typeName.Split("`")[0];
                var types = from x in type.GenericTypeArguments
                            select x.Name;

                genericParameters = $"<{string.Join(",", types)}>";
            }

            return $"{typeName}{genericParameters}";
        }

        public static bool InheritsOrImplements(this Type child, Type parent)
        {
            parent = ResolveGenericTypeDefinition(parent);

            var currentChild = child.IsGenericType
                ? child.GetGenericTypeDefinition()
                : child;

            while (currentChild != typeof(object))
            {
                if (parent == currentChild || HasAnyInterfaces(parent, currentChild))
                    return true;

                currentChild = currentChild.BaseType != null
                               && currentChild.BaseType.IsGenericType
                    ? currentChild.BaseType.GetGenericTypeDefinition()
                    : currentChild.BaseType;

                if (currentChild == null)
                    return false;
            }
            return false;
        }

        private static bool HasAnyInterfaces(Type parent, Type child)
        {
            return child.GetInterfaces()
                .Any(childInterface =>
                {
                    var currentInterface = childInterface.IsGenericType
                        ? childInterface.GetGenericTypeDefinition()
                        : childInterface;

                    return currentInterface == parent;
                });
        }

        private static Type ResolveGenericTypeDefinition(Type parent)
        {
            var shouldUseGenericType = true;
            if (parent.IsGenericType && parent.GetGenericTypeDefinition() != parent)
                shouldUseGenericType = false;

            if (parent.IsGenericType && shouldUseGenericType)
                parent = parent.GetGenericTypeDefinition();
            return parent;
        }

        public static bool IsPrimitive(this Type type)
        {
            return type.IsPrimitive || type == typeof(string) || type == typeof(decimal);
        }

        public static List<MethodInfo> GetAllMethodsWithAttribute<TAttribute>(this Type type) where TAttribute : Attribute
        {
            return type.GetMethods()
                .Where(method => method.GetCustomAttributes(typeof(TAttribute), true).Any())
                .ToList();
        }

        public static List<Type> GetAllNestedTypesWithAttribute<TAttribute>(this Type type) where TAttribute : Attribute
        {
            return type.GetNestedTypes()
                .Where(method => method.GetCustomAttributes(typeof(TAttribute), true).Any())
                .ToList();
        }

        public static List<Type> GetAllTypesWithAttribute<TAttribute>(this Assembly assembly) where TAttribute : Attribute
        {
            return assembly.GetTypes()
                .SelectMany(t => new[] { t }.Concat(t.GetNestedTypes()))
                .Where(method => method.GetCustomAttributes(typeof(TAttribute), true).Any())
                .ToList();
        }
    }
}
