using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.SDK
{
    public sealed class ScopedReference<T> : IDisposable
    {
        private T _value;
        private ScopedReference(ref T value)
        {
            _value = value;
        }

        public static ScopedReference<T> New(ref T value)
            => new ScopedReference<T>(ref value);

        public void SetValue(T value) => _value = value;
        public T GetValue() => _value;

        public void Dispose() => _value = default;
    }
}