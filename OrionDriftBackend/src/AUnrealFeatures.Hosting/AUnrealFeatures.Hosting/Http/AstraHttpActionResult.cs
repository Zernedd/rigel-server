using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Net;
using System.Text;
using System.Text.Json;

namespace AUnrealFeatures.Hosting.Http
{
    public abstract class AstraHttpActionResult<TBodyObject> : IHttpActionResult
    {
        static readonly JsonSerializerOptions _defaultJsonOptions = new JsonSerializerOptions
        {
            PropertyNamingPolicy = null,
            IncludeFields = false,
            IgnoreReadOnlyFields = true,
            DictionaryKeyPolicy = null
        };

        protected TBodyObject? BodyObject { get; }
        protected AstraHttpActionResult(TBodyObject? bodyObject = default)
        {
            BodyObject = bodyObject;
        }

        public abstract HttpStatusCode StatusCode { get; }
        public virtual string ContentType
        {
            get
            {
                if (BodyObject == null || BodyObject.GetType().IsPrimitive())
                    return "text/plain";
                if (BodyObject is byte[])
                    return "application/octet-stream";
                return "application/json";
            }
        }
        public virtual byte[] Body => SerializeIntoBuffer();

        protected virtual byte[] SerializeIntoBuffer()
        {
            if (BodyObject == null)
                return Array.Empty<byte>();
            if (BodyObject is byte[] array) return array;

            if (BodyObject.GetType().IsPrimitive())
                return Encoding.UTF8.GetBytes(BodyObject.ToString() ?? "null");
            return JsonSerializer.SerializeToUtf8Bytes(BodyObject, GetSerializerOptions());
        }

        protected virtual JsonSerializerOptions GetSerializerOptions()
        {
            return _defaultJsonOptions;
        }
    }
}