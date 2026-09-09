using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Interfaces;
using MimeTypes;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Text.Json;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Preprocessors.Default
{
    public class ContainerInformation
    {
        public string Name { get; set; }
        public string Description { get; set; }
        public int Owner { get; set; }
        public long CreatedAt { get; set; }
        public long UpdatedAt { get; set; }
        public bool Readable { get; set; }
        public bool Writeable { get; set; }
        public long Size { get; set; }
        public int FileCount { get; set; }
    }

    public sealed class ContainerInformationSensitive : ContainerInformation
    {
        public bool Temporary { get; set; }
        public long ExpiresAt { get; set; }
        public string Ticket { get; set; }
    }

    public class ContainerMessageResult
    {
        public bool Success { get; set; }
        public string Message { get; set; }
    }

    public sealed class ContainerMessageResult<T> : ContainerMessageResult
    {
        public T Value { get; set; }
    }

    // allow for endpoint such as
    // - api/v1/container/create                Creates a container
    // - api/v1/container/delete                Deletes a container
    // - api/v1/container/info                  Gets info on a container
    // - api/v1/container/file/upload           Uploads a file to a container (json body, with base64 content of the file)
    // - api/v1/container/file/delete           Deletes a file from a container
    // - dynamic urls                           Such as '/{ContainerName}/{FileName}' to grab a file
    //
    // use 'IHttpEndpoint AddEndpoint(HttpMethod httpMethod, string endpoint, Delegate method)'
    // to add an endpoint to the server
    public sealed class HttpStorageContainerProcessor : IHttpRequestPreprocessor
    {
        private readonly IHttpServer _httpServer;
        private readonly string _baseFolder;
        private readonly ConcurrentDictionary<string, ContainerInformationSensitive> _containerCache;
        private readonly ConcurrentDictionary<string, long> _containerSizes;
        private readonly ConcurrentDictionary<string, int> _containerFileCounts;

        public HttpStorageContainerProcessor(IHttpServer httpServer, string baseFolder, string[] containers)
        {
            _httpServer = httpServer;
            _baseFolder = baseFolder;
            _containerCache = new ConcurrentDictionary<string, ContainerInformationSensitive>();
            _containerSizes = new ConcurrentDictionary<string, long>();
            _containerFileCounts = new ConcurrentDictionary<string, int>();

            InitializeEndpoints();
            InitializeContainers(containers);
        }

        private void InitializeEndpoints()
        {
            _httpServer.AddEndpoint(HttpMethod.Post, "/api/v1/container/create", HandleContainerCreate, preprocessorInstance: this);
            _httpServer.AddEndpoint(HttpMethod.Post, "/api/v1/container/delete", HandleContainerDelete, preprocessorInstance: this);
            _httpServer.AddEndpoint(HttpMethod.Get, "/api/v1/container/info", HandleContainerInfo, preprocessorInstance: this);
            _httpServer.AddEndpoint(HttpMethod.Post, "/api/v1/container/file/upload", HandleFileUpload, preprocessorInstance: this);
            _httpServer.AddEndpoint(HttpMethod.Post, "/api/v1/container/file/delete", HandleFileDelete, preprocessorInstance: this);
            _httpServer.AddEndpoint(HttpMethod.Get, "/api/v1/container/list", HandleContainerList, preprocessorInstance: this);
        }

        private string GenerateTicket()
        {
            return Guid.NewGuid().ToString("N");
        }

        private void InitializeContainers(string[] containers)
        {
            foreach (var container in containers)
            {
                var containerPath = Path.Combine(_baseFolder, container);
                Directory.CreateDirectory(containerPath);
                LoadContainerMetadata(container);
            }
        }

        private void LoadContainerMetadata(string containerName)
        {
            var containerPath = Path.Combine(_baseFolder, containerName);
            var metadataPath = Path.Combine(containerPath, "metadata.json");

            if (File.Exists(metadataPath))
            {
                var metadata = File.ReadAllText(metadataPath);
                var containerInfo = JsonSerializer.Deserialize<ContainerInformationSensitive>(metadata);

                _containerCache[containerName] = containerInfo;
                UpdateContainerStats(containerName);
            }
        }

        private void UpdateContainerStats(string containerName)
        {
            var containerPath = Path.Combine(_baseFolder, containerName);
            var files = Directory.GetFiles(containerPath, "*", SearchOption.AllDirectories);
            long totalSize = files.Sum(f => new FileInfo(f).Length);
            int fileCount = files.Length;

            _containerSizes[containerName] = totalSize;
            _containerFileCounts[containerName] = fileCount;

            if (_containerCache.TryGetValue(containerName, out var containerInfo))
            {
                containerInfo.Size = totalSize;
                containerInfo.FileCount = fileCount;
                containerInfo.UpdatedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
                SaveContainerMetadata(containerName, containerInfo);
            }
        }

        private void SaveContainerMetadata(string containerName, ContainerInformationSensitive containerInfo)
        {
            var containerPath = Path.Combine(_baseFolder, containerName);
            var metadataPath = Path.Combine(containerPath, "metadata.json");
            File.WriteAllText(metadataPath, JsonSerializer.Serialize(containerInfo));
        }

        public async Task<HttpPreprocessorContainer> TryPreprocessRequest(IHttpRequest request, IHttpResponse response)
        {
            var paths = request.Uri.Split('/');
            if (request.Method == HttpMethod.Get && paths.Length == 2 && _containerCache.ContainsKey(paths[0]))
                return await HandleFileDownload(request, response, paths[0], paths[1]);

            return new HttpPreprocessorContainer
            {
                actionResult = Results.NotFound(),
                result = HttpPreprocessorResult.FAIL
            };
        }

        private async Task<IHttpActionResult> HandleContainerCreate(IHttpRequest request, IHttpResponse response)
        {
            var name = (string?)request.JsonBody?.GetValueOrDefault<string, object?>("name");
            if (string.IsNullOrEmpty(name))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Name is required." });

            if (_containerCache.ContainsKey(name))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Container already exists." });

            var containerInfo = new ContainerInformationSensitive
            {
                Name = name,
                Description = request.JsonBody.TryGetValue("description", out var description) ? (description?.ToString() ?? "") : "",
                Owner = request.JsonBody.TryGetValue("owner", out var ownerStr) && int.TryParse(ownerStr.ToString(), out var owner) ? owner : 0,
                CreatedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                UpdatedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                Temporary = request.JsonBody.TryGetValue("temporary", out var tempStr) && bool.TryParse(tempStr.ToString(), out var temp) && temp,
                ExpiresAt = request.JsonBody.TryGetValue("expiresIn", out var expiresInStr) && int.TryParse(expiresInStr.ToString(), out var expiresIn)
                    ? DateTimeOffset.UtcNow.AddSeconds(expiresIn).ToUnixTimeSeconds()
                    : 0,
                Readable = request.JsonBody.TryGetValue("readable", out var readableStr) && bool.TryParse(readableStr.ToString(), out var readable) && readable,
                Writeable = request.JsonBody.TryGetValue("writeable", out var writeableStr) && bool.TryParse(writeableStr.ToString(), out var writeable) && writeable,
                Ticket = GenerateTicket(),
                Size = 0,
                FileCount = 0
            };

            var containerPath = Path.Combine(_baseFolder, name);
            Directory.CreateDirectory(containerPath);

            _containerCache[name] = containerInfo;
            _containerSizes[name] = 0;
            _containerFileCounts[name] = 0;

            SaveContainerMetadata(name, containerInfo);
            return Results.Ok(new ContainerMessageResult<ContainerInformationSensitive> 
            { 
                Success = true, 
                Message = "Container created successfully.", 
                Value = containerInfo 
            });
        }


        private async Task<IHttpActionResult> HandleContainerDelete(IHttpRequest request, IHttpResponse response)
        {
            if (!request.JsonBody.TryGetValue("name", out var nameObj) || string.IsNullOrEmpty((string)nameObj))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Name is required." });
            var name = nameObj as string ?? throw new InvalidCastException();

            if (!request.JsonBody.TryGetValue("ticket", out var ticketObj) || string.IsNullOrEmpty((string)ticketObj))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Ticket is required." });
            var ticket = ticketObj as string ?? throw new InvalidCastException();

            if (!_containerCache.TryGetValue(name, out var containerInfo) || containerInfo.Ticket != ticket)
                return Results.Forbidden(new ContainerMessageResult { Success = false, Message = "Invalid container or ticket." });

            var containerPath = Path.Combine(_baseFolder, name);
            Directory.Delete(containerPath, true);

            _containerCache.TryRemove(name, out _);
            _containerSizes.TryRemove(name, out _);
            _containerFileCounts.TryRemove(name, out _);

            return Results.Ok(new ContainerMessageResult { Success = true, Message = "Container deleted successfully." });
        }

        private async Task<IHttpActionResult> HandleContainerInfo(IHttpRequest request, IHttpResponse response)
        {
            var name = request.GetQueryParameter("name");
            if (string.IsNullOrEmpty(name))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Name is required." });

            if (!_containerCache.TryGetValue(name, out var containerInfo))
            {
                return Results.NotFound(new ContainerMessageResult { Success = false, Message = "Container not found." });
            }

            var publicInfo = new ContainerInformation
            {
                Name = containerInfo.Name,
                Description = containerInfo.Description,
                Owner = containerInfo.Owner,
                CreatedAt = containerInfo.CreatedAt,
                UpdatedAt = containerInfo.UpdatedAt,
                Readable = containerInfo.Readable,
                Writeable = containerInfo.Writeable,
                Size = containerInfo.Size,
                FileCount = containerInfo.FileCount
            };

            return Results.Ok(new ContainerMessageResult<ContainerInformation> { Success = true, Message = "Container info retrieved successfully.", Value = publicInfo });
        }

        private async Task<IHttpActionResult> HandleFileUpload(IHttpRequest request, IHttpResponse response)
        {
            if (!(request.JsonBody?.TryGetValue("name", out var nameObj) == true && nameObj is string name && !string.IsNullOrEmpty(name)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Container name is required." });

            if (!(request.JsonBody?.TryGetValue("ticket", out var ticketObj) == true && ticketObj is string ticket && !string.IsNullOrEmpty(ticket)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Ticket is required." });

            if (!(request.JsonBody?.TryGetValue("fileName", out var fileNameObj) == true && fileNameObj is string fileName && !string.IsNullOrEmpty(fileName)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "File name is required." });

            if (!(request.JsonBody?.TryGetValue("fileContent", out var fileContentObj) == true && fileContentObj is string fileContent && !string.IsNullOrEmpty(fileContent)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "File content is required." });

            if (!_containerCache.TryGetValue(name, out var containerInfo) || containerInfo.Ticket != ticket)
                return Results.Forbidden(new ContainerMessageResult { Success = false, Message = "Invalid container or ticket." });

            if (!containerInfo.Writeable)
                return Results.Forbidden(new ContainerMessageResult { Success = false, Message = "Container is not writeable." });

            var containerPath = Path.Combine(_baseFolder, name);
            var filePath = Path.Combine(containerPath, fileName);

            byte[] fileBytes = Convert.FromBase64String(fileContent);
            await File.WriteAllBytesAsync(filePath, fileBytes);

            _containerSizes.AddOrUpdate(name, fileBytes.Length, (_, oldSize) => oldSize + fileBytes.Length);
            _containerFileCounts.AddOrUpdate(name, 1, (_, oldCount) => oldCount + 1);

            UpdateContainerStats(name);
            return Results.Ok(new ContainerMessageResult { Success = true, Message = "File uploaded successfully." });
        }

        private async Task<IHttpActionResult> HandleFileDelete(IHttpRequest request, IHttpResponse response)
        {
            if (!(request.JsonBody?.TryGetValue("name", out var nameObj) == true && nameObj is string name && !string.IsNullOrEmpty(name)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Container name is required." });

            if (!(request.JsonBody?.TryGetValue("ticket", out var ticketObj) == true && ticketObj is string ticket && !string.IsNullOrEmpty(ticket)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "Ticket is required." });

            if (!(request.JsonBody?.TryGetValue("fileName", out var fileNameObj) == true && fileNameObj is string fileName && !string.IsNullOrEmpty(fileName)))
                return Results.BadRequest(new ContainerMessageResult { Success = false, Message = "File name is required." });

            if (!_containerCache.TryGetValue(name, out var containerInfo) || containerInfo.Ticket != ticket)
                return Results.Forbidden(new ContainerMessageResult { Success = false, Message = "Invalid container or ticket." });

            if (!containerInfo.Writeable)
                return Results.Forbidden(new ContainerMessageResult { Success = false, Message = "Container is not writeable." });

            var containerPath = Path.Combine(_baseFolder, name);
            var filePath = Path.Combine(containerPath, fileName);
            if (!File.Exists(filePath))
            {
                return Results.NotFound(new ContainerMessageResult { Success = false, Message = "File not found." });
            }

            long fileSize = new FileInfo(filePath).Length;
            File.Delete(filePath);

            _containerSizes.AddOrUpdate(name, 0, (_, oldSize) => oldSize - fileSize);
            _containerFileCounts.AddOrUpdate(name, 0, (_, oldCount) => oldCount - 1);

            UpdateContainerStats(name);
            return Results.Ok(new ContainerMessageResult { Success = true, Message = "File deleted successfully." });
        }

        private async Task<HttpPreprocessorContainer> HandleFileDownload(IHttpRequest request, IHttpResponse response, string containerName, string fileName)
        {
            if (!_containerCache.TryGetValue(containerName, out var containerInfo))
            {
                return new HttpPreprocessorContainer
                {
                    actionResult = Results.NotFound(new ContainerMessageResult { Success = false, Message = "Container not found." }),
                    result = HttpPreprocessorResult.OK | HttpPreprocessorResult.STOP_AFTER
                };
            }

            if (!containerInfo.Readable)
            {
                return new HttpPreprocessorContainer
                {
                    actionResult = Results.Forbidden(new ContainerMessageResult { Success = false, Message = "Container is not readable." }),
                    result = HttpPreprocessorResult.OK | HttpPreprocessorResult.STOP_AFTER
                };
            }

            var filePath = Path.Combine(_baseFolder, containerName, fileName);
            if (!File.Exists(filePath))
            {
                return new HttpPreprocessorContainer
                {
                    actionResult = Results.Forbidden(new ContainerMessageResult { Success = false, Message = "File not found." }),
                    result = HttpPreprocessorResult.OK | HttpPreprocessorResult.STOP_AFTER
                };
            }

            var fileBytes = File.ReadAllBytes(filePath);
            return new HttpPreprocessorContainer
            {
                actionResult = Results.Configurable(
                    HttpStatusCode.OK, 
                    MimeTypeMap.GetMimeType(Path.GetFileName(filePath)), 
                    fileBytes),
                result = HttpPreprocessorResult.OK | HttpPreprocessorResult.STOP_AFTER
            };
        }

        private async Task<IHttpActionResult> HandleContainerList(IHttpRequest request, IHttpResponse response)
        {
            int? ownerFilter = null;
            if (request.Queries?.TryGetValue("owner", out var ownerStr) == true && ownerStr != null && int.TryParse(ownerStr, out var owner))
            {
                ownerFilter = owner;
            }

            var containers = _containerCache.Values
                .Where(c => !ownerFilter.HasValue || c.Owner == ownerFilter.Value)
                .Select(c => new
                {
                    c.Name,
                    c.Description,
                    c.Owner,
                    c.CreatedAt,
                    c.UpdatedAt,
                    c.Size,
                    c.FileCount,
                    c.Readable,
                    c.Writeable,
                    c.Temporary,
                    ExpiresAt = c.ExpiresAt > 0 ? c.ExpiresAt : (long?)null
                })
                .ToList();
            return Results.Ok(containers);
        }
    }
}