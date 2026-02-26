#include "ble_meshtastic.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp_bt.h>
#endif
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <globals.h>

#include <algorithm>
#include <time.h>
#include <vector>

#if __has_include(<NimBLEExtAdvertising.h>)
#define NIMBLE_V2_PLUS 1
#endif

namespace {

constexpr uint32_t MESH_BROADCAST = 0xFFFFFFFFu;
constexpr uint32_t MESH_PORT_TEXT = 1;
constexpr uint8_t MESH_PRIMARY_CHANNEL = 0;
constexpr uint8_t MESH_MAX_CHANNELS = 32;
constexpr uint8_t MESH_CHANNEL_ROLE_DISABLED = 0;
constexpr uint8_t MESH_CHANNEL_ROLE_PRIMARY = 1;
constexpr uint8_t MESH_CHANNEL_ROLE_SECONDARY = 2;
constexpr uint8_t PB_WT_VARINT = 0;
constexpr uint8_t PB_WT_FIXED64 = 1;
constexpr uint8_t PB_WT_LEN = 2;
constexpr uint8_t PB_WT_FIXED32 = 5;

static NimBLEUUID kMeshServiceUUID("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
static NimBLEUUID kToRadioUUID("f75c76d2-129e-4dad-a1dd-7866124401e7");
static NimBLEUUID kFromRadioUUID("2c55e69e-4993-11ed-b878-0242ac120002");
static NimBLEUUID kFromNumUUID("ed9da18c-a800-4f66-a670-aa7547e34453");

struct MeshNodeInfo {
    uint32_t num = 0;
    String id;
    String longName;
    String shortName;
    uint32_t lastHeard = 0;
    int32_t hopsAway = -1;
};

struct MeshChatMessage {
    uint32_t from = 0;
    uint32_t to = 0;
    uint8_t channel = MESH_PRIMARY_CHANNEL;
    uint32_t id = 0;
    uint32_t rxTime = 0;
    String text;
};

struct MeshChannelInfo {
    uint8_t index = MESH_PRIMARY_CHANNEL;
    uint8_t role = MESH_CHANNEL_ROLE_DISABLED;
    String name;
    uint32_t lastUpdate = 0;
};

struct MeshScanDevice {
    NimBLEAddress address;
    String name;
    int rssi = 0;
};

static bool g_meshInit = false;
static bool g_meshConnected = false;
static bool g_meshDisconnected = true;
static bool g_pendingFromRadio = false;
static bool g_configCompleted = false;
static bool g_authComplete = false;
static bool g_authOk = false;
static bool g_pairingRequested = false;
static bool g_waitPasskeyInput = false;
static bool g_waitPasskeyConfirm = false;
static uint32_t g_lastConfigRequestId = 0;
static uint32_t g_myNodeNum = 0;
static uint32_t g_rxFrames = 0;
static uint32_t g_confirmPin = 0;

static NimBLEScan *g_scan = nullptr;
static NimBLEClient *g_client = nullptr;
static NimBLERemoteCharacteristic *g_toRadioChr = nullptr;
static NimBLERemoteCharacteristic *g_fromRadioChr = nullptr;
static NimBLERemoteCharacteristic *g_fromNumChr = nullptr;

static std::vector<MeshNodeInfo> g_nodes;
static std::vector<MeshChatMessage> g_chatMessages;
static std::vector<MeshChannelInfo> g_channels;
static uint8_t g_activeChannel = MESH_PRIMARY_CHANNEL;

static String meshNodeLabel(uint32_t nodeNum);
static String meshChannelShortLabel(uint8_t channelIndex);
static String meshChannelLabel(uint8_t channelIndex);
static void meshPollIncoming(uint8_t maxFrames = 16);
static void meshResetBleState();
static void meshForceBleRecovery();
static void meshReleaseWifiMemory();
static bool meshStartBlockingScan(uint32_t durationMs);
static bool meshHandlePasskeyEntry();
static bool meshHandlePasskeyConfirm();

class MeshClientCallbacks : public NimBLEClientCallbacks {
public:
    void onConnectFail(NimBLEClient *, int) override {
        g_meshConnected = false;
        g_meshDisconnected = true;
        BLEConnected = false;
    }

#ifdef NIMBLE_V2_PLUS
    void onDisconnect(NimBLEClient *, int) override {
#else
    void onDisconnect(NimBLEClient *) override {
#endif
        g_meshConnected = false;
        g_meshDisconnected = true;
        BLEConnected = false;
    }

    void onPassKeyEntry(NimBLEConnInfo &) override {
        g_pairingRequested = true;
        g_waitPasskeyInput = true;
    }

    void onConfirmPasskey(NimBLEConnInfo &, uint32_t pin) override {
        g_pairingRequested = true;
        g_waitPasskeyConfirm = true;
        g_confirmPin = pin;
    }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
        g_authComplete = true;
        g_authOk = connInfo.isEncrypted() || connInfo.isAuthenticated();
    }
};

static MeshClientCallbacks g_clientCallbacks;

static bool pbReadVarint(const uint8_t *data, size_t len, size_t &pos, uint64_t &value) {
    value = 0;
    uint8_t shift = 0;
    while (pos < len && shift < 64) {
        const uint8_t byte = data[pos++];
        value |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

static bool pbReadFixed32(const uint8_t *data, size_t len, size_t &pos, uint32_t &value) {
    if (pos + 4 > len) return false;
    value = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) | ((uint32_t)data[pos + 2] << 16)
            | ((uint32_t)data[pos + 3] << 24);
    pos += 4;
    return true;
}

static bool pbReadKey(const uint8_t *data, size_t len, size_t &pos, uint32_t &field, uint8_t &wireType) {
    uint64_t key = 0;
    if (!pbReadVarint(data, len, pos, key)) return false;
    field = (uint32_t)(key >> 3);
    wireType = (uint8_t)(key & 0x07);
    return true;
}

static bool pbReadLenField(
    const uint8_t *data, size_t len, size_t &pos, const uint8_t *&fieldData, size_t &fieldLen
) {
    uint64_t l = 0;
    if (!pbReadVarint(data, len, pos, l)) return false;
    if (pos + l > len) return false;
    fieldData = data + pos;
    fieldLen = (size_t)l;
    pos += l;
    return true;
}

static bool pbSkipField(const uint8_t *data, size_t len, size_t &pos, uint8_t wireType) {
    uint64_t v = 0;
    switch (wireType) {
        case PB_WT_VARINT:
            return pbReadVarint(data, len, pos, v);
        case PB_WT_FIXED64:
            if (pos + 8 > len) return false;
            pos += 8;
            return true;
        case PB_WT_LEN: {
            const uint8_t *unused = nullptr;
            size_t unusedLen = 0;
            return pbReadLenField(data, len, pos, unused, unusedLen);
        }
        case PB_WT_FIXED32:
            if (pos + 4 > len) return false;
            pos += 4;
            return true;
        default:
            return false;
    }
}

static void pbWriteVarint(std::vector<uint8_t> &out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back((uint8_t)((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back((uint8_t)value);
}

static void pbWriteKey(std::vector<uint8_t> &out, uint32_t field, uint8_t wireType) {
    pbWriteVarint(out, ((uint64_t)field << 3) | wireType);
}

static void pbWriteFixed32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back((uint8_t)(value & 0xFF));
    out.push_back((uint8_t)((value >> 8) & 0xFF));
    out.push_back((uint8_t)((value >> 16) & 0xFF));
    out.push_back((uint8_t)((value >> 24) & 0xFF));
}

static void pbWriteFixed32Field(std::vector<uint8_t> &out, uint32_t field, uint32_t value) {
    pbWriteKey(out, field, PB_WT_FIXED32);
    pbWriteFixed32(out, value);
}

static void pbWriteVarintField(std::vector<uint8_t> &out, uint32_t field, uint64_t value) {
    pbWriteKey(out, field, PB_WT_VARINT);
    pbWriteVarint(out, value);
}

static void pbWriteBytesField(std::vector<uint8_t> &out, uint32_t field, const uint8_t *data, size_t len) {
    pbWriteKey(out, field, PB_WT_LEN);
    pbWriteVarint(out, len);
    out.insert(out.end(), data, data + len);
}

static String pbTextFromBytes(const uint8_t *data, size_t len) {
    String out;
    out.reserve((int)len);
    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if ((uint8_t)c < 0x20 && c != '\n' && c != '\t') c = ' ';
        out += c;
    }
    out.trim();
    return out;
}

static String meshNodeHex(uint32_t nodeNum) {
    char buf[12];
    snprintf(buf, sizeof(buf), "!%08X", (unsigned int)nodeNum);
    return String(buf);
}

static String meshFormatClock(uint32_t unixTime) {
    if (unixTime == 0) return "--:--";
    time_t t = (time_t)unixTime;
    struct tm tmInfo;
    if (localtime_r(&t, &tmInfo) == nullptr) return "--:--";
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tmInfo);
    return String(buf);
}

static String meshFormatDateTime(uint32_t unixTime) {
    if (unixTime == 0) return "-";
    time_t t = (time_t)unixTime;
    struct tm tmInfo;
    if (localtime_r(&t, &tmInfo) == nullptr) return String(unixTime);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
    return String(buf);
}

static String meshFormatAge(uint32_t unixTime) {
    if (unixTime == 0) return "-";
    time_t now = time(nullptr);
    if (now < (time_t)unixTime || now < 1700000000) return "-";
    uint32_t diff = (uint32_t)(now - (time_t)unixTime);
    if (diff < 60) return String(diff) + "s ago";
    if (diff < 3600) return String(diff / 60) + "m ago";
    if (diff < 86400) return String(diff / 3600) + "h ago";
    return String(diff / 86400) + "d ago";
}

static MeshChannelInfo *meshFindChannel(uint8_t channelIndex) {
    for (auto &channel : g_channels) {
        if (channel.index == channelIndex) return &channel;
    }
    return nullptr;
}

static MeshChannelInfo &meshGetOrCreateChannel(uint8_t channelIndex) {
    MeshChannelInfo *existing = meshFindChannel(channelIndex);
    if (existing != nullptr) return *existing;

    if (g_channels.size() >= MESH_MAX_CHANNELS) {
        auto it = std::min_element(
            g_channels.begin(), g_channels.end(), [](const MeshChannelInfo &a, const MeshChannelInfo &b) {
                return a.lastUpdate < b.lastUpdate;
            }
        );
        if (it != g_channels.end()) {
            *it = MeshChannelInfo();
            it->index = channelIndex;
            it->lastUpdate = (uint32_t)millis();
            return *it;
        }
    }

    g_channels.push_back(MeshChannelInfo());
    g_channels.back().index = channelIndex;
    g_channels.back().lastUpdate = (uint32_t)millis();
    return g_channels.back();
}

static void meshEnsurePrimaryChannel() {
    MeshChannelInfo &primary = meshGetOrCreateChannel(MESH_PRIMARY_CHANNEL);
    if (primary.role == MESH_CHANNEL_ROLE_DISABLED) primary.role = MESH_CHANNEL_ROLE_PRIMARY;
}

static String meshChannelRoleLabel(uint8_t role) {
    if (role == MESH_CHANNEL_ROLE_PRIMARY) return "primary";
    if (role == MESH_CHANNEL_ROLE_SECONDARY) return "secondary";
    return "disabled";
}

static String meshChannelShortLabel(uint8_t channelIndex) {
    MeshChannelInfo *channel = meshFindChannel(channelIndex);
    if (channel != nullptr && !channel->name.isEmpty()) return channel->name;
    if (channelIndex == MESH_PRIMARY_CHANNEL) return "Primary";
    return "CH" + String(channelIndex);
}

static String meshChannelLabel(uint8_t channelIndex) {
    return meshChannelShortLabel(channelIndex) + " [CH" + String(channelIndex) + "]";
}

static MeshNodeInfo *meshFindNode(uint32_t nodeNum) {
    for (auto &node : g_nodes) {
        if (node.num == nodeNum) return &node;
    }
    return nullptr;
}

static MeshNodeInfo &meshGetOrCreateNode(uint32_t nodeNum) {
    MeshNodeInfo *existing = meshFindNode(nodeNum);
    if (existing != nullptr) return *existing;

    if (g_nodes.size() >= 128) {
        auto it = std::min_element(g_nodes.begin(), g_nodes.end(), [](const MeshNodeInfo &a, const MeshNodeInfo &b) {
            return a.lastHeard < b.lastHeard;
        });
        if (it != g_nodes.end()) {
            *it = MeshNodeInfo();
            it->num = nodeNum;
            return *it;
        }
    }

    g_nodes.push_back(MeshNodeInfo());
    g_nodes.back().num = nodeNum;
    return g_nodes.back();
}

static void meshAppendChat(const MeshChatMessage &msg) {
    if (msg.text.isEmpty()) return;
    if (g_chatMessages.size() >= 96) g_chatMessages.erase(g_chatMessages.begin());
    g_chatMessages.push_back(msg);
}

static void meshParseUser(const uint8_t *data, size_t len, MeshNodeInfo &node) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;
        if (field == 1 && wire == PB_WT_LEN) {
            const uint8_t *v = nullptr;
            size_t l = 0;
            if (!pbReadLenField(data, len, pos, v, l)) break;
            node.id = pbTextFromBytes(v, l);
        } else if (field == 2 && wire == PB_WT_LEN) {
            const uint8_t *v = nullptr;
            size_t l = 0;
            if (!pbReadLenField(data, len, pos, v, l)) break;
            node.longName = pbTextFromBytes(v, l);
        } else if (field == 3 && wire == PB_WT_LEN) {
            const uint8_t *v = nullptr;
            size_t l = 0;
            if (!pbReadLenField(data, len, pos, v, l)) break;
            node.shortName = pbTextFromBytes(v, l);
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }
}

static void meshParseNodeInfo(const uint8_t *data, size_t len) {
    uint32_t nodeNum = 0;
    MeshNodeInfo parsed;
    size_t pos = 0;

    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;

        if (field == 1 && wire == PB_WT_VARINT) {
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            nodeNum = (uint32_t)v;
            parsed.num = nodeNum;
        } else if (field == 2 && wire == PB_WT_LEN) {
            const uint8_t *u = nullptr;
            size_t ul = 0;
            if (!pbReadLenField(data, len, pos, u, ul)) break;
            meshParseUser(u, ul, parsed);
        } else if (field == 5 && wire == PB_WT_FIXED32) {
            uint32_t v = 0;
            if (!pbReadFixed32(data, len, pos, v)) break;
            parsed.lastHeard = v;
        } else if (field == 9 && wire == PB_WT_VARINT) {
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            parsed.hopsAway = (int32_t)v;
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }

    if (nodeNum == 0) return;
    MeshNodeInfo &node = meshGetOrCreateNode(nodeNum);
    if (!parsed.id.isEmpty()) node.id = parsed.id;
    if (!parsed.longName.isEmpty()) node.longName = parsed.longName;
    if (!parsed.shortName.isEmpty()) node.shortName = parsed.shortName;
    if (parsed.lastHeard != 0) node.lastHeard = parsed.lastHeard;
    if (parsed.hopsAway >= 0) node.hopsAway = parsed.hopsAway;
}

static void meshParseMyInfo(const uint8_t *data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;
        if (field == 1 && wire == PB_WT_VARINT) {
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            g_myNodeNum = (uint32_t)v;
            return;
        }
        if (!pbSkipField(data, len, pos, wire)) break;
    }
}

static void meshParseChannelSettings(const uint8_t *data, size_t len, MeshChannelInfo &channel) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;

        if (field == 3 && wire == PB_WT_LEN) { // ChannelSettings.name
            const uint8_t *v = nullptr;
            size_t l = 0;
            if (!pbReadLenField(data, len, pos, v, l)) break;
            channel.name = pbTextFromBytes(v, l);
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }
}

static void meshParseChannel(const uint8_t *data, size_t len) {
    int32_t index = -1;
    MeshChannelInfo parsed;
    parsed.role = MESH_CHANNEL_ROLE_DISABLED;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;

        if (field == 1 && wire == PB_WT_VARINT) { // Channel.index
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            if (v > 255) return;
            index = (int32_t)v;
            parsed.index = (uint8_t)v;
        } else if (field == 2 && wire == PB_WT_LEN) { // Channel.settings
            const uint8_t *settings = nullptr;
            size_t settingsLen = 0;
            if (!pbReadLenField(data, len, pos, settings, settingsLen)) break;
            meshParseChannelSettings(settings, settingsLen, parsed);
        } else if (field == 3 && wire == PB_WT_VARINT) { // Channel.role
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            parsed.role = (uint8_t)v;
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }

    if (index < 0) return;

    MeshChannelInfo &channel = meshGetOrCreateChannel((uint8_t)index);
    channel.index = (uint8_t)index;
    channel.lastUpdate = (uint32_t)millis();
    channel.role = parsed.role;
    if (!parsed.name.isEmpty()) channel.name = parsed.name;
}

static void meshParseDataPayload(
    const uint8_t *data, size_t len, uint32_t from, uint32_t to, uint8_t channel, uint32_t packetId, uint32_t rxTime
) {
    uint32_t portNum = 0;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;

        if (field == 1 && wire == PB_WT_VARINT) {
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            portNum = (uint32_t)v;
        } else if (field == 2 && wire == PB_WT_LEN) {
            if (!pbReadLenField(data, len, pos, payload, payloadLen)) break;
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }

    if (portNum != MESH_PORT_TEXT || payload == nullptr || payloadLen == 0) return;

    MeshChatMessage chat;
    chat.from = from;
    chat.to = to;
    chat.channel = channel;
    chat.id = packetId;
    chat.rxTime = rxTime;
    chat.text = pbTextFromBytes(payload, payloadLen);
    MeshChannelInfo &chatChannel = meshGetOrCreateChannel(channel);
    if (chatChannel.role == MESH_CHANNEL_ROLE_DISABLED) {
        chatChannel.role = channel == MESH_PRIMARY_CHANNEL ? MESH_CHANNEL_ROLE_PRIMARY : MESH_CHANNEL_ROLE_SECONDARY;
    }
    chatChannel.lastUpdate = (uint32_t)millis();
    meshAppendChat(chat);
}

static void meshParseMeshPacket(const uint8_t *data, size_t len) {
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t channel = MESH_PRIMARY_CHANNEL;
    uint32_t packetId = 0;
    uint32_t rxTime = 0;
    const uint8_t *decoded = nullptr;
    size_t decodedLen = 0;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;

        if (field == 1 && wire == PB_WT_FIXED32) {
            if (!pbReadFixed32(data, len, pos, from)) break;
        } else if (field == 2 && wire == PB_WT_FIXED32) {
            if (!pbReadFixed32(data, len, pos, to)) break;
        } else if (field == 3 && wire == PB_WT_VARINT) {
            uint64_t v = 0;
            if (!pbReadVarint(data, len, pos, v)) break;
            channel = (uint32_t)v;
        } else if (field == 4 && wire == PB_WT_LEN) {
            if (!pbReadLenField(data, len, pos, decoded, decodedLen)) break;
        } else if (field == 6 && wire == PB_WT_FIXED32) {
            if (!pbReadFixed32(data, len, pos, packetId)) break;
        } else if (field == 7 && wire == PB_WT_FIXED32) {
            if (!pbReadFixed32(data, len, pos, rxTime)) break;
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }

    if (decoded != nullptr && decodedLen > 0) {
        const uint8_t parsedChannel = (uint8_t)std::min(channel, (uint32_t)255);
        meshParseDataPayload(decoded, decodedLen, from, to, parsedChannel, packetId, rxTime);
    }
}

static void meshParseFromRadioFrame(const uint8_t *data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!pbReadKey(data, len, pos, field, wire)) break;

        if (field == 2 && wire == PB_WT_LEN) {
            const uint8_t *pkt = nullptr;
            size_t pktLen = 0;
            if (!pbReadLenField(data, len, pos, pkt, pktLen)) break;
            meshParseMeshPacket(pkt, pktLen);
        } else if (field == 3 && wire == PB_WT_LEN) {
            const uint8_t *info = nullptr;
            size_t infoLen = 0;
            if (!pbReadLenField(data, len, pos, info, infoLen)) break;
            meshParseMyInfo(info, infoLen);
        } else if (field == 4 && wire == PB_WT_LEN) {
            const uint8_t *node = nullptr;
            size_t nodeLen = 0;
            if (!pbReadLenField(data, len, pos, node, nodeLen)) break;
            meshParseNodeInfo(node, nodeLen);
        } else if (field == 7 && wire == PB_WT_VARINT) {
            uint64_t cfgId = 0;
            if (!pbReadVarint(data, len, pos, cfgId)) break;
            g_configCompleted = (g_lastConfigRequestId != 0 && cfgId == g_lastConfigRequestId);
        } else if (field == 10 && wire == PB_WT_LEN) {
            const uint8_t *channel = nullptr;
            size_t channelLen = 0;
            if (!pbReadLenField(data, len, pos, channel, channelLen)) break;
            meshParseChannel(channel, channelLen);
        } else if (!pbSkipField(data, len, pos, wire)) {
            break;
        }
    }
}

static String meshNodeLabel(uint32_t nodeNum) {
    if (nodeNum == MESH_BROADCAST) return "Broadcast";
    MeshNodeInfo *node = meshFindNode(nodeNum);
    if (node != nullptr) {
        if (!node->shortName.isEmpty()) return node->shortName;
        if (!node->longName.isEmpty()) return node->longName;
    }
    return meshNodeHex(nodeNum);
}

static void meshReadFromRadioOnce() {
    if (!g_meshConnected || g_fromRadioChr == nullptr || !g_fromRadioChr->canRead()) return;

    std::string frame = g_fromRadioChr->readValue();
    if (frame.empty()) {
        g_pendingFromRadio = false;
        return;
    }

    g_rxFrames++;
    meshParseFromRadioFrame((const uint8_t *)frame.data(), frame.size());
}

static void meshPollIncoming(uint8_t maxFrames) {
    if (!g_meshConnected) return;
    for (uint8_t i = 0; i < maxFrames; i++) {
        const size_t beforeRx = g_rxFrames;
        meshReadFromRadioOnce();
        if (g_rxFrames == beforeRx) break;
    }
}

static bool meshSendToRadio(const std::vector<uint8_t> &frame) {
    if (!g_meshConnected || g_toRadioChr == nullptr) return false;
    if (!g_toRadioChr->canWrite() && !g_toRadioChr->canWriteNoResponse()) return false;
    return g_toRadioChr->writeValue(frame.data(), frame.size(), g_toRadioChr->canWrite());
}

static bool meshRequestConfig() {
    g_lastConfigRequestId++;
    if (g_lastConfigRequestId == 0) g_lastConfigRequestId = 1;
    g_configCompleted = false;

    std::vector<uint8_t> req;
    req.reserve(8);
    pbWriteVarintField(req, 3, g_lastConfigRequestId); // ToRadio.want_config_id
    return meshSendToRadio(req);
}

static bool meshSendText(uint32_t toNode, uint8_t channel, const String &text) {
    if (text.isEmpty()) return false;

    std::vector<uint8_t> decoded;
    decoded.reserve(16 + text.length());
    pbWriteVarintField(decoded, 1, MESH_PORT_TEXT); // Data.portnum
    pbWriteBytesField(decoded, 2, (const uint8_t *)text.c_str(), text.length()); // Data.payload

    std::vector<uint8_t> packet;
    packet.reserve(decoded.size() + 24);
    pbWriteFixed32Field(packet, 2, toNode); // MeshPacket.to
    if (channel != MESH_PRIMARY_CHANNEL) pbWriteVarintField(packet, 3, channel); // MeshPacket.channel
    pbWriteBytesField(packet, 4, decoded.data(), decoded.size()); // MeshPacket.decoded
    pbWriteFixed32Field(packet, 6, (uint32_t)millis()); // MeshPacket.id

    std::vector<uint8_t> toradio;
    toradio.reserve(packet.size() + 8);
    pbWriteBytesField(toradio, 1, packet.data(), packet.size()); // ToRadio.packet

    const bool ok = meshSendToRadio(toradio);
    if (ok) {
        MeshChatMessage own;
        own.from = g_myNodeNum;
        own.to = toNode;
        own.channel = channel;
        own.id = (uint32_t)millis();
        own.rxTime = (uint32_t)time(nullptr);
        own.text = text;
        MeshChannelInfo &chatChannel = meshGetOrCreateChannel(channel);
        if (chatChannel.role == MESH_CHANNEL_ROLE_DISABLED) {
            chatChannel.role = channel == MESH_PRIMARY_CHANNEL ? MESH_CHANNEL_ROLE_PRIMARY : MESH_CHANNEL_ROLE_SECONDARY;
        }
        chatChannel.lastUpdate = (uint32_t)millis();
        meshAppendChat(own);
    }
    return ok;
}

static void meshShowTextScreen(const char *title, const std::vector<String> &lines) {
    drawMainBorderWithTitle(title);
    for (const auto &line : lines) padprintln(line);
    padprintln("");
    padprintln("[Any key] Back");
    while (!check(AnyKeyPress)) vTaskDelay(10 / portTICK_PERIOD_MS);
}

static void meshShowNodeDetails(uint32_t nodeNum) {
    MeshNodeInfo *node = meshFindNode(nodeNum);
    if (node == nullptr) {
        displayInfo("Node info not available", false);
        return;
    }

    std::vector<String> lines;
    lines.push_back("Num: " + meshNodeHex(node->num));
    lines.push_back("Label: " + meshNodeLabel(node->num));
    lines.push_back("ID: " + (node->id.isEmpty() ? String("-") : node->id));
    lines.push_back("Long: " + (node->longName.isEmpty() ? String("-") : node->longName));
    lines.push_back("Short: " + (node->shortName.isEmpty() ? String("-") : node->shortName));
    lines.push_back("Last heard: " + meshFormatDateTime(node->lastHeard));
    lines.push_back("Seen: " + meshFormatAge(node->lastHeard));
    lines.push_back("Hops: " + String(node->hopsAway));
    meshShowTextScreen("Meshtastic Node", lines);
}

static void meshShowChatDetails(const MeshChatMessage &msg) {
    std::vector<String> lines;
    lines.push_back("From: " + meshNodeLabel(msg.from) + " " + meshNodeHex(msg.from));
    lines.push_back("To: " + meshNodeLabel(msg.to) + " " + meshNodeHex(msg.to));
    lines.push_back("Channel: " + meshChannelLabel(msg.channel));
    lines.push_back("Msg ID: " + String(msg.id));
    lines.push_back("Time: " + meshFormatDateTime(msg.rxTime));
    lines.push_back("");
    lines.push_back(msg.text);
    meshShowTextScreen("Meshtastic Chat", lines);
}

static void meshOpenNodesMenu() {
    meshPollIncoming(32);
    if (g_nodes.empty()) {
        displayInfo("Node list is empty", false);
        return;
    }

    std::vector<MeshNodeInfo> sorted = g_nodes;
    std::sort(sorted.begin(), sorted.end(), [](const MeshNodeInfo &a, const MeshNodeInfo &b) {
        return a.num < b.num;
    });

    std::vector<Option> nodeOptions;
    nodeOptions.reserve(sorted.size() + 1);
    for (const auto &node : sorted) {
        const uint32_t num = node.num;
        String label = meshFormatClock(node.lastHeard) + " " + meshNodeLabel(num);
        nodeOptions.push_back({label, [num]() { meshShowNodeDetails(num); }});
    }
    nodeOptions.push_back({"Back", []() {}});
    loopOptions(nodeOptions, MENU_TYPE_SUBMENU, "Meshtastic Nodes");
}

static size_t meshChatCountForChannel(uint8_t channelIndex) {
    size_t count = 0;
    for (const auto &msg : g_chatMessages) {
        if (msg.channel == channelIndex) count++;
    }
    return count;
}

static void meshShowChannelDetails(uint8_t channelIndex) {
    meshEnsurePrimaryChannel();
    MeshChannelInfo *channel = meshFindChannel(channelIndex);

    std::vector<String> lines;
    lines.push_back("Name: " + meshChannelShortLabel(channelIndex));
    lines.push_back("Index: CH" + String(channelIndex));
    String roleText = channel ? meshChannelRoleLabel(channel->role) : String("unknown");
    lines.push_back("Role: " + roleText);
    lines.push_back("Msgs: " + String(meshChatCountForChannel(channelIndex)));
    lines.push_back("Active: " + String(channelIndex == g_activeChannel ? "yes" : "no"));
    meshShowTextScreen("Meshtastic Channel", lines);
}

static void meshOpenChannelsMenu() {
    meshPollIncoming(32);
    meshEnsurePrimaryChannel();

    std::vector<MeshChannelInfo> sorted = g_channels;
    std::sort(sorted.begin(), sorted.end(), [](const MeshChannelInfo &a, const MeshChannelInfo &b) {
        return a.index < b.index;
    });

    std::vector<Option> channelOptions;
    channelOptions.reserve(sorted.size() + 2);
    for (const auto &channel : sorted) {
        const uint8_t channelIndex = channel.index;
        String label = (channelIndex == g_activeChannel ? "* " : "  ");
        label += meshChannelLabel(channelIndex);
        label += " (";
        label += meshChannelRoleLabel(channel.role);
        label += ")";
        channelOptions.push_back({label, [channelIndex]() {
                                      g_activeChannel = channelIndex;
                                      meshShowChannelDetails(channelIndex);
                                  }});
    }
    channelOptions.push_back({"Refresh channels", []() {
                                  meshRequestConfig();
                                  meshPollIncoming(64);
                              }});
    channelOptions.push_back({"Back", []() {}});
    loopOptions(channelOptions, MENU_TYPE_SUBMENU, "Meshtastic Channels");
}

static void meshOpenChatLog(int32_t channelFilter) {
    meshPollIncoming(32);
    if (g_chatMessages.empty()) {
        displayInfo("No chat messages yet", false);
        return;
    }

    std::vector<Option> chatOptions;
    const int start = (int)g_chatMessages.size() - 1;
    const int maxShown = 24;
    int shown = 0;
    for (int i = start; i >= 0 && shown < maxShown; --i) {
        const MeshChatMessage msg = g_chatMessages[i];
        if (channelFilter >= 0 && msg.channel != (uint8_t)channelFilter) continue;
        String txt = msg.text;
        txt.replace("\n", " ");
        if (txt.length() > 24) txt = txt.substring(0, 24) + "...";
        String label = meshFormatClock(msg.rxTime) + " " + meshChannelShortLabel(msg.channel) + " "
                       + meshNodeLabel(msg.from) + ": " + txt;
        chatOptions.push_back({label, [msg]() { meshShowChatDetails(msg); }});
        shown++;
    }
    if (chatOptions.empty()) {
        displayInfo("No chat messages on this channel", false);
        return;
    }
    chatOptions.push_back({"Back", []() {}});
    String title = channelFilter < 0 ? "Meshtastic Chat" : "Chat " + meshChannelShortLabel((uint8_t)channelFilter);
    loopOptions(chatOptions, MENU_TYPE_SUBMENU, title.c_str());
}

static void meshOpenChatMenu() {
    meshPollIncoming(16);
    if (g_chatMessages.empty()) {
        displayInfo("No chat messages yet", false);
        return;
    }

    meshEnsurePrimaryChannel();
    std::vector<MeshChannelInfo> sorted = g_channels;
    std::sort(sorted.begin(), sorted.end(), [](const MeshChannelInfo &a, const MeshChannelInfo &b) {
        return a.index < b.index;
    });

    std::vector<Option> chatMenu;
    chatMenu.reserve(sorted.size() + 3);
    chatMenu.push_back({String("Current: ") + meshChannelShortLabel(g_activeChannel),
                        []() { meshOpenChatLog(g_activeChannel); }});
    chatMenu.push_back({"All channels", []() { meshOpenChatLog(-1); }});
    for (const auto &channel : sorted) {
        const uint8_t channelIndex = channel.index;
        if (channelIndex == g_activeChannel) continue;
        const size_t count = meshChatCountForChannel(channelIndex);
        String label = meshChannelShortLabel(channelIndex) + " (" + String(count) + ")";
        chatMenu.push_back({label, [channelIndex]() { meshOpenChatLog(channelIndex); }});
    }
    chatMenu.push_back({"Back", []() {}});
    loopOptions(chatMenu, MENU_TYPE_SUBMENU, "Meshtastic Chat");
}

static bool meshPickNode(uint32_t &nodeNumOut) {
    meshPollIncoming(16);
    if (g_nodes.empty()) return false;

    bool chosen = false;
    std::vector<Option> pickOptions;
    for (const auto &node : g_nodes) {
        const uint32_t num = node.num;
        String label = meshNodeLabel(num) + " " + meshNodeHex(num);
        pickOptions.push_back({label, [&, num]() {
                                   nodeNumOut = num;
                                   chosen = true;
                               }});
    }
    pickOptions.push_back({"Back", []() {}});
    loopOptions(pickOptions, MENU_TYPE_SUBMENU, "Select Node");
    return chosen;
}

static void meshSendChannelUI() {
    String prompt = "Msg to " + meshChannelShortLabel(g_activeChannel) + ":";
    String text = keyboard("", 240, prompt);
    if (text == "\x1B") return;
    text.trim();
    if (text.isEmpty()) {
        displayError("Message is empty", false);
        return;
    }

    const bool ok = meshSendText(MESH_BROADCAST, g_activeChannel, text);
    if (ok) displaySuccess("Channel message sent", false);
    else displayError("Send failed", false);
}

static void meshSendDirectUI() {
    uint32_t node = 0;
    if (!meshPickNode(node)) {
        displayInfo("No node selected", false);
        return;
    }

    String prompt = "Msg to " + meshNodeLabel(node) + " [" + meshChannelShortLabel(g_activeChannel) + "]:";
    String text = keyboard("", 240, prompt);
    if (text == "\x1B") return;
    text.trim();
    if (text.isEmpty()) {
        displayError("Message is empty", false);
        return;
    }

    const bool ok = meshSendText(node, g_activeChannel, text);
    if (ok) displaySuccess("Message sent", false);
    else displayError("Send failed", false);
}

static void meshShowDeviceInfo() {
    meshPollIncoming(16);
    std::vector<String> lines;
    lines.push_back("Connected: " + String(g_meshConnected ? "yes" : "no"));
    if (g_client != nullptr && g_meshConnected) {
        lines.push_back("Peer: " + String(g_client->getPeerAddress().toString().c_str()));
        lines.push_back("RSSI: " + String(g_client->getRssi()) + " dBm");
    }
    lines.push_back("My node: " + (g_myNodeNum ? meshNodeHex(g_myNodeNum) : String("-")));
    lines.push_back("Active ch: " + meshChannelLabel(g_activeChannel));
    lines.push_back("Channels: " + String(g_channels.size()));
    lines.push_back("Nodes: " + String(g_nodes.size()));
    lines.push_back("Chat msgs: " + String(g_chatMessages.size()));
    lines.push_back("RX frames: " + String(g_rxFrames));
    lines.push_back("Config sync: " + String(g_configCompleted ? "done" : "pending"));
    meshShowTextScreen("Meshtastic Info", lines);
}

static void meshDisconnect() {
    if (g_fromNumChr != nullptr) g_fromNumChr->unsubscribe();
    if (g_client != nullptr && g_client->isConnected()) g_client->disconnect();
    g_toRadioChr = nullptr;
    g_fromRadioChr = nullptr;
    g_fromNumChr = nullptr;
    g_meshConnected = false;
    g_meshDisconnected = true;
    BLEConnected = false;
}

static void meshSessionLoop() {
    bool leave = false;
    while (!leave && g_meshConnected) {
        meshPollIncoming(16);
        meshEnsurePrimaryChannel();

        std::vector<Option> menu;
        menu.reserve(10);
        menu.push_back({"Device Info", []() { meshShowDeviceInfo(); }});
        menu.push_back({String("Channel: ") + meshChannelShortLabel(g_activeChannel), []() { meshOpenChannelsMenu(); }});
        menu.push_back({String("Nodes (") + String(g_nodes.size()) + ")", []() { meshOpenNodesMenu(); }});
        menu.push_back({String("Chat (") + String(g_chatMessages.size()) + ")", []() { meshOpenChatMenu(); }});
        menu.push_back({String("Send ") + meshChannelShortLabel(g_activeChannel), []() { meshSendChannelUI(); }});
        menu.push_back({"Send Direct", []() { meshSendDirectUI(); }});
        menu.push_back({"Refresh", []() {
                            meshRequestConfig();
                            meshPollIncoming(48);
                        }});
        menu.push_back({"Disconnect", [&]() {
                            meshDisconnect();
                            leave = true;
                        }});
        menu.push_back({"Back", [&]() { leave = true; }});

        loopOptions(menu, MENU_TYPE_SUBMENU, "Meshtastic BLE");
    }
}

static void meshOnFromNumNotify(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool) { g_pendingFromRadio = true; }

static void meshResetBleState() {
    g_meshInit = false;
    g_meshConnected = false;
    g_meshDisconnected = true;
    g_pendingFromRadio = false;
    g_authComplete = false;
    g_authOk = false;
    g_pairingRequested = false;
    g_waitPasskeyInput = false;
    g_waitPasskeyConfirm = false;
    g_confirmPin = 0;
    g_scan = nullptr;
    g_client = nullptr;
    g_toRadioChr = nullptr;
    g_fromRadioChr = nullptr;
    g_fromNumChr = nullptr;
    g_nodes.clear();
    g_chatMessages.clear();
    g_channels.clear();
    g_activeChannel = MESH_PRIMARY_CHANNEL;
    g_myNodeNum = 0;
    g_rxFrames = 0;
    BLEConnected = false;
}

static void meshReleaseWifiMemory() {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        vTaskDelay(120 / portTICK_PERIOD_MS);
    }
    esp_wifi_stop();
    esp_wifi_deinit();
}

static void meshForceBleRecovery() {
    if (g_scan != nullptr && NimBLEDevice::isInitialized() && g_scan->isScanning()) g_scan->stop();
    if (g_fromNumChr != nullptr) g_fromNumChr->unsubscribe();
    if (g_client != nullptr && g_client->isConnected()) {
        g_client->disconnect();
        vTaskDelay(80 / portTICK_PERIOD_MS);
    }

    NimBLEDevice::deinit(true);

#if !defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
        vTaskDelay(20 / portTICK_PERIOD_MS);
        status = esp_bt_controller_get_status();
    }
    if (status == ESP_BT_CONTROLLER_STATUS_INITED) esp_bt_controller_deinit();
#if defined(CONFIG_IDF_TARGET_ESP32)
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
#endif

    meshResetBleState();
}

static bool meshEnsureInit() {
    if (g_meshInit && g_scan != nullptr && NimBLEDevice::isInitialized()) return true;

    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
        if (!NimBLEDevice::isInitialized()) {
            meshReleaseWifiMemory();
            if (!NimBLEDevice::init("")) {
                if (attempt == 0) {
                    meshForceBleRecovery();
                    vTaskDelay(120 / portTICK_PERIOD_MS);
                    continue;
                }
                return false;
            }
        }

        g_scan = NimBLEDevice::getScan();
        if (g_scan != nullptr) {
            g_scan->setActiveScan(true);
            g_scan->setInterval(100);
            g_scan->setWindow(99);
            g_meshInit = true;
            return true;
        }

        if (attempt == 0) {
            meshForceBleRecovery();
            vTaskDelay(120 / portTICK_PERIOD_MS);
        }
    }

    return false;
}

static bool meshStartBlockingScan(uint32_t durationMs) {
#ifdef NIMBLE_V2_PLUS
    if (g_scan == nullptr) return false;
    if (!g_scan->start(durationMs, false, true)) return false;

    const uint32_t timeoutMs = durationMs + 1500;
    const uint32_t startedAt = millis();
    while (g_scan->isScanning()) {
        if (millis() - startedAt > timeoutMs) {
            g_scan->stop();
            break;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
#endif
    return true;
}

static String meshFriendlyName(const String &name, const NimBLEAddress &addr) {
    if (!name.isEmpty()) return name;
    return String(addr.toString().c_str());
}

static bool meshLooksLikeDevice(const String &name, bool hasService) {
    if (hasService) return true;
    String low = name;
    low.toLowerCase();
    return low.indexOf("meshtastic") >= 0 || low.indexOf("mesh") >= 0;
}

static bool meshHandlePasskeyEntry() {
    if (!g_waitPasskeyInput || g_client == nullptr) return true;

    while (true) {
        String input = num_keyboard("", 6, "PIN from Meshtastic:", true);
        if (input == "\x1B") return false;
        input.trim();
        if (input.length() != 6) {
            displayError("PIN must be 6 digits", false);
            continue;
        }
        bool valid = true;
        for (uint8_t i = 0; i < 6; ++i) {
            if (input[i] < '0' || input[i] > '9') {
                valid = false;
                break;
            }
        }
        if (!valid) {
            displayError("PIN must be numeric", false);
            continue;
        }

        NimBLEConnInfo conn = g_client->getConnInfo();
        if (!NimBLEDevice::injectPassKey(conn, (uint32_t)input.toInt())) return false;
        g_waitPasskeyInput = false;
        return true;
    }
}

static bool meshHandlePasskeyConfirm() {
    if (!g_waitPasskeyConfirm || g_client == nullptr) return true;

    bool accept = false;
    std::vector<Option> options = {
        {String("Confirm ") + String(g_confirmPin), [&]() { accept = true;  }},
        {"Reject",                                  [&]() { accept = false; }},
    };
    loopOptions(options, MENU_TYPE_SUBMENU, "Pairing Check");

    NimBLEConnInfo conn = g_client->getConnInfo();
    if (!NimBLEDevice::injectConfirmPasskey(conn, accept)) return false;
    g_waitPasskeyConfirm = false;
    return accept;
}

static std::vector<MeshScanDevice> meshScanDevices() {
    std::vector<MeshScanDevice> devices;

    drawMainBorderWithTitle("Meshtastic BLE");
    padprintln("Scanning BLE...");

    if (!meshEnsureInit()) {
        char msg[64];
        snprintf(
            msg,
            sizeof(msg),
            "BLE init failed, RAM:%lu",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
        );
        displayError(msg, false);
        return devices;
    }

#ifdef NIMBLE_V2_PLUS
    if (!meshStartBlockingScan(5000)) {
        meshForceBleRecovery();
        if (!meshEnsureInit() || !meshStartBlockingScan(5000)) {
            char msg[64];
            snprintf(
                msg,
                sizeof(msg),
                "BLE scan failed rc=30, RAM:%lu",
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
            );
            displayError(msg, false);
            return devices;
        }
    }
    NimBLEScanResults results = g_scan->getResults();
#else
    NimBLEScanResults results = g_scan->start(5, false);
#endif

    for (int i = 0; i < results.getCount(); i++) {
#ifdef NIMBLE_V2_PLUS
        const NimBLEAdvertisedDevice *adv = results.getDevice(i);
        if (adv == nullptr) continue;
        const bool hasService = adv->isAdvertisingService(kMeshServiceUUID);
        const String name = adv->getName().c_str();
        const NimBLEAddress address = adv->getAddress();
        const int rssi = adv->getRSSI();
#else
        NimBLEAdvertisedDevice adv = results.getDevice(i);
        const bool hasService = adv.isAdvertisingService(kMeshServiceUUID);
        const String name = adv.getName().c_str();
        const NimBLEAddress address = adv.getAddress();
        const int rssi = adv.getRSSI();
#endif

        if (!meshLooksLikeDevice(name, hasService)) continue;

        bool exists = false;
        for (const auto &d : devices) {
            if (d.address == address) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        MeshScanDevice d;
        d.address = address;
        d.name = meshFriendlyName(name, address);
        d.rssi = rssi;
        devices.push_back(d);
    }

    g_scan->clearResults();
    return devices;
}

static bool meshConnectToDevice(const MeshScanDevice &dev) {
    if (!meshEnsureInit()) return false;
    BLEConnected = false;
    meshDisconnect();
    g_nodes.clear();
    g_chatMessages.clear();
    g_channels.clear();
    g_activeChannel = MESH_PRIMARY_CHANNEL;
    meshEnsurePrimaryChannel();
    g_myNodeNum = 0;
    g_rxFrames = 0;
    g_pendingFromRadio = false;
    g_configCompleted = false;
    g_authComplete = false;
    g_authOk = false;
    g_pairingRequested = false;
    g_waitPasskeyInput = false;
    g_waitPasskeyConfirm = false;
    g_confirmPin = 0;

    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);

    if (g_client == nullptr) {
        g_client = NimBLEDevice::createClient();
        if (g_client == nullptr) return false;
        g_client->setClientCallbacks(&g_clientCallbacks, false);
        g_client->setConnectTimeout(5000);
    }

    drawMainBorderWithTitle("Meshtastic BLE");
    padprintln("Connecting:");
    padprintln(dev.name);
    padprintln(dev.address.toString().c_str());

    if (!g_client->connect(dev.address, true, false, true)) {
        g_meshConnected = false;
        g_meshDisconnected = true;
        BLEConnected = false;
        return false;
    }

    NimBLERemoteService *svc = g_client->getService(kMeshServiceUUID);
    if (svc == nullptr) {
        meshDisconnect();
        return false;
    }

    g_toRadioChr = svc->getCharacteristic(kToRadioUUID);
    g_fromRadioChr = svc->getCharacteristic(kFromRadioUUID);
    g_fromNumChr = svc->getCharacteristic(kFromNumUUID);
    if (g_toRadioChr == nullptr || g_fromRadioChr == nullptr) {
        meshDisconnect();
        return false;
    }

    if (g_fromNumChr != nullptr && (g_fromNumChr->canNotify() || g_fromNumChr->canIndicate())) {
        g_fromNumChr->subscribe(g_fromNumChr->canNotify(), meshOnFromNumNotify);
    }

    g_meshConnected = true;
    g_meshDisconnected = false;

    if (!g_client->secureConnection(true)) {
        meshDisconnect();
        displayError("Pairing start failed", false);
        return false;
    }

    const uint32_t authStart = millis();
    while (g_client->isConnected() && !g_authComplete && (millis() - authStart) < 60000UL) {
        if (g_waitPasskeyInput && !meshHandlePasskeyEntry()) {
            meshDisconnect();
            displayError("PIN entry canceled", false);
            return false;
        }
        if (g_waitPasskeyConfirm && !meshHandlePasskeyConfirm()) {
            meshDisconnect();
            displayError("Pairing rejected", false);
            return false;
        }
        if (!g_pairingRequested && !g_waitPasskeyInput && !g_waitPasskeyConfirm && (millis() - authStart) > 1500UL) {
            break;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    if (!g_client->isConnected()) {
        meshDisconnect();
        return false;
    }

    if (g_pairingRequested && (!g_authComplete || !g_authOk)) {
        meshDisconnect();
        displayError("Pairing failed", false);
        return false;
    }

    meshRequestConfig();
    for (uint8_t i = 0; i < 15; i++) {
        meshPollIncoming(16);
        if (g_configCompleted) break;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    if (!g_client->isConnected()) {
        meshDisconnect();
        return false;
    }

    BLEConnected = true;
    return true;
}

static void meshConnectMenu() {
    while (!check(EscPress)) {
        std::vector<MeshScanDevice> found = meshScanDevices();
        std::vector<Option> scanOptions;
        bool quit = false;

        for (const auto &dev : found) {
            const MeshScanDevice devCopy = dev;
            String label = dev.name + " (" + String(dev.rssi) + "dBm)";
            scanOptions.push_back({label, [devCopy]() {
                                       if (meshConnectToDevice(devCopy)) {
                                           displaySuccess("Link established", false);
                                           meshSessionLoop();
                                       } else {
                                           displayError("Connect/pair failed", false);
                                       }
                                   }});
        }

        scanOptions.push_back({"Scan again", []() {}});
        scanOptions.push_back({"Back", [&]() { quit = true; }});

        loopOptions(scanOptions, MENU_TYPE_SUBMENU, "Meshtastic Scan");
        if (quit) break;
    }
}

} // namespace

void ble_meshtastic_menu() {
    meshResetBleState();
    BLEConnected = false;
    meshConnectMenu();
    meshForceBleRecovery();
}
