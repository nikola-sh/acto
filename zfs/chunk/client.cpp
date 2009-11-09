
#include <port/pointers.h>

#include "chunk.h"


//------------------------------------------------------------------------------
static bool isAllowed(TFileInfo* file, sid_t client) {
    return true;
/*
    printf("isAllowed for: %Zu\n", (size_t)client);
    std::vector<MasterPending*>::iterator i = file->pendings.begin();

    while (i != file->pendings.end()) {
        printf("\t%Zu\n", (size_t)(*i)->client);
        if ((*i)->client == client) {
            file->pendings.erase(i);
            return true;
        }
        ++i;
    }
    return false;
*/
}
//------------------------------------------------------------------------------
void DoClientClose(int s, const RpcHeader* const hdr, void* param) {
    // -
    printf("client close\n");
}
//------------------------------------------------------------------------------
void doClientOpen(int s, const RpcHeader* const hdr, void* param) {
    OpenChunkRequest    req;
    TClientInfo* const  clientInfo = static_cast<TClientInfo*>(param);
    std::map<fileid_t, TFileInfo*>::iterator i;
    // -
    so_readsync(s, &req, sizeof(req), 5);
    // -
    {
        acto::core::MutexLocker    lock(guard);

        if ((i = files.find(req.stream)) != files.end()) {
            TFileInfo* const file = i->second;
            // проверить разрешил ли master открывать данный файл клиенту
            if (isAllowed(file, req.client)) {
                clientInfo->files[file->uid] = file;

                if (req.mode & ZFS_CREATE) {
                    char        buf[64];
                    sprintf(buf, "data/%Zu", (size_t)file->uid);
                    FILE* fd = fopen(buf, "w");
                    if (fd != 0) {
                        file->data = fd;
                        // -
                        OpenChunkResponse   rsp;
                        rsp.file = file->uid;
                        rsp.err  = 0;
                        send(s, &rsp, sizeof(rsp), 0);
                        return;
                    }
                }
                else {
                    char        buf[64];
                    sprintf(buf, "data/%Zu", (size_t)file->uid);
                    FILE* fd = fopen(buf, "r");
                    if (fd != 0) {
                        file->data = fd;
                        // -
                        OpenChunkResponse   rsp;
                        rsp.file = file->uid;
                        rsp.err  = 0;
                        send(s, &rsp, sizeof(rsp), 0);
                        return;
                    }
                }
            }
            else
                printf("access denided\n"); // Доступ к данному файлу запрещен
        }
    }
    // -
    OpenChunkResponse   rsp;
    rsp.file = 0;
    rsp.err  = 1;
    send(s, &rsp, sizeof(rsp), 0);
}
//------------------------------------------------------------------------------
/// Команда чтение данных из потока
void doClientRead(int s, const RpcHeader* const hdr, void* param) {
    ReadReqest req;

    so_readsync(s, &req, sizeof(ReadReqest), 5);

    const FilesMap::iterator i = files.find(req.stream);
    if (i != files.end()) {
        char            data[255];
        ReadResponse    rr;
        int rval = fread(data, 1, 255, i->second->data);
        if (rval > 0) {
            rr.stream = i->first;
            rr.crc    = 0;
            rr.size   = rval;
            rr.futher = false;
            send(s, &rr, sizeof(rr), 0);
            send(s, data, rr.size, 0);
            return;
        }
    }
    ReadResponse    rr;
    rr.stream = req.stream;
    rr.crc    = 0;
    rr.size   = 0;
    rr.futher = false;
    send(s, &rr, sizeof(rr), 0);
}
//------------------------------------------------------------------------------
/// Клиент присоединяет данные к указанному файлу
void doClientAppend(int s, const RpcHeader* const hdr, void* param) {
    AppendRequest       req;
    cl::array_ptr<char> data;
    // -
    so_readsync(s, &req, sizeof(AppendRequest), 5);
    // -
    data.reset(new char[req.bytes]);
    so_readsync(s, data.get(), req.bytes, 5);
    // -
    const FilesMap::iterator i = files.find(req.stream);
    if (i != files.end()) {
        fwrite(data.get(), 1, req.bytes, i->second->data);
        fflush(i->second->data);
    }
    // -
    TCommonResponse resp;
    resp.size = sizeof(TCommonResponse);
    resp.code = RPC_APPEND;
    resp.err  = 0;

    so_sendsync(s, &resp, sizeof(resp));
}

//------------------------------------------------------------------------------
void doClientDisconnected(int s, void* param) {
    printf("doClientDisconnected\n");

    TClientInfo* const info = static_cast<TClientInfo*>(param);
    {
        const ClientsMap::iterator i = clients.find(info->sid);
        if (i != clients.end()) {
            clients.erase(i);
            delete info;
        }
    }
    return;
}
