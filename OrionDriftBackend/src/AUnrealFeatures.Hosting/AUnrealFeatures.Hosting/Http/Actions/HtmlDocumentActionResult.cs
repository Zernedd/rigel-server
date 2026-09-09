using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Actions
{
    public sealed class HtmlDocumentActionResult : AstraHttpActionResult<string>
    {
        private readonly HttpStatusCode _statusCode;
        public HtmlDocumentActionResult(HttpStatusCode statusCode, string htmlContent) : base(htmlContent)
        {
            _statusCode = statusCode;
        }

        public override HttpStatusCode StatusCode => _statusCode;
        public override string ContentType => "text/html";
    }
}
