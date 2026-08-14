#include "gdx_updater_platform.h"

#include <switch.h>

namespace gdx::updater::platform {

namespace {

// True only while the updater own the socket driver.
bool sOwnsSocketDriver = false;
bool sNetworkUp = false;

SocketInitConfig SmallInitConfig() {
    SocketInitConfig config = {};
    config.tcp_tx_buf_size = 0x4000;
    config.tcp_rx_buf_size = 0x8000;
    config.tcp_tx_buf_max_size = 0x10000;
    config.tcp_rx_buf_max_size = 0x20000;
    config.udp_tx_buf_size = 0x2400;
    config.udp_rx_buf_size = 0xA500;
    config.sb_efficiency = 2;
    config.num_bsd_sessions = 3;
    config.bsd_service_type = BsdServiceType_User;
    return config;
}

} // namespace

bool NetworkInit() {
    if (sNetworkUp) {
        return true;
    }
    const SocketInitConfig config = SmallInitConfig();
    const Result res = socketInitialize(&config);
    if (R_SUCCEEDED(res)) {
        sOwnsSocketDriver = true;
        sNetworkUp = true;
        return true;
    }
    if (R_VALUE(res) == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
        sNetworkUp = true;
        return true;
    }
    return false;
}

void NetworkExit() {
    if (sOwnsSocketDriver) {
        socketExit();
        sOwnsSocketDriver = false;
    }
    sNetworkUp = false;
}

bool QueueNextLoad(const std::string& nroPath) {
    if (!envHasNextLoad()) {
        return false;
    }
    return R_SUCCEEDED(envSetNextLoad(nroPath.c_str(), nroPath.c_str()));
}

} // namespace gdx::updater::platform
