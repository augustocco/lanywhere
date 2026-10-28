#include "lan_discovery.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "ipinfo.hpp"
#include "lanywhere_relay.hpp"

namespace ams::mitm::ldn {

    static const int ModuleID = 0xFD;
    static const int LdnModuleId = 0xCB;

    const char *LANDiscovery::FakeSsid = "12345678123456781234567812345678";
    const LANDiscovery::LanEventFunc LANDiscovery::EmptyFunc = [](){};


    int LanStation::onRead() {
        if (!this->socket) {
            LogFormat("Nullptr %d\n", this->nodeId);
            return -1;
        }
        return this->socket->recvPacket([&](LANPacketType type, const void *data, size_t size, ReplyFunc reply) -> int {
			AMS_UNUSED(reply);
            if (type == LANPacketType::Connect) {
                LogFormat("on connect");
                NodeInfo *info = (decltype(info))data;
                if (size != sizeof(*info)) {
                    LogFormat("NodeInfo size is wrong");
                    return -1;
                }
                *this->nodeInfo = *info;
                this->status = NodeStatus::Connected;

                this->discovery->updateNodes();
            } else {
                LogFormat("unexpecting type %d", static_cast<int>(type));
            }
            return 0;
        });
    }

    void LanStation::onClose() {
        LogFormat("LanStation::onClose %d", this->nodeId);
        this->reset();
        this->discovery->updateNodes();
    }

    LDUdpSocket::LDUdpSocket(int fd, LANDiscovery *discovery)
        :   UdpLanSocketBase(fd, discovery->getListenPort()),
            discovery(discovery) {
        /* ... */
    }

    int LDUdpSocket::onRead() {
        LogFormat("LDUdpSocket::onRead");
        return this->recvPacket([&](LANPacketType type, const void *data, size_t size, ReplyFunc reply) -> int {
            switch (type) {
                case LANPacketType::Scan: {
                    if (this->discovery->getState() == CommState::AccessPointCreated) {
                        reply(LANPacketType::ScanResp, &this->discovery->networkInfo, sizeof(NetworkInfo));
                    }
                    break;
                }
                case LANPacketType::ScanResp: {
                    LogFormat("ScanResp");
                    NetworkInfo *info = (decltype(info))data;
                    if (size != sizeof(*info)) {
                        break;
                    }
                    this->scanResults.insert({info->common.bssid, *info});
                    break;
                }
                default: {
                    LogFormat("LDUdpSocket::onRead unhandle type %d", static_cast<int>(type));
                    break;
                }
            }
            return 0;
        });
    }

    void LDUdpSocket::onClose() {
        discovery->disconnect_reason = DisconnectReason::SignalLost;
        discovery->setState(CommState::Error);
    };

    int LDTcpSocket::onRead() {
        LogFormat("LDTcpSocket::onRead");
        const auto state = this->discovery->getState();
        if (state == CommState::Station || state == CommState::StationConnected) {
            return this->recvPacket([&](LANPacketType type, const void *data, size_t size, ReplyFunc reply) -> int {
				AMS_UNUSED(reply);
                if (type == LANPacketType::SyncNetwork) {
                    LogFormat("SyncNetwork");
                    NetworkInfo *info = (decltype(info))data;
                    if (size != sizeof(*info)) {
                        return -1;
                    }

                    this->discovery->onSyncNetwork(info);
                } else {
                    LogFormat("LDTcpSocket::onRead unhandle type %d", static_cast<int>(type));
                    return -1;
                }

                return 0;
            });
        } else if (state == CommState::AccessPointCreated) {
            struct sockaddr_in addr;
            socklen_t addrlen = sizeof(addr);
            int new_fd = accept(this->getFd(), (struct sockaddr *)&addr, &addrlen);
            if (new_fd < 0)
            {
                LogFormat("accept failed");
                return -1;
            }
            this->discovery->onConnect(new_fd);
            return 0;
        } else {
            LogFormat("LDTcpSocket::onRead wrong state %d", static_cast<int>(state));
            return -1;
        }
    }

    void LDTcpSocket::onClose() {
        LogFormat("LDTcpSocket::onClose");
        this->discovery->onDisconnectFromHost();
    }

    u32 LDUdpSocket::getBroadcast() {
        u32 address, netmask, gateway, primary_dns, secondary_dns;
        Result rc = nifmGetCurrentIpConfigInfo(&address, &netmask, &gateway, &primary_dns, &secondary_dns);
        address = ntohl(address);
        netmask = ntohl(netmask);
        if (R_FAILED(rc)) {
            LogFormat("Broadcast failed to get ip");
            return 0xFFFFFFFF;
        }
        u32 ret = address | ~netmask;
        return ret;
    }

    void LANDiscovery::onSyncNetwork(NetworkInfo *info) {
        this->networkInfo = *info;
        if (this->state == CommState::Station) {
            this->setState(CommState::StationConnected);
        }
        this->onNetworkInfoChanged();
    }

    void LANDiscovery::onConnect(int new_fd) {
        LogFormat("Accepted %d", new_fd);
        if (this->stationCount() >= StationCountMax) {
            LogFormat("Close new_fd. stations are full");
            close(new_fd);
            return;
        }

        bool found = false;
        for (auto &i : this->stations) {
            if (i.getStatus() == NodeStatus::Disconnected) {
                i.link(new_fd);
                found = true;
                break;
            }
        }

        if (!found) {
            LogFormat("Close new_fd. no free station found");
            close(new_fd);
        }
    }

    void LANDiscovery::onDisconnectFromHost() {
        LogFormat("onDisconnectFromHost state: %d", static_cast<int>(this->state));
        if (this->state == CommState::StationConnected) {
            this->setState(CommState::Station);
        }
    }

    void LANDiscovery::onNetworkInfoChanged() {
        if (this->isNodeStateChanged()) {
            this->lanEvent();
        }
        return;
    }

    Result LANDiscovery::setAdvertiseData(const u8 *data, uint16_t size) {
        if (size > AdvertiseDataSizeMax) {
            return MAKERESULT(ModuleID, 10);
        }

        if (size > 0 && data != nullptr) {
            std::memcpy(this->networkInfo.ldn.advertiseData, data, size);
        } else {
            LogFormat("LANDiscovery::setAdvertiseData data %p size %lu", data, size);
        }
        this->networkInfo.ldn.advertiseDataSize = size;

        this->updateNodes();

        return 0;
    }

    Result LANDiscovery::initNetworkInfo() {
        Result rc = getFakeMac(&this->networkInfo.common.bssid);
        if (R_FAILED(rc)) {
            return rc;
        }
        this->networkInfo.common.channel = 6;
        this->networkInfo.common.linkLevel = 3;
        this->networkInfo.common.networkType = 2;
        this->networkInfo.common.ssid = FakeSsid;

        auto nodes = this->networkInfo.ldn.nodes;
        for (int i = 0; i < NodeCountMax; i++) {
            nodes[i].nodeId = i;
            nodes[i].isConnected = 0;
        }

        return 0;
    }

    Result LANDiscovery::getFakeMac(MacAddress *mac) {
        mac->raw[0] = 0x02;
        mac->raw[1] = 0x00;

        u32 ip;
        Result rc = 0;
        if (lanywhere::InRoom()) {
            // 02:xx:xx:xx:ip:ip, con xx de la sala: cada grupo abierto tiene un BSSID distinto.
            const u32 tag = lanywhere::RoomTag();
            memcpy(mac->raw + 1, &tag, sizeof(tag));
            const u32 my_ip = lanywhere::MyIp();
            mac->raw[4] = (my_ip >> 8) & 0xFF;
            mac->raw[5] = my_ip & 0xFF;
            return 0;
        } else if (R_SUCCEEDED(rc = nifmGetCurrentIpAddress(&ip))) {
            ip = ntohl(ip);
        }
        if (R_SUCCEEDED(rc)) {
            memcpy(mac->raw + 2, &ip, sizeof(ip));
        }

        return rc;
    }

    Result LANDiscovery::setSocketOpts(int fd) {
        int rc;

        {
            int b = 1;
            rc = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &b, sizeof(b));
            if (rc != 0) {
                return MAKERESULT(ModuleID, 4);
            }
        }
        {
            int yes = 1;
            rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (rc != 0) {
                // return MAKERESULT(ModuleID, 5);
                LogFormat("SO_REUSEADDR failed");
            }
        }

        return 0;
    }

    Result LANDiscovery::initTcp(bool listening) {
        int fd;
        Result rc;
        struct sockaddr_in addr;
        std::scoped_lock lock(this->pollMutex);

        if (this->tcp) {
            this->tcp->close();
        }
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return MAKERESULT(ModuleID, 6);
        }
        auto tcpSocket = std::make_unique<LDTcpSocket>(fd, this);

        if (listening) {
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htons(INADDR_ANY);
            addr.sin_port = htons(listenPort);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                return MAKERESULT(ModuleID, 7);
            }
            if (listen(fd, 10) != 0) {
                return MAKERESULT(ModuleID, 8);
            }
        }
        rc = setSocketOpts(fd);
        if (R_FAILED(rc)) {
            return rc;
        }

        this->tcp = std::move(tcpSocket);

        return 0;
    }

    Result LANDiscovery::initUdp(bool listening) {
        int fd;
        Result rc;
        struct sockaddr_in addr;
        std::scoped_lock lock(this->pollMutex);

        if (this->udp) {
            this->udp->close();
        }
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return MAKERESULT(ModuleID, 1);
        }
        auto udpSocket = std::make_unique<LDUdpSocket>(fd, this);

        if (listening) {
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htons(INADDR_ANY);
            addr.sin_port = htons(listenPort);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                return MAKERESULT(ModuleID, 2);
            }
        }
        rc = setSocketOpts(fd);
        if (R_FAILED(rc)) {
            return rc;
        }

        this->udp = std::move(udpSocket);

        return 0;
    }

    void LANDiscovery::initNodeStateChange() {
        for (auto &i : this->nodeChanges) {
            i.stateChange = NodeStateChange_None;
        }
        for (auto &i : this->nodeLastStates) {
            i = 0;
        }
    }

    bool LANDiscovery::isNodeStateChanged() {
        bool changed = false;
        const auto &nodes = this->networkInfo.ldn.nodes;
        for (int i = 0; i < NodeCountMax; i++) {
            if (nodes[i].isConnected != this->nodeLastStates[i]) {
                if (nodes[i].isConnected) {
                    this->nodeChanges[i].stateChange |= NodeStateChange_Connect;
                } else {
                    this->nodeChanges[i].stateChange |= NodeStateChange_Disconnect;
                }
                this->nodeLastStates[i] = nodes[i].isConnected;
                changed = true;
            }
        }
        return changed;
    }

    void LANDiscovery::Worker(void* args) {
        LANDiscovery* self = (LANDiscovery*)args;

        self->worker();
    }

    Result LANDiscovery::scan(NetworkInfo *pOutNetwork, u16 *count, ScanFilter filter) {
        this->udp->scanResults.clear();

        if (lanywhere::InRoom()) {
            lanywhere::SendControl(lanywhere::AllMembers, lanywhere::LdnMsg::Scan, nullptr, 0);
        } else {
            if (this->udp->sendBroadcast(LANPacketType::Scan) < 0) {
                return MAKERESULT(ModuleID, 20);
            }
            // LANywhere: y los grupos abiertos de este juego en el servidor.
            this->publicGroupCount = 0;
            if (lanywhere::PublicEnabled()) {
                const u64 game = (filter.flag & ScanFilterFlag_LocalCommunicationId) ? filter.networkId.intentId.localCommunicationId : 0;
                lanywhere::FindGroups(game, &LANDiscovery::onPublicGroup, this, false);
                this->keepPreferredGroup();
            }
        }

        svcSleepThread(1000000000L); // 1sec

        int i = 0;
        for (auto& item : this->udp->scanResults) {
            if (i >= *count) {
                break;
            }
            auto &info = item.second;

            bool copy = true;
            // filter
            if (filter.flag & ScanFilterFlag_LocalCommunicationId) {
                copy &= filter.networkId.intentId.localCommunicationId == info.networkId.intentId.localCommunicationId;
            }
            if (filter.flag & ScanFilterFlag_SessionId) {
                copy &= filter.networkId.sessionId == info.networkId.sessionId;
            }
            if (filter.flag & ScanFilterFlag_NetworkType) {
                copy &= filter.networkType == info.common.networkType;
            }
            if (filter.flag & ScanFilterFlag_Ssid) {
                copy &= filter.ssid == info.common.ssid;
            }
            if (filter.flag & ScanFilterFlag_SceneId) {
                copy &= filter.networkId.intentId.sceneId == info.networkId.intentId.sceneId;
            }

            if (copy) {
                pOutNetwork[i++] = info;
            }
        }
        *count = i;

        return 0;
    }

    void LANDiscovery::resetStations() {
        for (auto &i : this->stations) {
            i.reset();
        }
    }

    int LANDiscovery::stationCount() {
        int count = 0;

        for (auto const &i : this->stations) {
            if (i.getStatus() != NodeStatus::Disconnected) {
                count++;
            }
        }

        return count;
    }

    void LANDiscovery::updateNodes() {
        int count = 0;
        for (auto &i : this->stations) {
            bool connected = i.getStatus() == NodeStatus::Connected;
            if (connected) {
                count++;
            }
            i.overrideInfo();
        }
        this->networkInfo.ldn.nodeCount = count + 1;

        if (this->autoRoom && this->state == CommState::AccessPointCreated) {
            lanywhere::Publish(&this->networkInfo, sizeof(this->networkInfo));
        }
        for (auto &i : stations) {
            if (i.getStatus() == NodeStatus::Connected && i.member >= 0) {
                lanywhere::SendControl(i.member, lanywhere::LdnMsg::Sync, &this->networkInfo, sizeof(this->networkInfo));
            } else if (i.getStatus() == NodeStatus::Connected) {
                int ret = i.sendPacket(LANPacketType::SyncNetwork, &this->networkInfo, sizeof(this->networkInfo));
                if (ret < 0) {
                    LogFormat("Failed to sendTcp");
                }
            }
        }

        this->onNetworkInfoChanged();
    }

    int LANDiscovery::loopPoll() {
        int rc;
        if (!initialized) {
            return 0;
        }

        std::scoped_lock lock(this->pollMutex);
        int nfds = 2 + StationCountMax;
        Pollable *fds[nfds];
        fds[0] = this->udp.get();
        fds[1] = this->tcp.get();
        for (int i = 0; i < StationCountMax; i++) {
            fds[2 + i] = this->stations.data() + i;
        }
        rc = Pollable::Poll(fds, nfds);
        this->pollRoom();

        return rc;
    }

    // LANywhere: con una sala activa, la búsqueda, la unión y la sincronización del grupo van entre miembros
    // de la sala por el servidor en lugar de por broadcast UDP y TCP en la red local.
    void LANDiscovery::pollRoom() {
        static u8 buffer[lanywhere::MaxControl];
        u8 from;
        lanywhere::LdnMsg msg;
        ssize_t size;
        while ((size = lanywhere::TakeControl(&from, &msg, buffer, sizeof(buffer))) >= 0) {
            if (static_cast<size_t>(size) <= sizeof(buffer)) {
                this->onRoomMessage(from, static_cast<u8>(msg), buffer, size);
            }
        }
    }

    void LANDiscovery::onRoomMessage(u8 from, u8 msg, const void *data, size_t size) {
        lanywhere::Log("ldn: mensaje %u de %u (%zu bytes), estado %d", msg, from, size, static_cast<int>(this->state));
        switch (static_cast<lanywhere::LdnMsg>(msg)) {
            case lanywhere::LdnMsg::Scan: {
                if (this->state == CommState::AccessPointCreated) {
                    lanywhere::SendControl(from, lanywhere::LdnMsg::ScanResp, &this->networkInfo, sizeof(NetworkInfo));
                }
                break;
            }
            case lanywhere::LdnMsg::ScanResp: {
                if (size == sizeof(NetworkInfo) && this->udp) {
                    const NetworkInfo *info = static_cast<const NetworkInfo *>(data);
                    this->udp->scanResults.insert({info->common.bssid, *info});
                }
                break;
            }
            case lanywhere::LdnMsg::Connect: {
                if (this->state != CommState::AccessPointCreated || size != sizeof(NodeInfo)) {
                    break;
                }
                LanStation *slot = nullptr;
                for (auto &i : this->stations) {
                    if (i.member == from) { slot = &i; break; }
                }
                const bool rejoin = slot != nullptr;
                for (auto &i : this->stations) {
                    if (slot == nullptr && i.getStatus() == NodeStatus::Disconnected) { slot = &i; }
                }
                if (slot == nullptr || (!rejoin && this->stationCount() >= this->networkInfo.ldn.nodeCountMax - 1)) {
                    lanywhere::SendControl(from, lanywhere::LdnMsg::Disconnect, nullptr, 0);
                    break;
                }
                slot->member = from;
                *slot->nodeInfo = *static_cast<const NodeInfo *>(data);
                slot->status = NodeStatus::Connected;
                this->updateNodes();
                break;
            }
            case lanywhere::LdnMsg::Sync: {
                if ((this->state == CommState::Station || this->state == CommState::StationConnected) &&
                    from == this->hostMember && size == sizeof(NetworkInfo)) {
                    NetworkInfo info = *static_cast<const NetworkInfo *>(data);
                    this->onSyncNetwork(&info);
                }
                break;
            }
            case lanywhere::LdnMsg::Disconnect:
            case lanywhere::LdnMsg::MemberLeft: {
                if (this->state == CommState::AccessPointCreated) {
                    for (auto &i : this->stations) {
                        if (i.member == from) {
                            i.reset();
                            this->updateNodes();
                        }
                    }
                } else if (from == this->hostMember) {
                    this->hostMember = -1;
                    this->onDisconnectFromHost();
                }
                break;
            }
        }
    }

    // Si en la app se eligió un grupo abierto y está entre los encontrados, el juego solo ve ese (los de la red local
    // siguen apareciendo): así no hay que buscarlo en la lista del juego.
    void LANDiscovery::keepPreferredGroup() {
        char preferred[lanywhere::CodeLength + 1];
        if (!lanywhere::PreferredGroup(preferred, sizeof(preferred)) || !this->udp) {
            return;
        }
        size_t keep = this->publicGroupCount;
        for (size_t i = 0; i < this->publicGroupCount; i++) {
            if (std::strcmp(this->publicGroups[i].code, preferred) == 0) {
                keep = i;
            }
        }
        if (keep == this->publicGroupCount) {
            return;  // ya no está abierto: se muestran todos
        }
        for (size_t i = 0; i < this->publicGroupCount; i++) {
            if (i != keep) {
                this->udp->scanResults.erase(this->publicGroups[i].bssid);
            }
        }
        this->publicGroups[0] = this->publicGroups[keep];
        this->publicGroupCount = 1;
        lanywhere::Log("ldn: se muestra solo el grupo elegido (sala %s)", preferred);
    }

    void LANDiscovery::onPublicGroup(const char *code, const void *data, void *user) {
        LANDiscovery *self = static_cast<LANDiscovery *>(user);
        const NetworkInfo *info = static_cast<const NetworkInfo *>(data);
        if (self->publicGroupCount >= self->publicGroups.size() || !self->udp) {
            return;
        }
        auto &g = self->publicGroups[self->publicGroupCount++];
        g.bssid = info->common.bssid;
        util::Strlcpy(g.code, code, sizeof(g.code));
        self->udp->scanResults.insert({info->common.bssid, *info});
    }

    void LANDiscovery::leaveAutoRoom() {
        if (this->autoRoom) {
            this->autoRoom = false;
            this->hostMember = -1;
            lanywhere::LeaveRoom();
            lanywhere::Log("ldn: fuera del grupo abierto");
        }
    }

    // El anfitrión cierra el grupo: las estaciones de la sala tienen que enterarse (en LAN lo hace el cierre del TCP).
    void LANDiscovery::roomCloseStations() {
        if (this->state != CommState::AccessPointCreated) {
            return;
        }
        for (auto &i : this->stations) {
            if (i.member >= 0) {
                lanywhere::SendControl(i.member, lanywhere::LdnMsg::Disconnect, nullptr, 0);
            }
        }
    }

    LANDiscovery::~LANDiscovery() {
        LogFormat("~LANDiscovery");
        if (this->initialized) {
            LogFormat("finalize not called");
            Result rc = this->finalize();
            LogFormat("finalize: %d", rc);
        }
    }

    void LANDiscovery::worker() {
        this->stop = false;
        while (!this->stop) {

            int rc = loopPoll();
            if (rc < 0) {
                break;
            }
            svcSleepThread(10000000L); // 10ms
        }
        LogFormat("Worker exit");
    }

    Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork) {
        Result rc = 0;

        if (this->state == CommState::AccessPointCreated || this->state == CommState::StationConnected) {
            std::memcpy(pOutNetwork, &networkInfo, sizeof(networkInfo));
        } else {
            rc = MAKERESULT(LdnModuleId, 32);
        }

        return rc;
    }

    Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork, NodeLatestUpdate *pOutUpdates, int bufferCount) {
        Result rc = 0;

        if (bufferCount < 0 || bufferCount > NodeCountMax) {
            return MAKERESULT(ModuleID, 50);
        }

        if (this->state == CommState::AccessPointCreated || this->state == CommState::StationConnected) {
            std::memcpy(pOutNetwork, &networkInfo, sizeof(networkInfo));

            char str[10] = {0};
            for (int i = 0; i < bufferCount; i++) {
                pOutUpdates[i].stateChange = nodeChanges[i].stateChange;
                nodeChanges[i].stateChange = NodeStateChange_None;
                str[i] = '0' + pOutUpdates[i].stateChange;
            }
            LogFormat("getNetworkInfo updates %s", str);
        } else {
            rc = MAKERESULT(LdnModuleId, 32);
        }

        return rc;
    }

    Result LANDiscovery::getNodeInfo(NodeInfo *node, const UserConfig *userConfig, u16 localCommunicationVersion) {
        u32 ipAddress;
        Result rc = 0;
        if (lanywhere::InRoom()) {
            ipAddress = lanywhere::MyIp();
        } else {
            rc = nifmGetCurrentIpAddress(&ipAddress);
            if (R_FAILED(rc))
            {
                return rc;
            }
            ipAddress = ntohl(ipAddress);
        }
        rc = getFakeMac(&node->macAddress);
        if (R_FAILED(rc)) {
            return rc;
        }

        node->isConnected = 1;
        // LANywhere: en una sala, los demás ven el apodo, nunca el nombre de usuario de la consola.
        if (lanywhere::InRoom()) {
            memset(node->userName, 0, sizeof(node->userName));
            lanywhere::Nickname(node->userName, sizeof(node->userName));
        } else {
            strcpy(node->userName, userConfig->userName);
        }
        node->localCommunicationVersion = localCommunicationVersion;
        node->ipv4Address = ipAddress;

        return 0;
    }

    Result LANDiscovery::createNetwork(const SecurityConfig *securityConfig, const UserConfig *userConfig, const NetworkConfig *networkConfig) {
        LogFormat("SecurityConfig");
        LogHex(securityConfig->passphrase, securityConfig->passphraseSize);
        Result rc = 0;

        if (this->state != CommState::AccessPoint) {
            return MAKERESULT(LdnModuleId, 32);
        }

        // LANywhere: sin sala privada, el grupo se abre a todos en una sala nueva del servidor. Si el servidor
        // no responde, el grupo queda solo en la red local, como siempre.
        if (!lanywhere::InRoom() && lanywhere::PublicEnabled()) {
            this->autoRoom = R_SUCCEEDED(lanywhere::CreateRoom(networkConfig->intentId.localCommunicationId));
            lanywhere::Log("ldn: grupo abierto %s", this->autoRoom ? "publicado" : "solo en la red local");
        }

        rc = this->initTcp(true);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = this->initNetworkInfo();
        if (R_FAILED(rc)) {
            return rc;
        }
        this->networkInfo.ldn.nodeCountMax = networkConfig->nodeCountMax;
        this->networkInfo.ldn.securityMode = securityConfig->securityMode;

        if (networkConfig->channel == 0) {
            this->networkInfo.common.channel = 6;
        } else {
            this->networkInfo.common.channel = networkConfig->channel;
        }

        ams::os::GenerateRandomBytes(&this->networkInfo.networkId.sessionId, sizeof(SessionId));
        this->networkInfo.networkId.intentId = networkConfig->intentId;

        NodeInfo *node0 = &this->networkInfo.ldn.nodes[0];
        rc = this->getNodeInfo(node0, userConfig, networkConfig->localCommunicationVersion);
        if (R_FAILED(rc)) {
            return rc;
        }

        this->setState(CommState::AccessPointCreated);

        this->initNodeStateChange();
        node0->isConnected = 1;
        this->updateNodes();

        return rc;
    }

    Result LANDiscovery::destroyNetwork() {
        this->roomCloseStations();
        this->leaveAutoRoom();
        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::AccessPoint);

        return 0;
    }

    Result LANDiscovery::disconnect() {
        if (this->hostMember >= 0) {
            lanywhere::SendControl(this->hostMember, lanywhere::LdnMsg::Disconnect, nullptr, 0);
            this->hostMember = -1;
        }
        this->leaveAutoRoom();
        if (this->tcp) {
            this->tcp->close();
        }
        this->setState(CommState::Station);

        return 0;
    }

    Result LANDiscovery::openAccessPoint() {
        this->disconnect_reason = DisconnectReason::None;
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->roomCloseStations();
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::AccessPoint);

        return 0;
    }

    Result LANDiscovery::closeAccessPoint() {
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->roomCloseStations();
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::Initialized);

        return 0;
    }

    Result LANDiscovery::openStation() {
        this->disconnect_reason = DisconnectReason::None;
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::Station);

        return 0;
    }

    Result LANDiscovery::closeStation() {
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::Initialized);

        return 0;
    }

    Result LANDiscovery::connect(const NetworkInfo *networkInfo, UserConfig *userConfig, u16 localCommunicationVersion) {
        if (networkInfo->ldn.nodeCount == 0) {
            return MAKERESULT(ModuleID, 30);
        }

        if (!lanywhere::InRoom()) {
            for (size_t i = 0; i < this->publicGroupCount; i++) {
                if (this->publicGroups[i].bssid == networkInfo->common.bssid) {
                    const auto &g = this->publicGroups[i];
                    if (R_FAILED(lanywhere::JoinRoom(g.code, networkInfo->networkId.intentId.localCommunicationId))) {
                        lanywhere::Log("ldn: no se pudo entrar en la sala %s", g.code);
                        return MAKERESULT(ModuleID, 31);
                    }
                    this->autoRoom = true;
                    Result rc = this->connectRoom(networkInfo, userConfig, localCommunicationVersion);
                    if (R_FAILED(rc)) {
                        this->leaveAutoRoom();
                    } else {
                        lanywhere::PreferGroup("");  // ya está dentro: la próxima búsqueda vuelve a mostrarlos todos
                    }
                    return rc;
                }
            }
        }
        if (lanywhere::InRoom()) {
            return this->connectRoom(networkInfo, userConfig, localCommunicationVersion);
        }
        u32 hostIp = networkInfo->ldn.nodes[0].ipv4Address;
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(hostIp);
        addr.sin_port = htons(listenPort);
        LogFormat("connect hostIp %x", hostIp);

        Result rc = this->initTcp(false);
        if (R_FAILED(rc)) {
            return rc;
        }

        int ret = ::connect(this->tcp->getFd(), (struct sockaddr *)&addr, sizeof(addr));
        if (ret != 0) {
            LogFormat("connect failed");
            return MAKERESULT(ModuleID, 31);
        }

        NodeInfo myNode = {0};
        rc = this->getNodeInfo(&myNode, userConfig, localCommunicationVersion);
        if (R_FAILED(rc)) {
            return rc;
        }
        ret = this->tcp->sendPacket(LANPacketType::Connect, &myNode, sizeof(myNode));
        if (ret < 0) {
            LogFormat("sendPacket failed");
            return MAKERESULT(ModuleID, 32);
        }
        this->initNodeStateChange();

        svcSleepThread(1000000000L); // 1sec

        return 0;
    }

    // LANywhere: unirse al grupo de otro miembro de la sala. Espera su Sync como mucho 3 s.
    Result LANDiscovery::connectRoom(const NetworkInfo *networkInfo, UserConfig *userConfig, u16 localCommunicationVersion) {
        const int host = lanywhere::MemberOfIp(networkInfo->ldn.nodes[0].ipv4Address);
        lanywhere::Log("ldn: unirse al grupo del miembro %d", host);
        if (host < 0) {
            return MAKERESULT(ModuleID, 31);
        }
        NodeInfo myNode = {0};
        Result rc = this->getNodeInfo(&myNode, userConfig, localCommunicationVersion);
        if (R_FAILED(rc)) {
            return rc;
        }
        this->initNodeStateChange();
        this->hostMember = host;
        if (!lanywhere::SendControl(host, lanywhere::LdnMsg::Connect, &myNode, sizeof(myNode))) {
            this->hostMember = -1;
            return MAKERESULT(ModuleID, 32);
        }
        for (int i = 0; i < 30 && this->state != CommState::StationConnected; i++) {
            svcSleepThread(100000000L); // 100ms
        }
        if (this->state != CommState::StationConnected) {
            this->hostMember = -1;
            return MAKERESULT(ModuleID, 31);
        }
        return 0;
    }

    Result LANDiscovery::finalize() {
        Result rc = 0;

        if (this->initialized) {
            this->roomCloseStations();
            this->leaveAutoRoom();
            this->stop = true;
            os::WaitThread(&this->workerThread);
            os::DestroyThread(&this->workerThread);
            this->udp.reset();
            this->tcp.reset();
            this->resetStations();
            this->initialized = false;

            rc = nifmRequestCancel(&request);
            if (R_FAILED(rc))
            {
                LogFormat("final nifmRequestCancel failed: %x", rc);
            }

            NifmNetworkProfileData networkProfile;
            rc = nifmGetCurrentNetworkProfile(&networkProfile);
            if (R_FAILED(rc))
            {
                LogFormat("final nifmGetCurrentProfile failed: %x", rc);
            }

            if (networkProfile.ip_setting_data.mtu != originalMtu) {
                networkProfile.ip_setting_data.mtu = originalMtu;
                rc = nifmSetNetworkProfile(&networkProfile, &networkProfile.uuid);
                if (R_FAILED(rc)) {
                    LogFormat("final nifmSetNetworkProfile failed: %x", rc);
                }
            }
        }

        this->setState(CommState::None);

        return rc;
    }

    Result LANDiscovery::initialize(LanEventFunc lanEvent, bool listening) {
        if (this->initialized)
        {
            return 0;
        }

        NifmNetworkProfileData networkProfile;
        Result rc = nifmGetCurrentNetworkProfile(&networkProfile);
        if (R_FAILED(rc))
        {
            LogFormat("nifmGetCurrentNetworkProfile failed: %x", rc);
            return rc;
        }

        originalMtu = networkProfile.ip_setting_data.mtu;
        // Cambiar el perfil reconecta la red (y corta la conexión de LANywhere con el servidor): solo si hace falta.
        if (originalMtu != 1500) {
            networkProfile.ip_setting_data.mtu = 1500;

            rc = nifmSetNetworkProfile(&networkProfile, &networkProfile.uuid);
            if (R_FAILED(rc))
            {
                LogFormat("nifmSetNetworkProfile failed: %x", rc);
                return rc;
            }
        }

        rc = nifmCreateRequest(&request, true);
        if (R_FAILED(rc))
        {
            LogFormat("nifmCreateRequest failed: %x", rc);
            return rc;
        }

        rc = nifmSetLocalNetworkMode(&request, true);
        if (R_FAILED(rc)) {
            LogFormat("nifmSetLocalNetworkMode failed %x", rc);
            return rc;
        }

        rc = nifmRequestSubmitAndWait(&request);
        if (R_FAILED(rc))
        {
            LogFormat("nifmRequestSubmitAndWait failed: %x", rc);
            return rc;
        }

        for (auto &i : stations) {
            i.discovery = this;
            i.nodeInfo = &this->networkInfo.ldn.nodes[i.nodeId];
            i.reset();
        }

        this->lanEvent = lanEvent;
        rc = this->initUdp(listening);
        if (R_FAILED(rc)) {
            LogFormat("initUdp %x", rc);
            return rc;
        }

        rc = os::CreateThread(&this->workerThread, &Worker, this, reinterpret_cast<void *>(util::AlignUp(reinterpret_cast<uintptr_t>(stack.get()), os::ThreadStackAlignment)), StackSize, 0x15, 2);
        if (R_FAILED(rc)) {
            LogFormat("LANDiscovery Failed to threadCreate: %x", rc);
            return 0xF601;
        }

        os::StartThread(&this->workerThread);
        this->setState(CommState::Initialized);

        this->initialized = true;
        return 0;
    }
}
