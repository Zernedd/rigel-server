using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using AUnrealFeatures.Hosting.Database.Interfaces;
using Serilog;

namespace AUnrealFeatures.Hosting.Database
{
    public sealed class AstraDatabaseBuilder : IDatabaseBuilder
    {
        private readonly List<SimpleDatabaseDescriptor> _descriptors = new List<SimpleDatabaseDescriptor>();
        private static readonly ILogger _logger = ModuleInitialization.InitializeLogger("AstraDatabaseBuilder");

        public static IDatabaseBuilder Shared => new AstraDatabaseBuilder();

        public IDatabaseBuilder WithDatabase(string name, DatabaseOptions databaseOptions)
        {
            if (!_descriptors.Any(x => x.name == name))
            {
                _descriptors.Add(new SimpleDatabaseDescriptor
                {
                    databaseId = databaseOptions.databaseId,
                    name = name,
                    path = Path.Combine(Environment.CurrentDirectory, $"{name}.db"),
                    password = databaseOptions.usePassword ? databaseOptions.password : null
                });
                return this;
            }

            _logger.Warning("Duplicate entry found for '{Name}', cannot add another database.", name);
            return this;
        }

        public IDatabaseManager Build() => new AstraDatabaseManager(_descriptors);
    }
}
