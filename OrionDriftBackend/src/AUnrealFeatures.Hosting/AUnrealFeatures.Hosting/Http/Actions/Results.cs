using AUnrealFeatures.Hosting.Http.Interfaces;
using System.Net;

namespace AUnrealFeatures.Hosting.Http.Actions
{
    public sealed class ConfigurableActionResult : AstraHttpActionResult<object>
    {
        private readonly HttpStatusCode _statusCode;
        private readonly string _contentType;
        public ConfigurableActionResult(HttpStatusCode statusCode, string contentType, object content = null) : base(content)
        {
            _statusCode = statusCode;
            _contentType = contentType;
        }

        public override HttpStatusCode StatusCode => _statusCode;
        public override string ContentType => _contentType;
    }

    public static class Results
    {
        public static IHttpActionResult Ok(object content = null) => new HttpOkActionResult(content);
        public static IHttpActionResult Created(object content = null) => new HttpCreatedActionResult(content);
        public static IHttpActionResult Accepted(object content = null) => new HttpAcceptedActionResult(content);
        public static IHttpActionResult NoContent() => new HttpNoContentActionResult();
        public static IHttpActionResult PartialContent(object content = null) => new HttpPartialContentActionResult(content);
        public static IHttpActionResult MultipleChoices(object content = null) => new HttpMultipleChoicesActionResult(content);
        public static IHttpActionResult MovedPermanently(object content = null) => new HttpMovedPermanentlyActionResult(content);
        public static IHttpActionResult Found(object content = null) => new HttpFoundActionResult(content);
        public static IHttpActionResult SeeOther(object content = null) => new HttpSeeOtherActionResult(content);
        public static IHttpActionResult NotModified() => new HttpNotModifiedActionResult();
        public static IHttpActionResult TemporaryRedirect(object content = null) => new HttpTemporaryRedirectActionResult(content);
        public static IHttpActionResult BadRequest(object content = null) => new HttpBadRequestActionResult(content);
        public static IHttpActionResult Unauthorized(object content = null) => new HttpUnauthorizedActionResult(content);
        public static IHttpActionResult PaymentRequired(object content = null) => new HttpPaymentRequiredActionResult(content);
        public static IHttpActionResult Forbidden(object content = null) => new HttpForbiddenActionResult(content);
        public static IHttpActionResult NotFound(object content = null) => new HttpNotFoundActionResult(content);
        public static IHttpActionResult MethodNotAllowed(object content = null) => new HttpMethodNotAllowedActionResult(content);
        public static IHttpActionResult NotAcceptable(object content = null) => new HttpNotAcceptableActionResult(content);
        public static IHttpActionResult ProxyAuthenticationRequired(object content = null) => new HttpProxyAuthenticationRequiredActionResult(content);
        public static IHttpActionResult RequestTimeout(object content = null) => new HttpRequestTimeoutActionResult(content);
        public static IHttpActionResult Conflict(object content = null) => new HttpConflictActionResult(content);
        public static IHttpActionResult Gone(object content = null) => new HttpGoneActionResult(content);
        public static IHttpActionResult LengthRequired(object content = null) => new HttpLengthRequiredActionResult(content);
        public static IHttpActionResult PreconditionFailed(object content = null) => new HttpPreconditionFailedActionResult(content);
        public static IHttpActionResult RequestEntityTooLarge(object content = null) => new HttpRequestEntityTooLargeActionResult(content);
        public static IHttpActionResult RequestUriTooLong(object content = null) => new HttpRequestUriTooLongActionResult(content);
        public static IHttpActionResult UnsupportedMediaType(object content = null) => new HttpUnsupportedMediaTypeActionResult(content);
        public static IHttpActionResult RequestedRangeNotSatisfiable(object content = null) => new HttpRequestedRangeNotSatisfiableActionResult(content);
        public static IHttpActionResult ExpectationFailed(object content = null) => new HttpExpectationFailedActionResult(content);
        public static IHttpActionResult UpgradeRequired(object content = null) => new HttpUpgradeRequiredActionResult(content);
        public static IHttpActionResult InternalServerError(object content = null) => new HttpInternalServerErrorActionResult(content);
        public static IHttpActionResult NotImplemented(object content = null) => new HttpNotImplementedActionResult(content);
        public static IHttpActionResult BadGateway(object content = null) => new HttpBadGatewayActionResult(content);
        public static IHttpActionResult ServiceUnavailable(object content = null) => new HttpServiceUnavailableActionResult(content);
        public static IHttpActionResult GatewayTimeout(object content = null) => new HttpGatewayTimeoutActionResult(content);
        public static IHttpActionResult HttpVersionNotSupported(object content = null) => new HttpHttpVersionNotSupportedActionResult(content);

        public static IHttpActionResult HtmlDocument(HttpStatusCode statusCode, string htmlContent) => new HtmlDocumentActionResult(statusCode, htmlContent);

        public static IHttpActionResult Configurable(HttpStatusCode statusCode, string contentType, object content = null) 
            => new ConfigurableActionResult(statusCode, contentType, content);
    }
}
