using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.WebSockets.Interfaces
{
    public delegate void WebSocketStateMachineOnOpen(IWebSocketClient socketClient);
    public delegate void WebSocketStateMachineOnMessage(IWebSocketClient socketClient, byte[] content);
    public delegate void WebSocketStateMachineOnClose(IWebSocketClient socketClient);

    public interface IWebSocketStateMachine
    {
        IWebSocketStateMachine OnOpen(WebSocketStateMachineOnOpen onOpenAction);
        IWebSocketStateMachine OnMessage(WebSocketStateMachineOnMessage onMessageAction);
        IWebSocketStateMachine OnClose(WebSocketStateMachineOnClose onCloseAction);
    }
}
