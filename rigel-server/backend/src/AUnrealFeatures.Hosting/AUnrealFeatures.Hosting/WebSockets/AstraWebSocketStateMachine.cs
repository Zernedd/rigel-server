using AUnrealFeatures.Hosting.WebSockets.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.WebSockets
{
    public sealed class AstraWebSocketStateMachine : IWebSocketStateMachine
    {
        public IWebSocketStateMachine OnOpen(WebSocketStateMachineOnOpen onOpenAction)
        {
            if (_onOpenActions != null && _onOpenActions.Count < 25)
                _onOpenActions.Add(onOpenAction);
            return this;
        }
        public void InvokeOnOpen(IWebSocketClient socketClient) 
            => _onOpenActions.ForEach(element => element.Invoke(socketClient));

        public IWebSocketStateMachine OnMessage(WebSocketStateMachineOnMessage onMessageAction)
        {
            if (_onMessageActions != null && _onMessageActions.Count < 25)
                _onMessageActions.Add(onMessageAction);
            return this;
        }
        public void InvokeOnMessage(IWebSocketClient socketClient, byte[] content) 
            => _onMessageActions.ForEach(element => element.Invoke(socketClient, content));

        public IWebSocketStateMachine OnClose(WebSocketStateMachineOnClose onCloseAction)
        {
            if (_onCloseActions != null && _onCloseActions.Count < 25)
                _onCloseActions.Add(onCloseAction);
            return this;
        }
        public void InvokeOnClose(IWebSocketClient socketClient) 
            => _onCloseActions.ForEach(element => element.Invoke(socketClient));

        private readonly List<WebSocketStateMachineOnOpen> _onOpenActions = new();
        private readonly List<WebSocketStateMachineOnMessage> _onMessageActions = new();
        private readonly List<WebSocketStateMachineOnClose> _onCloseActions = new();
    }
}
