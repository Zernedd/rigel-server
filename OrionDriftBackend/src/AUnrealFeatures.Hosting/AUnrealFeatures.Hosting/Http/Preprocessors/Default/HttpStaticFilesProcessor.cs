using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Interfaces;
using MimeTypes;
using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Preprocessors.Default
{
    public sealed class HttpStaticFilesProcessor : IHttpRequestPreprocessor
    {
        private readonly string _baseFolder;
        private readonly bool _allowExtensionlessLookups;

        public HttpStaticFilesProcessor(string baseFolder, bool allowExtensionlessLookups = false)
        {
            _baseFolder = baseFolder;
            _allowExtensionlessLookups = allowExtensionlessLookups;
        }

        public async Task<HttpPreprocessorContainer> TryPreprocessRequest(IHttpRequest request, IHttpResponse response)
        {
            string filePath = GetFilePath(request.Uri);

            if (await FileExistsAsync(filePath))
                return await CreateSuccessResponseAsync(filePath);

            if (await DirectoryExistsAsync(filePath))
            {
                string indexFile = await FindIndexFileAsync(filePath);
                if (indexFile != null)
                    return await CreateSuccessResponseAsync(indexFile);
            }

            if (_allowExtensionlessLookups)
            {
                string fileWithExtension = await FindFileWithExtensionAsync(filePath);
                if (fileWithExtension != null)
                    return await CreateSuccessResponseAsync(fileWithExtension);
            }

            return new HttpPreprocessorContainer
            {
                actionResult = Results.NotFound(),
                result = HttpPreprocessorResult.FAIL
            };
        }

        private string GetFilePath(string uri)
        {
            string[] parts = new[] { Environment.CurrentDirectory, _baseFolder }
                .Concat(uri.Split('/', StringSplitOptions.RemoveEmptyEntries))
                .ToArray();
            return Path.Combine(parts);
        }

#pragma warning disable CS8603 // Possible null reference return.
        private async Task<string> FindIndexFileAsync(string directoryPath)
        {
            var files = await Task.Run(() => Directory.GetFiles(directoryPath));
            return files.FirstOrDefault(file => Path.GetFileNameWithoutExtension(file).Equals("index", StringComparison.OrdinalIgnoreCase));
        }

        private async Task<string> FindFileWithExtensionAsync(string filePath)
        {
            string directory = Path.GetDirectoryName(filePath);
            if (directory == null) return null;

            string fileNameWithoutExtension = Path.GetFileNameWithoutExtension(filePath);

            var files = await Task.Run(() => Directory.GetFiles(directory));
            return files.FirstOrDefault(file => Path.GetFileNameWithoutExtension(file).Equals(fileNameWithoutExtension, StringComparison.OrdinalIgnoreCase));
#pragma warning restore CS8603 // Possible null reference return.
        }

        private async Task<HttpPreprocessorContainer> CreateSuccessResponseAsync(string filePath)
        {
            string mimeType = MimeTypeMap.GetMimeType(Path.GetExtension(filePath).TrimStart('.'));
            byte[] fileContent = await File.ReadAllBytesAsync(filePath);

            return new HttpPreprocessorContainer
            {
                actionResult = Results.Configurable(HttpStatusCode.OK, mimeType, fileContent),
                result = HttpPreprocessorResult.OK | HttpPreprocessorResult.STOP_AFTER
            };
        }

        private Task<bool> FileExistsAsync(string path) => Task.Run(() => File.Exists(path));
        private Task<bool> DirectoryExistsAsync(string path) => Task.Run(() => Directory.Exists(path));
    }
}