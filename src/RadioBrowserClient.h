#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <stdexcept>
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
        const Brx& Name() const { return iName; }
        const Brx& Uri() const { return iUri; }         // resolved stream url
        const Brx& ImageUri() const { return iImage; }   // favicon (may be empty)
        const Brx& Tags() const { return iTags; }
        const Brx& CountryCode() const { return iCountry; }
        const Brx& Codec() const { return iCodec; }
        TUint Bitrate() const { return iBitrate; }
        TBool Hls() const { return iHls; }
        TBool LastCheckOk() const { return iLastCheckOk; }
    private:
        friend class RadioBrowserClient;
        Bwh iName;
        Bwh iUri;
        Bwh iImage;
        Bwh iTags;
        Bwh iCountry;
        Bwh iCodec;
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
