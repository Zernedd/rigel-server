using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.IO;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public sealed class AstraHttpResponse : IHttpResponse, IHttpResponseInternal
    {
        private readonly HttpListenerResponse _response;
        private byte[]? _content;

        private AstraHttpResponse(HttpListenerResponse response)
        {
            _response = response;
            Headers = new Dictionary<string, string>();
        }

        public static IHttpResponse New(HttpListenerResponse httpListenerResponse)
            => new AstraHttpResponse(httpListenerResponse);

        public HttpStatusCode StatusCode
        {
            get => (HttpStatusCode)_response.StatusCode;
            set => _response.StatusCode = (int)value;
        }

        public string ContentType
        {
            get => _response.ContentType;
            set => _response.ContentType = value;
        }

        public byte[] Content
        {
            get => _content ?? Array.Empty<byte>();
            set
            {
                _content = value;
                _response.ContentLength64 = value.Length;
            }
        }

        public Dictionary<string, string> Headers { get; }

        public void SetHeader(string key, string value)
        {
            Headers[key] = value;
            _response.Headers[key] = value;
        }

        public void RemoveHeader(string key)
        {
            Headers.Remove(key);
            _response.Headers.Remove(key);
        }

        public void SetCookie(Cookie cookie) => _response.Cookies.Add(cookie);
        public void Redirect(string url) => _response.Redirect(url);

        public void SetContentString(string content, Encoding? encoding = null)
        {
            encoding ??= Encoding.UTF8;
            Content = encoding.GetBytes(content);
        }

        public async Task WriteContentAsync(Stream stream)
        {
            using var responseStream = _response.OutputStream;
            await stream.CopyToAsync(responseStream);
        }

        public void SetJsonContent(object obj)
        {
            SetContentString(System.Text.Json.JsonSerializer.Serialize(obj));
            ContentType = "application/json";
        }

        public void SetXmlContent(object obj)
        {
            var serializer = new System.Xml.Serialization.XmlSerializer(obj.GetType());

            using var stringWriter = new StringWriter();
            serializer.Serialize(stringWriter, obj);

            SetContentString(stringWriter.ToString());
            ContentType = "application/xml";
        }

        public void EnableCors(string origin = "*", string methods = "GET,POST,PUT,DELETE,OPTIONS", string headers = "*")
        {
            SetHeader("Access-Control-Allow-Origin", origin);
            SetHeader("Access-Control-Allow-Methods", methods);
            SetHeader("Access-Control-Allow-Headers", headers);
        }

        public void SetContentDisposition(string fileName, bool inline = false)
        {
            var disposition = inline ? "inline" : "attachment";
            SetHeader("Content-Disposition", $"{disposition}; filename=\"{fileName}\"");
        }

        public void SetCacheControl(int maxAgeSeconds) => SetHeader("Cache-Control", $"max-age={maxAgeSeconds}, public");

        public void ApplyToHttpListenerResponse(IHttpActionResult httpActionResult)
        {
            if (httpActionResult == null) return;
            foreach (var header in Headers)
                _response.Headers[header.Key] = header.Value;

            _response.StatusCode = (int)httpActionResult.StatusCode;
            Content = httpActionResult.Body;
            _response.ContentType = httpActionResult.ContentType;

            if (_content != null)
            {
                _response.ContentLength64 = _content.Length;
                _response.OutputStream.Write(_content, 0, _content.Length);
            }
        }
    }
}