using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Interfaces
{
    public interface IHttpResponseInternal
    {
        void ApplyToHttpListenerResponse(IHttpActionResult httpActionResult);
    }

    public interface IHttpResponse : IHttpResponseInternal
    {
        HttpStatusCode StatusCode { get; set; }
        string ContentType { get; set; }
        byte[] Content { get; set; }
        Dictionary<string, string> Headers { get; }

        void SetHeader(string key, string value);
        void RemoveHeader(string key);

        void SetCookie(Cookie cookie);
        void Redirect(string url);

        void SetContentString(string content, Encoding? encoding = null);
        Task WriteContentAsync(Stream stream);

        void SetJsonContent(object obj);
        void SetXmlContent(object obj);

        void EnableCors(string origin = "*", string methods = "GET,POST,PUT,DELETE,OPTIONS", string headers = "*");

        void SetContentDisposition(string fileName, bool inline = false);
        void SetCacheControl(int maxAgeSeconds);
    }
}
