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
    TIpAddress subnet = (*subnetList)[0]->Subnet();
    Library::DestroySubnetList(subnetList);
    DvStack* dvStack = lib->StartDv();
    dvStack->Start();
    (void)subnet;

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
