using AUnrealFeatures.Hosting.Http.Preprocessors.Default;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public static class AstraHttpServerExtensions
    {
        public static void AddStaticFiles(this AstraHttpServer httpServer, string baseFolder, bool allowExtensionlessLookups = false)
            => httpServer.AddPreprocessor<HttpStaticFilesProcessor>(baseFolder, allowExtensionlessLookups);

        public static void AddStorageContainers(this AstraHttpServer httpServer, string baseFolder, string[] containers)
            => httpServer.AddPreprocessor<HttpStorageContainerProcessor>(httpServer, baseFolder, containers);
    }
}
