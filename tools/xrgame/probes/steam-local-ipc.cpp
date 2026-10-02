// Private validation probe: run beside a client with disable_networking=1.
// No account credentials or game files are needed.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "steam/steam_api.h"

static int failures;
static void check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!value) ++failures;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    SetEnvironmentVariableA("SteamAppId", "480");
    HMODULE library = LoadLibraryA(argv[1]);
    auto factory = library ? reinterpret_cast<void* (*)(const char*, int*)>(GetProcAddress(library, "CreateInterface")) : nullptr;
    if (!factory) { std::printf("client load failed %lu\n", GetLastError()); return 2; }
    auto client = static_cast<ISteamClient020*>(factory("SteamClient020", nullptr));
    if (!client) return 2;
    HSteamPipe pipe = client->CreateSteamPipe();
    HSteamUser user = client->ConnectToGlobalUser(pipe);
    auto sockets = static_cast<ISteamNetworkingSockets012*>(client->GetISteamGenericInterface(user, pipe, "SteamNetworkingSockets012"));
    if (!sockets) return 2;
    HSteamNetConnection a = 0, b = 0;
    check(sockets->CreateSocketPair(&a, &b, false, nullptr, nullptr), "socket pair");
    auto group = sockets->CreatePollGroup();
    check(sockets->SetConnectionPollGroup(b, group), "poll group");
    std::vector<char> payload(65536, 'Q');
    int64 previous = 0;
    for (int bytes : {0, 23, 65536}) {
        int64 number = 0;
        EResult sent = sockets->SendMessageToConnection(a, payload.data(), bytes, k_nSteamNetworkingSend_Reliable, &number);
        check(sent == k_EResultOK, "offline send");
        if (sent != k_EResultOK) break;
        check(number > previous, "message sequence"); previous = number;
        SteamNetworkingMessage_t* message = nullptr;
        ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < 3000 && !message) {
            sockets->ReceiveMessagesOnPollGroup(group, &message, 1);
            if (!message) Sleep(10);
        }
        check(message && message->m_conn == b && message->m_cbSize == bytes &&
            message->m_nMessageNumber == number && (!bytes || !memcmp(message->m_pData, payload.data(), bytes)), "poll group payload");
        if (message) message->Release();
    }
    const char reply[] = "server reply";
    check(sockets->SendMessageToConnection(b, reply, sizeof(reply), k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK, "reverse send");
    SteamNetworkingMessage_t* received = nullptr;
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 3000 && !received) {
        sockets->ReceiveMessagesOnConnection(a, &received, 1);
        if (!received) Sleep(10);
    }
    check(received && received->m_cbSize == sizeof(reply) && !memcmp(received->m_pData, reply, sizeof(reply)), "reverse payload");
    if (received) received->Release();
    check(sockets->CloseConnection(a, 0, "probe complete", false), "close endpoint");
    SteamNetConnectionInfo_t info{};
    start = GetTickCount64();
    do {
        sockets->GetConnectionInfo(b, &info);
        if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) break;
        Sleep(10);
    } while (GetTickCount64() - start < 3000);
    check(info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer, "peer close notification");
    check(sockets->SendMessageToConnection(b, reply, sizeof(reply), 0, nullptr) == k_EResultNoConnection, "closed send rejected");
    sockets->CloseConnection(b, 0, "probe complete", false);
    sockets->DestroyPollGroup(group);
    client->ReleaseUser(pipe, user);
    client->BReleaseSteamPipe(pipe);
    client->BShutdownIfAllPipesClosed();
    std::printf("END failures=%d\n", failures);
    return failures ? 1 : 0;
}
