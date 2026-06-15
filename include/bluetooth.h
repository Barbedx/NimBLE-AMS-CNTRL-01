#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace Bluetooth {

// A device seen during the discovery scan, exposed to the setup portal so the
// user can pick which one to pair with (iOS advertises with a rotating random
// address and usually no name, so the user identifies their phone by RSSI /
// proximity).
struct ScanCandidate {
    std::string address; // "aa:bb:cc:dd:ee:ff"
    std::string name;    // may be empty (iOS rarely advertises a name)
    int         rssi;    // signal strength; stronger (closer to 0) == nearer
    bool        isApple; // manufacturer company id == 0x004C
    bool        isBonded; // already bonded to this identity
};

void Begin(const std::string& device_name);
void End();
void Service();
bool IsConnected();
bool IsTimeSet();
void ClearBonds();

// True once at least one bond is stored in NVS. When bonded the device
// reconnects to the phone automatically with no further user interaction.
bool HasBond();

// Address of the currently connected peer, or "" when not connected.
std::string ConnectedAddress();

// --- Setup portal support ---
// Thread-safe snapshot of devices seen during the discovery scan.
std::vector<ScanCandidate> GetCandidates();
// Ask the connection state machine to connect+pair with the given address.
// Returns false if the address is not in the current candidate list.
bool RequestConnect(const std::string& address);

} // namespace Bluetooth
