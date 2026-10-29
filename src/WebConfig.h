#pragma once

// Tiny embedded web configuration ui + persistent preferences store for the
// Radio Browser UPnP media server. No external http server needed: the
// config page is served by a dedicated thread on its own port (default
// 8028), bound to all interfaces of the host.

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <string>
#include <vector>

namespace OpenHome {
    class Environment;
namespace Av {

class RadioBrowserMediaServer;

class Preferences
{
public:
    static const TUint kMaxCountries = 16;
    static const TUint kMaxGenres = 16;
public:
    Preferences();

    // Load from / save to a json config file. Missing file => defaults.
    void Load(const std::string& aPath);
    void Save(const std::string& aPath);

    const std::vector<std::string>& Countries() const { return iCountries; }
    const std::vector<std::string>& Genres() const { return iGenres; }
    const std::string& ApiServer() const { return iApiServer; }
    TUint WebPort() const { return iWebPort; }
    const std::string& Room() const { return iRoom; }

    void SetCountries(const std::vector<std::string>& aCountries);
    void SetGenres(const std::vector<std::string>& aGenres);
    void SetApiServer(const std::string& aServer);
    void SetWebPort(TUint aPort);
    void SetRoom(const std::string& aRoom);

private:
    std::vector<std::string> iCountries;
    std::vector<std::string> iGenres;
    std::string iApiServer;
    TUint iWebPort;
    std::string iRoom;
};

// Blocking accept loop; run on its own thread. Serves:
//   GET /            configuration page (html form)
//   POST /save       apply the form (redirect to /)
class WebConfig
{
public:
    WebConfig(Environment& aEnv, Preferences& aPrefs, RadioBrowserMediaServer& aServer);
    ~WebConfig();
    void Start(TUint aPort);
    void Stop();
private:
    static void Entry(void* aArg);
    void Run();
    void HandleRequest(int aFd);
    std::string Page() const;
    void ApplyForm(const std::string& aBody);
private:
    Environment& iEnv;
    Preferences& iPrefs;
    RadioBrowserMediaServer& iServer;
    int iListenFd;
    volatile bool iRunning;
    class ThreadHandle;
    ThreadHandle* iThread;
};

} // namespace Av
} // namespace OpenHome
