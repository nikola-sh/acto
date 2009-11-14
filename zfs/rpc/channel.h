
#ifndef __rpc_channel_h__
#define __rpc_channel_h__

#include <stdio.h>

#include <map>

#include "rpc.h"

/**
  * Канал обмена сообщениями между узлами
  *
  * Удаляется самостоятельно при закрытии сокета
  */
class TCommandChannel {
    /// Тип обработчика команд
    typedef void (*Handler)(int, const RpcHeader* const, void*);
    /// Обработчик закрытия соединения
    typedef void (*OnDisconnected)(int, void*);
    /// Карта обработчиков комманд
    typedef std::map<ssize_t, Handler>  HandlerMap;

    void*           m_ctx;
    HandlerMap      m_handlers;
    OnDisconnected  m_ondisconnected;

public:
    TCommandChannel()
        : m_ctx(0)
    {
    }

private:
    ///
    static void GetCommand(int s, SOEVENT* const ev) {
        TCommandChannel* const channel = static_cast<TCommandChannel*>(ev->param);

        switch (ev->type) {
        case SOEVENT_CLOSED:
            if (channel->m_ondisconnected != 0)
                channel->m_ondisconnected(s, channel->m_ctx);
            delete channel;
            return;
        case SOEVENT_TIMEOUT:
            break;
        case SOEVENT_READ:
            {
                RpcHeader hdr;
                const int rval = recv(s, &hdr, sizeof(hdr), MSG_PEEK);
                // -
                if (rval == sizeof(hdr)) {
                    HandlerMap::const_iterator i = channel->m_handlers.find(hdr.code);
                    // -
                    if (i != channel->m_handlers.end()) {
                        i->second(s, &hdr, channel->m_ctx);
                    }
                    else {
                        // Не зарегистрированная комманда
                        char   buf[256];
                        size_t total = 0;
                        while (total < hdr.size) {
                            int rval = so_readsync(s, buf, sizeof(buf), 5);
                            if (rval > 0)
                                total += rval;
                            if (rval == -1)
                                break;
                        }
                        // Unknown command code
                        send(s, buf, sizeof(buf), 0);
                    }
                    // Logging...
                    printf("%s\t%d\n", RpcCommandString(hdr.code), hdr.size);
                }
                else {
                    if (rval == -1) {
                        //printf("getCommand: error\n");
                    }
                }
            }
            break;
        }
        so_pending(s, SOEVENT_READ, 10, &TCommandChannel::GetCommand, ev->param);
    }

public:
    void activate(int s, void* ctx) {
        m_ctx = ctx;
        so_pending(s, SOEVENT_READ, 10, &TCommandChannel::GetCommand, this);
    }
    /// -
    void registerHandler(const ssize_t code, Handler handler) {
        m_handlers[code] = handler;
    }
    ///
    void onDisconnected(OnDisconnected handler) {
        m_ondisconnected = handler;
    }
};

#endif // __rpc_channel_h__