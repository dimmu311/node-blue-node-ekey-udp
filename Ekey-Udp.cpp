#include "Ekey-Udp.h"

#include <homegear-base/HelperFunctions/Net.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <netdb.h>
#include <homegear-node/JsonDecoder.h>
#include <dialog.h>

Flows::INode *MyFactory::createNode(const std::string &path, const std::string &type, const std::atomic_bool *frontendConnected) {
    return new Ekey::Ekey(path, type, frontendConnected);
}

Flows::NodeFactory *getFactory() {
    return (Flows::NodeFactory *)(new MyFactory);
}

namespace Ekey {

Ekey::Ekey(const std::string &path, const std::string &type, const std::atomic_bool *frontendConnected) : Flows::INode(path, type, frontendConnected) {
}

Ekey::~Ekey() = default;

bool Ekey::init(const Flows::PNodeInfo &info) {
    try {
        _nodeInfo = info;
        return true;
    }
    catch (const std::exception &ex) {
        _out->printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    return false;
}

bool Ekey::start() {
    std::string listenAddress;
    uint16_t port = 0;

    auto settingsIterator = _nodeInfo->info->structValue->find("listenaddress");
    if (settingsIterator != _nodeInfo->info->structValue->end()) listenAddress = settingsIterator->second->stringValue;

    if (!listenAddress.empty() && !BaseLib::Net::isIp(listenAddress)) listenAddress = BaseLib::Net::getMyIpAddress(listenAddress);
    else if (listenAddress.empty()) listenAddress = BaseLib::Net::getMyIpAddress();

    settingsIterator = _nodeInfo->info->structValue->find("listenport");
    if (settingsIterator != _nodeInfo->info->structValue->end()) port = (uint16_t)Flows::Math::getUnsignedNumber(settingsIterator->second->stringValue);

    settingsIterator = _nodeInfo->info->structValue->find("protocol");
    if (settingsIterator != _nodeInfo->info->structValue->end()) _protocol = settingsIterator->second->stringValue;

    _stopListenThread = true;
    if (_listenThread.joinable()) _listenThread.join();
    _stopListenThread = false;
    _listenThread = std::thread(&Ekey::listen, this, listenAddress, port, PayloadType::hex);

    return true;
}

void Ekey::stop() {
    _stopListenThread = true;
}

void Ekey::waitForStop() {
    if (_listenThread.joinable()) _listenThread.join();
}

int Ekey::getSocketDescriptor(const std::string &listenAddress, uint16_t port) {
    struct addrinfo *serverInfo = nullptr;
    int socketDescriptor = -1;
    try {
        struct addrinfo hostInfo{};
        hostInfo.ai_family = AF_UNSPEC;
        hostInfo.ai_socktype = SOCK_DGRAM;
        std::string portString = std::to_string(port);
        if (getaddrinfo(listenAddress.c_str(), portString.c_str(), &hostInfo, &serverInfo) != 0) {
            freeaddrinfo(serverInfo);
            serverInfo = nullptr;
            _out->printError("Error: Could not get address information. Is the specified IP address correct?");
            return -1;
        }

        socketDescriptor = socket(serverInfo->ai_family, SOCK_DGRAM, 0);
        if (socketDescriptor == -1) {
            _out->printError("Error: Could not create socket.");
            freeaddrinfo(serverInfo);
            return -1;
        }

        if (!(fcntl(socketDescriptor, F_GETFL) & O_NONBLOCK)) {
            if (fcntl(socketDescriptor, F_SETFL, fcntl(socketDescriptor, F_GETFL) | O_NONBLOCK) < 0) {
                freeaddrinfo(serverInfo);
                serverInfo = nullptr;
                close(socketDescriptor);
                _out->printError("Error: Could not set non blocking mode.");
                return -1;
            }
        }

        if (serverInfo->ai_family == AF_INET) {
            struct sockaddr_in localSock{};
            localSock.sin_family = serverInfo->ai_family;
            localSock.sin_port = htons(port);
            localSock.sin_addr.s_addr = ((struct sockaddr_in *)serverInfo->ai_addr)->sin_addr.s_addr;

            if (bind(socketDescriptor, (struct sockaddr *)&localSock, sizeof(localSock)) == -1) {
                _out->printError("Error: Binding to address " + listenAddress + " failed: " + std::string(strerror(errno)));
                close(socketDescriptor);
                freeaddrinfo(serverInfo);
                return -1;
            }

            uint32_t localSockSize = sizeof(localSock);
            if (getsockname(socketDescriptor, (struct sockaddr *)&localSock, &localSockSize) == -1) {
                freeaddrinfo(serverInfo);
                serverInfo = nullptr;
                close(socketDescriptor);
                _out->printError("Error: Could not get listen IP and/or port.");
                return -1;
            }

            auto *s = (struct sockaddr_in *)&(localSock.sin_addr);
            char ipStringBuffer[INET6_ADDRSTRLEN + 1];
            inet_ntop(AF_INET, s, ipStringBuffer, sizeof(ipStringBuffer));
            ipStringBuffer[INET6_ADDRSTRLEN] = '\0';
            _out->printInfo("Info: Now listening on IP " + std::string(ipStringBuffer) + " and port " + portString + ".");
        } else //AF_INET6
        {
            struct sockaddr_in6 localSock{};
            localSock.sin6_family = serverInfo->ai_family;
            localSock.sin6_port = htons(port);
            localSock.sin6_addr = ((struct sockaddr_in6 *)serverInfo->ai_addr)->sin6_addr;

            if (bind(socketDescriptor, (struct sockaddr *)&localSock, sizeof(localSock)) == -1) {
                _out->printError("Error: Binding to address " + listenAddress + " failed: " + std::string(strerror(errno)));
                close(socketDescriptor);
                freeaddrinfo(serverInfo);
                return -1;
            }

            uint32_t localSockSize = sizeof(localSock);
            if (getsockname(socketDescriptor, (struct sockaddr *)&localSock, &localSockSize) == -1) {
                freeaddrinfo(serverInfo);
                serverInfo = nullptr;
                close(socketDescriptor);
                _out->printError("Error: Could not get listen IP and/or port.");
                return -1;
            }

            auto *s = (struct sockaddr_in6 *)&(localSock.sin6_addr);
            char ipStringBuffer[INET6_ADDRSTRLEN + 1];
            inet_ntop(AF_INET6, s, ipStringBuffer, sizeof(ipStringBuffer));
            ipStringBuffer[INET6_ADDRSTRLEN] = '\0';
            _out->printInfo("Info: Now listening on IP " + std::string(ipStringBuffer) + " and port " + portString + ".");
        }

        freeaddrinfo(serverInfo);
        return socketDescriptor;
    }
    catch (const std::exception &ex) {
        _out->printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    close(socketDescriptor);
    freeaddrinfo(serverInfo);
    return -1;
}

void Ekey::listen(const std::string &listenAddress, uint16_t port, PayloadType payloadType) {
    try {
        int socketDescriptor = -1;
        std::array<uint8_t, 4096> buffer{};
        while (!_stopListenThread) {
            if (socketDescriptor == -1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                socketDescriptor = getSocketDescriptor(listenAddress, port);
                continue;
            }

            timeval timeout{};
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            int32_t nfds = socketDescriptor + 1;
            fd_set readFileDescriptor;
            FD_ZERO(&readFileDescriptor);
            FD_SET(socketDescriptor, &readFileDescriptor);

            auto result = select(nfds, &readFileDescriptor, nullptr, nullptr, &timeout);
            if (result == 0) continue;
            else if (result != 1) {
                close(socketDescriptor);
                socketDescriptor = -1;
                continue;
            }

            struct sockaddr clientInfo{};
            uint32_t addressLength = sizeof(sockaddr);
            ssize_t bytesRead = 0;
            do {
                bytesRead = recvfrom(socketDescriptor, buffer.data(), buffer.size(), 0, &clientInfo, &addressLength);
            } while (bytesRead < 0 && (errno == EAGAIN || errno == EINTR));

            if (bytesRead <= 0) {
                close(socketDescriptor);
                socketDescriptor = -1;
                continue;
            }

            std::array<char, INET6_ADDRSTRLEN + 1> ipStringBuffer{};
            if (clientInfo.sa_family == AF_INET) {
                auto *s = (struct sockaddr_in *)&clientInfo;
                inet_ntop(AF_INET, &s->sin_addr, ipStringBuffer.data(), ipStringBuffer.size());
            } else { // AF_INET6
                auto *s = (struct sockaddr_in6 *)&clientInfo;
                inet_ntop(AF_INET6, &s->sin6_addr, ipStringBuffer.data(), ipStringBuffer.size());
            }
            ipStringBuffer.back() = 0;
            auto senderIp = std::string(ipStringBuffer.data());

            Flows::PVariable message = std::make_shared<Flows::Variable>(Flows::VariableType::tStruct);
            message->structValue->emplace("senderIp", std::make_shared<Flows::Variable>(senderIp));

            std::string packet(buffer.begin(),buffer.begin() + bytesRead);
            Flows::PVariable var = std::make_shared<Flows::Variable>(Flows::VariableType::tStruct);
            if(_protocol == "rare") processRarePacket(packet, var);
            else if (_protocol == "home") processHomePacket(packet, var);
            else if (_protocol == "multi") processMultiPacket(packet, var);

            message->structValue->emplace("payload", var);
            output(0, message);
        }

        close(socketDescriptor);
    }
    catch (const std::exception &ex) {
        _out->printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

void Ekey::processRarePacket(const std::string &data, Flows::PVariable &var) {

/*RARE 72 Byte
    1   nVersion            long            3
    2   nCmd                long            0x88 = Dezimal 136.. mit Finger Türe öffnen
                                            0x89 = Dezimal 137.. schlechter oder unbekannter Finger
    3   nTerminalID         long            Adresse des Fingerscanners. Siehe Berechnung untenstehend
    4   strTerminalSerial   char[14]        0
    5   nRelayID            char[1]         0.. Channel 1 (Relay1)
                                            1.. Channel 2 (Relay2)
                                            2.. Channel 3 (Relay3)
                                            15.. Doppelrelais
    6   nReserved           char[1]         Leer
    7   nUserID             long            Usernummer lt. Speicherplatz auf ekey home Steuereinheit
                                            1.. User 1
                                            2.. User 2
                                            99 .. User 99
                                            0… Unbekannter User
    8   nFinger             long            Fingernummer lt. ekey home Steuereinheit
                                            0 .. Finger 1
                                            1 .. Finger 2
                                            9 .. Finger 0
                                            13 .. RFID
    9   strEvent            char[16]        0
    10  sTime               char[16]        0
    11  strName             unsignedshort   0
    12  strPersonalID       unsignedshort   0

    Calculate Terminal Address
    aaaaaa ww yy ssss
    Adresse = (((yy * 53 + ww) * 655367)) + ssss) + 0x70000000
 */
    try{
        _out->printMessage("Process Rare Packet -> 0x" + Flows::HelperFunctions::getHexString(data),4);
        //if(data.length() != 72) {
        //    _out->printError("dropping packet because of length mismatch. packet was " + std::to_string(data.length()) + " bytes long and is 0x" + Flows::HelperFunctions::getHexString(data));
        //    return;
        //}
        var->structValue->emplace("version", std::make_shared<Flows::Variable>(std::stoi(data.substr(0, 8), nullptr, 16)));

    }
    catch (const std::exception &ex) {
        _out->printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

void Ekey::processHomePacket(const std::string &data, Flows::PVariable &var) {
/*Home 22 Byte
    PAKETTYP    1 String                1           Pakettyp „Nutzdaten“
    USER ID     4 String (dezimal)      0000-9999   Benutzernummer (Default 0000)
    FINGER ID   1 String (dezimal)      0-9         1.. linker kleiner Finger
                                                    2.. linker Ringfinger
                                                    3.. linker Mittelfinger
                                                    4.. linker Zeigefinger
                                                    5.. linker Daumen
                                                    6.. rechter Daumen
                                                    7.. rechter Zeigefinger
                                                    8.. rechter Mittelfinger
                                                    9.. rechter Ringfinger
                                                    0.. rechter kleiner Finger
                                                    R.. RFID
                                                    „-„.. kein Finger
    SERIENNR FS 14 String               xxxxxx xx xx xxxx
                                                    Stelle 1-6 = Artikelnummer
                                                    Stelle 7-8 = Produktionswoche
                                                    Stelle 9-10 = Produktionsjahr
                                                    Stelle 11-14 = fortlaufende Nummer
    AKTION      1 String                1,2         1.. Öffnen
                                                    2.. Ablehnung unbekannter Finger
    RELAIS      1 String                1-4; „-„    1.. Relais 1
                                                    2.. Relais 2
                                                    3.. Relais 3
                                                    4.. Relais 4
                                                    d.. Doppelrelais
                                                    „-„.. kein Relais

    ok,  1_0046_4_80156809150025_1_2
    nok, 1_0000_–_80156809150025_2_-
 */
    try {
        _out->printMessage("Process Home Packet -> 0x" + Flows::HelperFunctions::getHexString(data),4);
        if(data.length() != 27){
            _out->printError("dropping packet because of length mismatch. packet was " + std::to_string(data.length()) + " bytes long and is 0x" + Flows::HelperFunctions::getHexString(data));
            return;
        }

        var->structValue->emplace("packetType", std::make_shared<Flows::Variable>(std::stoi(data.substr(0,1))));
        var->structValue->emplace("userId", std::make_shared<Flows::Variable>(std::stoi(data.substr(1,4))));

        auto fingerId = data.substr(5,1);
        std::string finger;
        int32_t iFinger;
        getFinger(fingerId, finger, iFinger);
        var->structValue->emplace("fingerId", std::make_shared<Flows::Variable>(iFinger));
        var->structValue->emplace("finger", std::make_shared<Flows::Variable>(finger));
        var->structValue->emplace("serialNr", std::make_shared<Flows::Variable>(data.substr(6, 14)));

        int32_t iAction = std::stoi(data.substr(20, 1));
        std::string action;
        if(iAction == 1) action = "open";
        else if (iAction == 2) action = "refuse";

        var->structValue->emplace("action", std::make_shared<Flows::Variable>(action));

        auto relaysId = data.substr(21,1);
        int32_t iRelays;
        std::string relays;
        if(relaysId == "d"){
            relays = "Multiple Relays";
            iRelays = -1;
        } else if (relaysId == "-"){
            relays = "none";
            iRelays = -2;
        } else {
            iRelays = std::stoi(relaysId);
            relays = "Relays" + std::to_string(iRelays);
        }

        var->structValue->emplace("relaysId", std::make_shared<Flows::Variable>(iRelays));
        var->structValue->emplace("relay", std::make_shared<Flows::Variable>(relays));
    }
    catch (const std::exception &ex) {
        _out->printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

void Ekey::processMultiPacket(const std::string &data, Flows::PVariable &var) {
/*Multi 37 Byte
    PAKETTYP    1 String                1           Pakettyp „Nutzdaten“
    USER ID     4 String (dezimal)      0000-9999   Benutzernummer (Default 0000)
    USER NAME   9 String                xxxxxxxxx   Benutzername
                                                    xx.. alphanumerisch
                                                    -.. undefiniert
    USER STATUS 1 String                0, 1, „-„   0.. Benutzer deaktiviert
                                                    1.. Benutzer aktiviert
                                                    „-„.. undefiniert
    FINGER ID   1 String (dezimal)      0-9         1.. linker kleiner Finger
                                                    2.. linker Ringfinger
                                                    3.. linker Mittelfinger
                                                    4.. linker Zeigefinger
                                                    5.. linker Daumen
                                                    6.. rechter Daumen
                                                    7.. rechter Zeigefinger
                                                    8.. rechter Mittelfinger
                                                    9.. rechter Ringfinger
                                                    0.. rechter kleiner Finger
                                                    „-„.. kein Finger
    SCHLÜSSEL ID 1 String               1-4, „-„    1.. Schlüssel 1
                                                    2.. Schlüssel 2
                                                    3.. Schlüssel 3
                                                    4.. Schlüssel 4
                                                    „-„.. Generalschlüssel
    SERIENNR FS 14 String               xxxxxx xx xx xxxx
                                                    Stelle 1-6 = Artikelnummer
                                                    Stelle 7-8 = Produktionswoche
                                                    Stelle 9-10 = Produktionsjahr
                                                    Stelle 11-14 = fortlaufende Nummer
    NAME FS     4 String                xxxx        Fingerscannerbezeichnung
    AKTION      1 String                1-8         1.. Öffnen
                                                    2.. Ablehnung unbekannter Finger
                                                    3.. Ablehnung Zeitfenster A
                                                    4.. Ablehnung Zeitfenster B
                                                    5.. Ablehnung inaktiv
                                                    6.. Ablehnung „Nur immer Nutzer“
                                                    7.. Fingerscanner nicht mit Steuereinheit gekoppelt
                                                    8.. digitaler Input
                                                    A.. 1-minütige Sperre der Codetastatur
                                                    B.. 15-minütige Sperre der Codetastatur
    INPUT ID    1 String                1-4, „-„    1.. digitaler Eingang 1
                                                    2.. digitaler Eingang 2
                                                    3.. digitaler Eingang 3
                                                    4.. digitaler Eingang 4
                                                    „-„.. kein digitaler Eingang

    ok 1_0003_JOSEF----_1_7_2_80156809150025_GAR-_1_-
    nok 1_0003_JOSEF----_1_7_2_80156809150025_GAR-_3_-
 */
    try{
        _out->printInfo("***************************************");
        _out->printMessage("Process Multi Packet -> 0x" + Flows::HelperFunctions::getHexString(data),4);
        if(data.length() != 37){
            _out->printError("dropping packet because of length mismatch. packet was " + std::to_string(data.length()) + " bytes long and is 0x" + Flows::HelperFunctions::getHexString(data));
            return;
        }

        var->structValue->emplace("packetType", std::make_shared<Flows::Variable>(std::stoi(data.substr(0,1))));
        var->structValue->emplace("userId", std::make_shared<Flows::Variable>(std::stoi(data.substr(1,4))));
        std::string userName = data.substr(5,9);
        userName.erase(std::remove(userName.begin(), userName.end(), '-'), userName.end());
        var->structValue->emplace("username", std::make_shared<Flows::Variable>(userName));

        auto userStateId = data.substr(14, 1);
        std::string userState;
        if(userStateId == "-") userState = "undefined";
        else {
            int32_t iUserState = std::stoi(userStateId);
            if (iUserState == 0) userState = "user inactive";
            else if (iUserState == 1) userState = "user active";
        }
        var->structValue->emplace("userState", std::make_shared<Flows::Variable>(userState));

        auto fingerId = data.substr(15,1);
        std::string finger;
        int32_t iFinger;
        getFinger(fingerId, finger, iFinger);
        var->structValue->emplace("fingerId", std::make_shared<Flows::Variable>(iFinger));
        var->structValue->emplace("finger", std::make_shared<Flows::Variable>(finger));

        auto keyId = data.substr(16, 1);
        std::string key;
        if(keyId == "-") key = "Mainkey";
        else {
            int32_t iKeyId = std::stoi(keyId);
            key = "Key " + std::to_string(iKeyId);
        }
        var->structValue->emplace("key", std::make_shared<Flows::Variable>(key));

        var->structValue->emplace("serialNr", std::make_shared<Flows::Variable>(data.substr(17, 14)));

        std::string readerName = data.substr(31,4);
        readerName.erase(std::remove(readerName.begin(), readerName.end(), '-'), readerName.end());
        var->structValue->emplace("readerName", std::make_shared<Flows::Variable>(readerName));

        int32_t iAction = std::stoi(data.substr(35, 1), nullptr, 16);
        std::string action;
        if(iAction == 1) action = "open";
        else if (iAction == 2) action = "refuse unknown finger";
        else if (iAction == 3) action = "refuse time slot A";
        else if (iAction == 4) action = "refuse time slot B";
        else if (iAction == 5) action = "refuse disabled";
        else if (iAction == 6) action = "refuse \"Only always users\"";
        else if (iAction == 7) action = "scanner not connected to control panel";
        else if (iAction == 8) action = "digital input";
        else if (iAction == 0xA) action = "codepad 1 min. lock";
        else if (iAction == 0xb) action = "codepad 15 min. lock";
        var->structValue->emplace("action", std::make_shared<Flows::Variable>(action));

        auto inputId = data.substr(36, 1);
        std::string input;
        if(inputId == "-") input = "no digital input";
        else {
            int32_t iInputId = std::stoi(inputId);
            input = "Input " + std::to_string(iInputId);
        }
        var->structValue->emplace("input", std::make_shared<Flows::Variable>(input));

    }
    catch (const std::exception &ex) {
        _out->printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

    void Ekey::getFinger(const std::string &fingerId, std::string &finger, int32_t &iFinger) {
        if(fingerId == "R"){
            finger = "RFID";
            iFinger = -1;
        } else if (fingerId == "-"){
            finger = "Unknown Finger";
            iFinger = -2;
        } else {
            iFinger = std::stoi(fingerId);
            if(iFinger < 6 && iFinger > 0){
                finger = "Left Hand";
            } else {
                finger = "Right Hand";
            }

            if(iFinger == 1 || iFinger == 0){
                finger.append(" Pinky");
            } else if (iFinger == 2 || iFinger == 9) {
                finger.append(" Ring Finger");
            } else if (iFinger == 3 || iFinger == 8) {
                finger.append(" Middle Finger");
            } else if (iFinger == 4 || iFinger == 7) {
                finger.append(" Index Finger");
            } else if (iFinger == 5 || iFinger == 6) {
                finger.append(" Thumb");
            }
        }
    }
}