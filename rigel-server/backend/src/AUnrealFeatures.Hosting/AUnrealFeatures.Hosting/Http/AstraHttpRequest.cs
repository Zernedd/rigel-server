using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.IO;
using System.Text.Json;
using System.Web;

namespace AUnrealFeatures.Hosting.Http
{
    public sealed class AstraHttpRequest : IHttpRequest
    {
        private readonly HttpListenerRequest _request;

        private byte[]? _body;
        private Dictionary<string, object>? _jsonBody;
        private Dictionary<string, string>? _formBody;

        private AstraHttpRequest(HttpListenerRequest request)
        {
            _request = request;
        }

        public static IHttpRequest New(HttpListenerRequest httpListenerRequest)
            => new AstraHttpRequest(httpListenerRequest);

        public string? GetHeaderValue(string key, [Optional] string? defaultValue) => Headers.TryGetValue(key, out var value) ? value : defaultValue;
        public string? GetQueryParameter(string key, [Optional] string? defaultValue) => Queries.TryGetValue(key, out var value) ? value : defaultValue;

        private HttpMethod _method;
        public HttpMethod Method
        {
            get
            {
                if (_method == null)
                    _method = new HttpMethod(_request.HttpMethod);
                return _method;
            }
        }

        public string Host => _request.Url.Host;
        public string RequestId => _request.RequestTraceIdentifier.ToString();
        // Some clients build URLs as "{base}/{route}" where base already ends in '/',
        // producing a doubled leading slash (e.g. the A2 Mothership "//v2/player/...").
        // The route table stores single-slash paths, so collapse runs of '/' before
        // matching, otherwise the request 404s and the client reports an auth failure.
        public string Uri => CollapseSlashes(_request.Url!.AbsolutePath);

        private static string CollapseSlashes(string path)
        {
            if (string.IsNullOrEmpty(path) || path.IndexOf("//", StringComparison.Ordinal) < 0)
                return path;
            var sb = new StringBuilder(path.Length);
            var prevSlash = false;
            foreach (var c in path)
            {
                if (c == '/')
                {
                    if (prevSlash) continue;
                    prevSlash = true;
                }
                else prevSlash = false;
                sb.Append(c);
            }
            return sb.ToString();
        }

        // HTTP header names are case-insensitive (RFC 7230 §3.2). Proxies (ngrok in particular)
        // normalize header names to title-case ("X-Entitytoken" instead of "X-EntityToken"),
        // so a case-sensitive lookup misses and ends up returning 401 even when the right
        // header is present. Build the dict with OrdinalIgnoreCase so lookups always work.
        public Dictionary<string, string> Headers => _request.Headers.AllKeys
            .ToDictionary(k => k!, k => _request.Headers[k]!, StringComparer.OrdinalIgnoreCase);
        public Dictionary<string, string> Queries => _request.QueryString.AllKeys.ToDictionary(k => k!, k => _request.QueryString[k])!;

        public byte[] Body
        {
            get
            {
                if (_body == null)
                {
                    using var ms = new MemoryStream();
                    _request.InputStream.CopyTo(ms);
                    _body = ms.ToArray();
                }
                return _body;
            }
        }

        public Dictionary<string, object> JsonBody
        {
            get
            {
                if (_jsonBody == null)
                {
                    if (_request.ContentType!.StartsWith("application/json", StringComparison.OrdinalIgnoreCase))
                    {
                        _jsonBody = JsonSerializer.Deserialize<Dictionary<string, object>>(Body) ?? new Dictionary<string, object>();
                    }
                    else throw new InvalidOperationException("The request body is not in JSON format.");
                }
                return _jsonBody;
            }
        }

        public Dictionary<string, string> FormBody
        {
            get
            {
                if (_formBody == null)
                {
                    if (_request.ContentType!.StartsWith("application/x-www-form-urlencoded", StringComparison.OrdinalIgnoreCase))
                    {
                        _formBody = HttpUtility.ParseQueryString(Encoding.UTF8.GetString(Body)).AllKeys
                                               .ToDictionary(k => k!, k => HttpUtility.ParseQueryString(Encoding.UTF8.GetString(Body))[k])!;
                    }
                    else throw new InvalidOperationException("The request body is not in URL-encoded format.");
                }
                return _formBody;
            }
        }

        public IPAddress Remote
        {
            get
            {
                if (Headers.ContainsKey("Cf-Connecting-Ip")) return IPAddress.Parse(Headers["Cf-Connecting-Ip"]);
                if (Headers.ContainsKey("X-Real-Ip")) return IPAddress.Parse(Headers["X-Real-Ip"]);

                return _request.RemoteEndPoint.Address;
            }
        }

        public IPAddress Origin => _request.LocalEndPoint.Address;
    }
}