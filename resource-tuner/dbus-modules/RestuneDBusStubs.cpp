#include "RestuneDBus.h"

std::shared_ptr<RestuneSDBus> RestuneSDBus::restuneSDBusInstance = nullptr;

RestuneSDBus::RestuneSDBus() {}

// rename extensions folder to dbus-modules
ErrCode startService(const std::string& unitName) {
    LOGE("URM_DBUS_DEP_MODULES", "dBus support not found, skipping.");
    return RC_SUCCESS;
}

ErrCode stopService(const std::string& unitName) {
    LOGE("URM_DBUS_DEP_MODULES", "dBus support not found, skipping.");
    return RC_SUCCESS;
}

ErrCode restartService(const std::string& unitName) {
    LOGE("URM_DBUS_DEP_MODULES", "dBus support not found, skipping.");
    return RC_SUCCESS;
}

void* RestuneSDBus::getBus() {}

RestuneSDBus::~RestuneSDBus() {}
