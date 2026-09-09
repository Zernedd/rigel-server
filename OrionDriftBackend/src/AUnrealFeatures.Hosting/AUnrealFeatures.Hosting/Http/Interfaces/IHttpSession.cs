using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http.Interfaces
{
    public interface IHttpSession
    {
        string SessionId { get; }
        string SessionType { get; }
        DateTime ExpiresAt { get; }
        List<string> Scopes { get; }
        Dictionary<string, string> Claims { get; }

        bool IsExpired();
        void AddScope(string scope);
        void RemoveScope(string scope);

        void AddClaim(string key, string value);
        void RemoveClaim(string key);

        void Extend(TimeSpan duration);
    }
}
