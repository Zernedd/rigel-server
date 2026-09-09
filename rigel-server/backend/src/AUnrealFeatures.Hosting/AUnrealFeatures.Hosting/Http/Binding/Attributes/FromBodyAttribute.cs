using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Binding.Attributes
{
    public sealed class FromBodyAttribute : Attribute
    {
        public FromBodyAttribute(string name = "")
        {
            Name = name;
        }

        public string Name { get; private set; } = "";
    }
}
