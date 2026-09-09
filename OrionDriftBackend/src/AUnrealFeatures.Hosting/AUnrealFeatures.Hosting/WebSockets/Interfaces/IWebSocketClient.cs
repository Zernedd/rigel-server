using AUnrealFeatures.Hosting.Http.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.WebSockets.Interfaces
{
    public interface IWebSocketClient
    {
        string? GetQueryParameter(string key, [Optional] string? defaultValue);
        string? GetHeaderValue(string key, [Optional] string? defaultValue);

        Task SendMessageAsync(string content);
        Task SendMessageAsync(byte[] content, WebSocketMessageType messageType);

        Task<WebSocketReceiveResult> ReadMessageAsync(byte[] content);
        Task<ValueWebSocketReceiveResult> ReadMessageAsync(Memory<byte> content);

        Task CloseAsync(ushort code, string reason);

        void SetClientId(string clientId);
        string ClientId { get; }

        WebSocketState State { get; }
        bool IsConnected { get; }

        string Route { get; }
        Dictionary<string, string> Queries { get; }
        Dictionary<string, string> Headers { get; }

        IHttpRequest Request { get; }
    }
}
