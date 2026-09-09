using System.Net;

namespace AUnrealFeatures.Hosting.Http.Actions
{
    public sealed class HttpOkActionResult : AstraHttpActionResult<object>
    {
        public HttpOkActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.OK;
    }

    public sealed class HttpCreatedActionResult : AstraHttpActionResult<object>
    {
        public HttpCreatedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Created;
    }

    public sealed class HttpAcceptedActionResult : AstraHttpActionResult<object>
    {
        public HttpAcceptedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Accepted;
    }

    public sealed class HttpNoContentActionResult : AstraHttpActionResult<object>
    {
        public HttpNoContentActionResult() : base(null) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.NoContent;
    }

    public sealed class HttpPartialContentActionResult : AstraHttpActionResult<object>
    {
        public HttpPartialContentActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.PartialContent;
    }

    public sealed class HttpMultipleChoicesActionResult : AstraHttpActionResult<object>
    {
        public HttpMultipleChoicesActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.MultipleChoices;
    }

    public sealed class HttpMovedPermanentlyActionResult : AstraHttpActionResult<object>
    {
        public HttpMovedPermanentlyActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.MovedPermanently;
    }

    public sealed class HttpFoundActionResult : AstraHttpActionResult<object>
    {
        public HttpFoundActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Found;
    }

    public sealed class HttpSeeOtherActionResult : AstraHttpActionResult<object>
    {
        public HttpSeeOtherActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.SeeOther;
    }

    public sealed class HttpNotModifiedActionResult : AstraHttpActionResult<object>
    {
        public HttpNotModifiedActionResult() : base(null) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.NotModified;
    }

    public sealed class HttpTemporaryRedirectActionResult : AstraHttpActionResult<object>
    {
        public HttpTemporaryRedirectActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.TemporaryRedirect;
    }

    public sealed class HttpBadRequestActionResult : AstraHttpActionResult<object>
    {
        public HttpBadRequestActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.BadRequest;
    }

    public sealed class HttpUnauthorizedActionResult : AstraHttpActionResult<object>
    {
        public HttpUnauthorizedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Unauthorized;
    }

    public sealed class HttpPaymentRequiredActionResult : AstraHttpActionResult<object>
    {
        public HttpPaymentRequiredActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.PaymentRequired;
    }

    public sealed class HttpForbiddenActionResult : AstraHttpActionResult<object>
    {
        public HttpForbiddenActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Forbidden;
    }

    public sealed class HttpNotFoundActionResult : AstraHttpActionResult<object>
    {
        public HttpNotFoundActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.NotFound;
    }

    public sealed class HttpMethodNotAllowedActionResult : AstraHttpActionResult<object>
    {
        public HttpMethodNotAllowedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.MethodNotAllowed;
    }

    public sealed class HttpNotAcceptableActionResult : AstraHttpActionResult<object>
    {
        public HttpNotAcceptableActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.NotAcceptable;
    }

    public sealed class HttpProxyAuthenticationRequiredActionResult : AstraHttpActionResult<object>
    {
        public HttpProxyAuthenticationRequiredActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.ProxyAuthenticationRequired;
    }

    public sealed class HttpRequestTimeoutActionResult : AstraHttpActionResult<object>
    {
        public HttpRequestTimeoutActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.RequestTimeout;
    }

    public sealed class HttpConflictActionResult : AstraHttpActionResult<object>
    {
        public HttpConflictActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Conflict;
    }

    public sealed class HttpGoneActionResult : AstraHttpActionResult<object>
    {
        public HttpGoneActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.Gone;
    }

    public sealed class HttpLengthRequiredActionResult : AstraHttpActionResult<object>
    {
        public HttpLengthRequiredActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.LengthRequired;
    }

    public sealed class HttpPreconditionFailedActionResult : AstraHttpActionResult<object>
    {
        public HttpPreconditionFailedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.PreconditionFailed;
    }

    public sealed class HttpRequestEntityTooLargeActionResult : AstraHttpActionResult<object>
    {
        public HttpRequestEntityTooLargeActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.RequestEntityTooLarge;
    }

    public sealed class HttpRequestUriTooLongActionResult : AstraHttpActionResult<object>
    {
        public HttpRequestUriTooLongActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.RequestUriTooLong;
    }

    public sealed class HttpUnsupportedMediaTypeActionResult : AstraHttpActionResult<object>
    {
        public HttpUnsupportedMediaTypeActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.UnsupportedMediaType;
    }

    public sealed class HttpRequestedRangeNotSatisfiableActionResult : AstraHttpActionResult<object>
    {
        public HttpRequestedRangeNotSatisfiableActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.RequestedRangeNotSatisfiable;
    }

    public sealed class HttpExpectationFailedActionResult : AstraHttpActionResult<object>
    {
        public HttpExpectationFailedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.ExpectationFailed;
    }

    public sealed class HttpUpgradeRequiredActionResult : AstraHttpActionResult<object>
    {
        public HttpUpgradeRequiredActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.UpgradeRequired;
    }

    public sealed class HttpInternalServerErrorActionResult : AstraHttpActionResult<object>
    {
        public HttpInternalServerErrorActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.InternalServerError;
    }

    public sealed class HttpNotImplementedActionResult : AstraHttpActionResult<object>
    {
        public HttpNotImplementedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.NotImplemented;
    }

    public sealed class HttpBadGatewayActionResult : AstraHttpActionResult<object>
    {
        public HttpBadGatewayActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.BadGateway;
    }

    public sealed class HttpServiceUnavailableActionResult : AstraHttpActionResult<object>
    {
        public HttpServiceUnavailableActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.ServiceUnavailable;
    }

    public sealed class HttpGatewayTimeoutActionResult : AstraHttpActionResult<object>
    {
        public HttpGatewayTimeoutActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.GatewayTimeout;
    }

    public sealed class HttpHttpVersionNotSupportedActionResult : AstraHttpActionResult<object>
    {
        public HttpHttpVersionNotSupportedActionResult(object content) : base(content) { }
        public override HttpStatusCode StatusCode => HttpStatusCode.HttpVersionNotSupported;
    }
}