using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Controllers.Attributes
{
    public sealed class HttpParentToAttribute : Attribute
    {
        public HttpParentToAttribute(Type type)
        {
            Type = type;
        }

        public Type Type { get; private set; }
    }
}
