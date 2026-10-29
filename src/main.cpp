// RadioBrowserUpnp - standalone UPnP Media Server exposing the
// radio-browser.info community directory, built on openhome/ohNet.
//
// Targets: Linux (Ubuntu/Debian amd64 & aarch64), Synology NAS
// (Container Manager or any UPnP-capable DSM app).

#include "RadioBrowserMediaServer.h"
#include "WebConfig.h"

#include <OpenHome/Net/Core/OhNet.h>
#include <OpenHome/Net/Private/DviStack.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/Ascii.h>

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace OpenHome;
using namespace OpenHome::Net;
using namespace OpenHome::Av;

static std::atomic<bool> gQuit(false);

static void SigHandler(int)
{
    gQuit = true;
}

// Deterministic udn from the host name so the device keeps a stable identity
// across restarts (control points remember pinned stations by udn).
static std::string MakeUdn()
{
    char host[64] = {0};
    if (::gethostname(host, sizeof(host) - 1) != 0) {
        std::strcpy(host, "radiobrowser");
    }
    // simple djb2 hash of the host name
    uint32_t h = 5381;
    for (char* p = host; *p; p++) {
        h = ((h << 5) + h) + static_cast<uint32_t>(static_cast<unsigned char>(*p));
    }
    std::ostringstream os;
    os << "uuid:" << std::hex << h << "-0000-4000-8000-" << std::hex << h << h;
    return os.str();
}

static std::string DefaultConfigPath()
{
    const char* env = std::getenv("RADIOBROWSERUPNP_CONFIG");
    if (env && *env) {
        return env;
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/radiobrowserupnp.json";
    }
    return "/etc/radiobrowserupnp.json";
}

static std::string AdapterAddrText(const TIpAddress& aAddr)
{
    // aAddr.iV4 is network order
    const uint32_t a = aAddr.iV4;
    std::ostringstream os;
    os << ((a >> 0) & 0xff) << "." << ((a >> 8) & 0xff) << "."
       << ((a >> 16) & 0xff) << "." << ((a >> 24) & 0xff);
    return os.str();
}

// Pick the subnet to announce on. On NAS hosts (docker0, ovs bridges, vpn
// tun adapters, ...) the first adapter ohNet would pick is often not the
// LAN interface, leaving the device invisible to control points.
// Priority: RADIOBROWSERUPNP_SUBNET env override, then the first adapter
// whose name contains eth/enp/ens/em (typical LAN naming), then the first
// non-loopback adapter.
static const NetworkAdapter* PickAdapter(const std::vector<NetworkAdapter*>& aList)
{
    Log::Print("RadioBrowserUpnp: %d network adapter(s):\n", (int)aList.size());
    for (const auto* a : aList) {
        Log::Print("  - %s: addr=%s subnet=%s\n",
                   a->Name(),
                   AdapterAddrText(a->Address()).c_str(),
                   AdapterAddrText(a->Subnet()).c_str());
    }
    const char* envSubnet = std::getenv("RADIOBROWSERUPNP_SUBNET");
    if (envSubnet && *envSubnet) {
        // parse dotted quad into network order (host bytes reversed)
        uint32_t parts[4] = {0, 0, 0, 0};
        const int parsed = std::sscanf(envSubnet, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]);
        if (parsed == 4) {
            const uint32_t want = htonl(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]);
            for (const auto* a : aList) {
                if (a->Address().iFamily == kFamilyV4 &&
                    want == a->Subnet().iV4) {
                    Log::Print("RadioBrowserUpnp: subnet override matched %s\n", a->Name());
                    return a;
                }
            }
            Log::Print("RadioBrowserUpnp: no adapter matches subnet %s\n", envSubnet);
        }
        else {
            Log::Print("RadioBrowserUpnp: bad RADIOBROWSERUPNP_SUBNET '%s'\n", envSubnet);
        }
    }
    for (const char* pat : { "eth", "enp", "ens", "em" }) {
        for (const auto* a : aList) {
            if (std::strstr(a->Name(), pat) != nullptr) {
                return a;
            }
        }
    }
    return aList.empty() ? nullptr : aList[0];
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, SigHandler);
    std::signal(SIGTERM, SigHandler);

    // Preferences
    Preferences prefs;
    const std::string configPath = DefaultConfigPath();
    prefs.Load(configPath);

    // ohNet device stack
    InitialisationParams* initParams = InitialisationParams::Create();
    initParams->SetDvUpnpServerPort(0); // pick a free port
    Library* lib = new Library(initParams);
    std::vector<NetworkAdapter*>* subnetList = lib->CreateSubnetList();
    if (subnetList->empty()) {
        Log::Print("RadioBrowserUpnp: no network adapter\n");
        Library::DestroySubnetList(subnetList);
        delete lib;
        return 1;
    }
    const NetworkAdapter* chosen = PickAdapter(*subnetList);
    if (chosen == nullptr) {
        Log::Print("RadioBrowserUpnp: no network adapter\n");
        Library::DestroySubnetList(subnetList);
        delete lib;
        return 1;
    }
    Log::Print("RadioBrowserUpnp: announcing on %s (addr %s)\n",
               chosen->Name(), AdapterAddrText(chosen->Address()).c_str());
    lib->SetCurrentSubnet(chosen->Subnet());
    Library::DestroySubnetList(subnetList);
    DvStack* dvStack = lib->StartDv();
    dvStack->Start();

    const std::string udn = MakeUdn();
    const Bws<128> udnBuf(reinterpret_cast<const TByte*>(udn.data()), static_cast<TUint>(udn.size()));
    const std::string room = prefs.Room();
    const Bws<64> roomBuf(reinterpret_cast<const TByte*>(room.data()), static_cast<TUint>(room.size()));

    RadioBrowserMediaServer* server = new RadioBrowserMediaServer(*dvStack, dvStack->Env(), udnBuf, roomBuf);
    // Apply the persisted country / genre lists over the code defaults.
    {
        std::vector<Brn> countries;
        for (const auto& c : prefs.Countries()) {
            countries.push_back(Brn(c.c_str()));
        }
        std::vector<Brn> genres;
        for (const auto& g : prefs.Genres()) {
            genres.push_back(Brn(g.c_str()));
        }
        if (!countries.empty()) {
            server->SetCountryList(countries);
        }
        if (!genres.empty()) {
            server->SetGenreList(genres);
        }
    }
    server->Start();

    // Embedded configuration web ui
    WebConfig web(dvStack->Env(), prefs, *server);
    web.Start(prefs.WebPort());

    Log::Print("RadioBrowserUpnp: media server running (web ui on port %u)\n", prefs.WebPort());

    while (!gQuit) {
        ::usleep(500 * 1000);
    }

    Log::Print("RadioBrowserUpnp: shutting down\n");
    web.Stop();
    prefs.Save(configPath);
    delete server;
    delete lib;
    return 0;
}
