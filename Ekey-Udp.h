#ifndef EKEY_H_
#define EKEY_H_

#include <homegear-node/NodeFactory.h>
#include <homegear-node/INode.h>
#include <thread>

class MyFactory : Flows::NodeFactory {
public:
    Flows::INode *createNode(const std::string &path, const std::string &type, const std::atomic_bool *frontendConnected) override;
};

extern "C" Flows::NodeFactory *getFactory();

namespace Ekey {

class Ekey : public Flows::INode {
public:
    Ekey(const std::string &path, const std::string &type, const std::atomic_bool *frontendConnected);
    ~Ekey() override;

    bool init(const Flows::PNodeInfo &info) override;
    bool start() override;
    void stop() override;
    void waitForStop() override;
private:
    enum class PayloadType {
        raw,
        hex,
        json
    };

    Flows::PNodeInfo _nodeInfo;
    std::atomic_bool _stopListenThread{false};
    std::thread _listenThread;
    std::string _protocol;

    int getSocketDescriptor(const std::string &listenAddress, uint16_t port);
    void listen(const std::string &listenAddress, uint16_t port, PayloadType payloadType);

    void processRarePacket(const std::string &data, Flows::PVariable &var);
    void processHomePacket(const std::string &data, Flows::PVariable &var);
    void processMultiPacket(const std::string &data, Flows::PVariable &var);

    void getFinger(const std::string &fingerId, std::string &finger, int32_t &iFinger);
};

}
#endif