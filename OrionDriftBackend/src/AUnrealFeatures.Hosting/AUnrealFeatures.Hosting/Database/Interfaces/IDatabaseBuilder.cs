using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database.Interfaces
{
    public struct DatabaseOptions
    {
        public int databaseId;
        public string friendlyName;
        public bool usePassword;
        public string password;
    }

    public interface IDatabaseBuilder
    {
        IDatabaseBuilder WithDatabase(string name, DatabaseOptions databaseOptions);
        IDatabaseManager Build();
    }
}
