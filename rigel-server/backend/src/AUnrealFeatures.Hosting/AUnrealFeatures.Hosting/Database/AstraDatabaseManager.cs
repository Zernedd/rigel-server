using AUnrealFeatures.Hosting.Database.Interfaces;
using Serilog;
using Serilog.Core;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database
{
    public class SimpleDatabaseDescriptor
    {
        public int databaseId;
        public string name;
        public string path;
        public string password;
    }

    public sealed class AstraDatabaseManager : IDatabaseManager
    {
        private readonly List<SimpleDatabaseDescriptor> _descriptors;
        private readonly List<IDatabase> _cachedDatabases;

        private static readonly ILogger _logger = ModuleInitialization.InitializeLogger("AstraDatabaseManager");

        public AstraDatabaseManager(List<SimpleDatabaseDescriptor> descriptors)
        {
            if (descriptors.Count == 0)
                _logger.Warning("The 'descriptors' field has 0 elements! This means no databases are currently being managed. Is this supposed to happen?");

            _descriptors = descriptors;
            _cachedDatabases = new List<IDatabase>();
        }

        public IDatabase? GetDatabase([Optional] string name, [Optional] int id)
        {
            if (string.IsNullOrEmpty(name) && id == 0)
                throw new ArgumentNullException("You cannot get a database when 'name' or 'id' are not set.");

            var databaseDescriptor = _descriptors.FirstOrDefault(
                x => x?.databaseId == id || x?.name == name,
                null
            );

            if (databaseDescriptor == null)
            {
                _logger.Warning("Tried to get a database that didnt exist inside the descriptor list.");
                return null;
            }

            var cachedDatabase = _cachedDatabases.FirstOrDefault(
                x => x.Descriptor.databaseId == id || x.Descriptor.name == name
            );

            if (cachedDatabase == null)
            {
                var database = new AstraDatabase(databaseDescriptor);
                _cachedDatabases.Add(database);

                return database;
            }
            else return cachedDatabase;
        }
    }
}
