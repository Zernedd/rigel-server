using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Hosting.WebSockets.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.WebSockets;
using System.Reflection.Metadata;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.WebSockets
{
    public sealed class AstraWebSocketClient : IWebSocketClient
    {
        private readonly HttpListenerContext _httpListenerContext;
        private readonly HttpListenerWebSocketContext _webSocketContext;
        private readonly IHttpRequest _httpRequest;
        public AstraWebSocketClient(HttpListenerContext httpListenerContext, HttpListenerWebSocketContext webSocketContext, [Optional] string? clientId)
        {
            _httpListenerContext = httpListenerContext;
            _webSocketContext = webSocketContext;
            _httpRequest = AstraHttpRequest.New(_httpListenerContext.Request);

            _clientId = clientId;
        }
        public static IWebSocketClient New(HttpListenerContext httpListenerContext, HttpListenerWebSocketContext webSocketContext, [Optional] string? clientId)
            => new AstraWebSocketClient(httpListenerContext, webSocketContext);

        public string? GetHeaderValue(string key, [Optional] string? defaultValue) => Headers.TryGetValue(key, out var value) ? value : defaultValue;
        public string? GetQueryParameter(string key, [Optional] string? defaultValue) => Queries.TryGetValue(key, out var value) ? value : defaultValue;

        public async Task SendMessageAsync(string content) 
            => await SendMessageAsync(Encoding.UTF8.GetBytes(content), WebSocketMessageType.Text);

        public async Task SendMessageAsync(byte[] content, WebSocketMessageType messageType)
        {
            if (_webSocketContext.WebSocket.State != WebSocketState.Open)
                return;
            await _webSocketContext.WebSocket.SendAsync(content, messageType, true, CancellationToken.None);
        }

        public async Task<WebSocketReceiveResult> ReadMessageAsync(byte[] content)
        {
            if (_webSocketContext.WebSocket.State != WebSocketState.Open)
                return null!;
            return await _webSocketContext.WebSocket.ReceiveAsync(content, CancellationToken.None);
        }

        public async Task<ValueWebSocketReceiveResult> ReadMessageAsync(Memory<byte> content)
        {
            if (_webSocketContext.WebSocket.State != WebSocketState.Open)
                return default!;
            return await _webSocketContext.WebSocket.ReceiveAsync(content, CancellationToken.None);
        }

        public async Task CloseAsync(ushort code, string reason)
        {
            if (_webSocketContext.WebSocket.State != WebSocketState.Open)
                return;
            await _webSocketContext.WebSocket.CloseAsync((WebSocketCloseStatus)code, reason, CancellationToken.None);
        }

        public void SetClientId(string clientId)
            => _clientId = clientId;
        private string _clientId;

        public string ClientId
        {
            get
            {
                if (string.IsNullOrEmpty(_clientId))
                    _clientId = StringExtensions.CreateRandomString(15);
                return _clientId;
            }
        }

        public WebSocketState State => _webSocketContext.WebSocket.State;
        public bool IsConnected => _webSocketContext.WebSocket.State == WebSocketState.Open;

        public string Route => _httpListenerContext.Request.Url!.AbsolutePath;
        // Case-insensitive header lookup — see AstraHttpRequest.Headers for rationale.
        public Dictionary<string, string> Headers => _httpListenerContext.Request.Headers.AllKeys
            .ToDictionary(k => k!, k => _httpListenerContext.Request.Headers[k]!, StringComparer.OrdinalIgnoreCase);
        public Dictionary<string, string> Queries => _httpListenerContext.Request.QueryString.AllKeys.ToDictionary(k => k!, k => _httpListenerContext.Request.QueryString[k])!;
        public IHttpRequest Request => _httpRequest;
    }
}
