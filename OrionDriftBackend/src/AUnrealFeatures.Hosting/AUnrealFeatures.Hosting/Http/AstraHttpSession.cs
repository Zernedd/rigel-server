using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Http
{
    public sealed class AstraHttpSession : IHttpSession
    {
        private string _sessionId;
        private string _sessionType;
        private DateTime _expiresAt;
        private List<string> _scopes;
        private Dictionary<string, string> _claims;

        private AstraHttpSession(string sessionId, string sessionType, DateTime expiresAt, List<string> scopes, Dictionary<string, string> claims)
        {
            _sessionId = sessionId;
            _sessionType = sessionType;
            _expiresAt = expiresAt;
            _scopes = scopes;
            _claims = claims;
        }

        public static IHttpSession New(string sessionId, string sessionType, DateTime expiresAt, List<string> scopes, Dictionary<string, string> claims)
        {
            return new AstraHttpSession(sessionId, sessionType, expiresAt, scopes, claims);
        }

        public string SessionId => _sessionId;
        public string SessionType => _sessionType;
        public DateTime ExpiresAt => _expiresAt;
        public List<string> Scopes => _scopes;
        public Dictionary<string, string> Claims => _claims;
        public bool IsExpired() => DateTime.UtcNow > ExpiresAt;

        public void AddScope(string scope)
        {
            if (!_scopes.Contains(scope))
                _scopes.Add(scope);
        }

        public void RemoveScope(string scope)
        {
            _scopes.Remove(scope);
        }

        public void AddClaim(string key, string value)
        {
            _claims[key] = value;
        }
        public void RemoveClaim(string key) => _claims.Remove(key);

        public void Extend(TimeSpan duration)
        {
            _expiresAt = DateTime.UtcNow.Add(duration);
        }
    }
}