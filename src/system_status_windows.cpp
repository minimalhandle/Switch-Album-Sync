#include "nso_album_sync/auto_sync_policy.hpp"

#include "nso_album_sync/windows_compat.hpp"
#include <netlistmgr.h>
#include <wrl/client.h>

#include <ctime>

namespace nso {

bool battery_saver_on() {
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.SystemStatusFlag == 1;
}

bool metered_connection() {
    // Network List Manager reports the cost Windows assigns to the current
    // connection. Fixed or variable cost means the user marked it metered (or
    // Windows did, e.g. for mobile data).
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool metered = false;
    {
        Microsoft::WRL::ComPtr<INetworkCostManager> costs;
        DWORD cost = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&costs))) &&
            SUCCEEDED(costs->GetCost(&cost, nullptr))) {
            metered = (cost & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE)) != 0;
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return metered;
}

int local_hour_now() {
    const auto now = std::time(nullptr);
    std::tm local{};
    return localtime_s(&local, &now) == 0 ? local.tm_hour : 12;
}

std::chrono::milliseconds user_idle_time() {
    LASTINPUTINFO input{sizeof(input)};
    if (!GetLastInputInfo(&input)) return std::chrono::milliseconds::zero();
    // Both are GetTickCount values; unsigned subtraction handles wraparound.
    return std::chrono::milliseconds(static_cast<DWORD>(GetTickCount() - input.dwTime));
}

}  // namespace nso
