using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Binding.Attributes
{
    public sealed class FromFormAttribute : Attribute
    {
        public FromFormAttribute(string name = "") 
        {
            Name = name;
        }

        public string Name { get; private set; } = "";
    }
}
