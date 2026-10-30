#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace OpenHome {
    class Environment;
namespace Av {

// Client for the community radio directory at radio-browser.info
// (https://api.radio-browser.info). Uses the public JSON API: no key, no auth.
//
// Ported from ohPlayer's RadioBrowserClient for a standalone ohNet build:
// no ohMediaPlayer UriLoader/SslContext dependency. Plain http is used
// (the API answers both http and https); DNS resolution is done with
// getaddrinfo and the request is sent over ohNet's SocketTcpClient.
//
// All calls are synchronous (blocking); callers run them on UPnP invoker
// threads, never on a pipeline.
class RadioBrowserClient
{
public:
    static const TUint kDefaultMaxResults = 100;
    static const TUint kUriMaxBytes = 512;
    static const TUint kNameMaxBytes = 256;
    static const TUint kImageMaxBytes = 512;
    static const TUint kTagsMaxBytes = 256;
public:
    class Station
    {
    public:
        Station();
        // Views are valid as long as the Station object is alive and unmodified.
        Brn Name() const { return Str(iName); }
        Brn Uri() const { return Str(iUri); }         // resolved stream url
        Brn ImageUri() const { return Str(iImage); }  // favicon (may be empty)
        Brn Tags() const { return Str(iTags); }
        Brn CountryCode() const { return Str(iCountry); }
        Brn Codec() const { return Str(iCodec); }
        TUint Bitrate() const { return iBitrate; }
        TBool Hls() const { return iHls; }
        TBool LastCheckOk() const { return iLastCheckOk; }
    private:
        friend class RadioBrowserClient;
        static Brn Str(const std::string& aStr)
        {
            return Brn(reinterpret_cast<const TByte*>(aStr.data()),
                       static_cast<TUint>(aStr.size()));
        }
        std::string iName;
        std::string iUri;
        std::string iImage;
        std::string iTags;
        std::string iCountry;
        std::string iCodec;
        TUint iBitrate;
        TBool iHls;
        TBool iLastCheckOk;
    };

public:
    class ApiError : public std::runtime_error
    {
    public:
        ApiError(const TChar* aMsg) : std::runtime_error(aMsg) {}
    };

public:
    RadioBrowserClient(Environment& aEnv, const Brx& aUserAgent);
    ~RadioBrowserClient();

    void SetServer(const Brx& aServer);

    void SearchByName(const Brx& aName, std::vector<Station>& aResults,
                      TUint aMaxResults = kDefaultMaxResults);
    void TopStations(std::vector<Station>& aResults,
                     TUint aMaxResults = kDefaultMaxResults);
    void ByCountryCode(const Brx& aCountryCode, std::vector<Station>& aResults,
                       TUint aMaxResults = kDefaultMaxResults);
    void ByTag(const Brx& aTag, std::vector<Station>& aResults,
               TUint aMaxResults = kDefaultMaxResults);

private:
    void Execute(const Brx& aPathAndQuery, std::vector<Station>& aResults,
                 TUint aMaxResults);
    void ParseStations(const std::vector<TByte>& aData,
                       std::vector<Station>& aResults, TUint aMaxResults);

private:
    Environment& iEnv;
    Bwh iServer;
    Bwh iRequestHost;
    Bwh iUserAgent;
    Bwh iRequestUri;
};

} // namespace Av
} // namespace OpenHome
