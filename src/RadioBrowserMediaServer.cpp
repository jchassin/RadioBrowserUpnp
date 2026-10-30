#include "RadioBrowserMediaServer.h"
#include <OpenHome/Net/Private/DviStack.h>
#include <OpenHome/Net/Core/DvDevice.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/Converter.h>
#include <chrono>

using namespace OpenHome;

namespace OpenHome {
namespace Av {

static const Brn kProtocolInfoSink("*:*:*:*"); // accept anything; playback is done by the renderer's own protocols


static const char* kDidlPreamble =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
    " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">";

static const char* kDidlEpilogue = "</DIDL-Lite>";


static const Brn kQualitySuffixHi("/hi");
static const Brn kQualitySuffixMid("/mid");
static const Brn kQualitySuffixLow("/low");

static TBool SplitQuality(const Brx& aId, const Brx& aPrefix, Brn& aKey, Brn& aQuality)
{
    // aId: "<prefix><key>[/hi|/mid|/low]" ; returns false if prefix doesn't match
    if (aId.Bytes() <= aPrefix.Bytes() ||
        Brn(aId.Ptr(), aPrefix.Bytes()) != aPrefix) {
        return false;
    }
    const TByte* p = aId.Ptr() + aPrefix.Bytes();
    TUint bytes = aId.Bytes() - aPrefix.Bytes();
    // look for the quality suffix at the end
    if (bytes > kQualitySuffixHi.Bytes() &&
        Brn(p + bytes - kQualitySuffixHi.Bytes(), kQualitySuffixHi.Bytes()) == kQualitySuffixHi) {
        aQuality.Set(kQualitySuffixHi.Ptr(), kQualitySuffixHi.Bytes());
        aKey.Set(p, bytes - kQualitySuffixHi.Bytes());
        return true;
    }
    if (bytes > kQualitySuffixMid.Bytes() &&
        Brn(p + bytes - kQualitySuffixMid.Bytes(), kQualitySuffixMid.Bytes()) == kQualitySuffixMid) {
        aQuality.Set(kQualitySuffixMid.Ptr(), kQualitySuffixMid.Bytes());
        aKey.Set(p, bytes - kQualitySuffixMid.Bytes());
        return true;
    }
    if (bytes > kQualitySuffixLow.Bytes() &&
        Brn(p + bytes - kQualitySuffixLow.Bytes(), kQualitySuffixLow.Bytes()) == kQualitySuffixLow) {
        aQuality.Set(kQualitySuffixLow.Ptr(), kQualitySuffixLow.Bytes());
        aKey.Set(p, bytes - kQualitySuffixLow.Bytes());
        return true;
    }
    return false;
}

// RadioBrowserMediaServer::ContentDirectory

RadioBrowserMediaServer::ContentDirectory::ContentDirectory(Net::DvDeviceStandard& aDevice, RadioBrowserMediaServer& aServer)
    : DvProviderUpnpOrgContentDirectory1(aDevice)
    , iServer(aServer)
{
    // The generated action GetSystemUpdateID binds its output parameter to the
    // SystemUpdateID property; the property must exist (and be initialised)
    // before any SCPD request or event subscription dereferences it.
    EnablePropertySystemUpdateID();
    SetPropertySystemUpdateID(1);
    EnableActionGetSearchCapabilities();
    EnableActionGetSortCapabilities();
    EnableActionGetSystemUpdateID();
    EnableActionBrowse();
    EnableActionSearch();
}

void RadioBrowserMediaServer::ContentDirectory::GetSearchCapabilities(Net::IDvInvocation& aInvocation, Net::IDvInvocationResponseString& aSearchCaps)
{
    aInvocation.StartResponse();
    aSearchCaps.Write(Brn("dc:title"));
    aSearchCaps.WriteFlush();
    aInvocation.EndResponse();
}

void RadioBrowserMediaServer::ContentDirectory::GetSortCapabilities(Net::IDvInvocation& aInvocation, Net::IDvInvocationResponseString& aSortCaps)
{
    aInvocation.StartResponse();
    aSortCaps.Write(Brx::Empty());
    aSortCaps.WriteFlush();
    aInvocation.EndResponse();
}

void RadioBrowserMediaServer::ContentDirectory::GetSystemUpdateID(Net::IDvInvocation& aInvocation, Net::IDvInvocationResponseUint& aId)
{
    aInvocation.StartResponse();
    aId.Write(iServer.iSystemUpdateId);
    aInvocation.EndResponse();
}

void RadioBrowserMediaServer::ContentDirectory::Browse(Net::IDvInvocation& aInvocation, const Brx& aObjectID, const Brx& aBrowseFlag, const Brx& /*aFilter*/,
                                                       TUint aStartingIndex, TUint aRequestedCount, const Brx& /*aSortCriteria*/,
                                                       Net::IDvInvocationResponseString& aResult, Net::IDvInvocationResponseUint& aNumberReturned,
                                                       Net::IDvInvocationResponseUint& aTotalMatches, Net::IDvInvocationResponseUint& aUpdateID)
{
    Bws<65536> didl;
    TUint numberReturned = 0;
    TUint totalMatches = 0;
    iServer.UpdateId();
    try {
        // BrowseMetadata: return the object itself (as a container).
        static const Brn kBrowseMetadata("BrowseMetadata");
        if (aBrowseFlag == kBrowseMetadata) {
            std::lock_guard<std::recursive_mutex> lock(iServer.iLock);
            didl.Append(Brn(kDidlPreamble));
            iServer.WriteContainerDidl(didl, aObjectID, aObjectID, 0);
            didl.Append(Brn(kDidlEpilogue));
            numberReturned = 1;
            totalMatches = 1;
        }
        else {
        // NOTE: no global lock here. Container browses take the lock only
        // while reading their (small, local) lists; station-list fetches run
        // outside any lock so a slow radio-browser api call never blocks
        // other concurrent browses (this was causing control-point timeouts).
        static const Brn kRoot("0");
        static const Brn kTop("top");
        static const Brn kCountry("country");
        static const Brn kGenre("genre");
        static const Brn kName("name");
        static const Brn kSearch("search");
        static const Brn kCountryPrefix("country/");
        static const Brn kGenrePrefix("genre/");
        static const Brn kNamePrefix("name/");

        if (aObjectID == kRoot) {
            didl.Append(Brn(kDidlPreamble));
            iServer.BrowseRoot(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID == kTop) {
            didl.Append(Brn(kDidlPreamble));
            iServer.BrowseTop(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID == kCountry) {
            didl.Append(Brn(kDidlPreamble));
            iServer.BrowseCountry(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID == kGenre) {
            didl.Append(Brn(kDidlPreamble));
            iServer.BrowseGenre(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID == kName) {
            didl.Append(Brn(kDidlPreamble));
            iServer.BrowseName(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID == kSearch) {
            didl.Append(Brn(kDidlPreamble));
            iServer.BrowseSearch(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID.Bytes() > kCountryPrefix.Bytes() &&
                 Brn(aObjectID.Ptr(), kCountryPrefix.Bytes()) == kCountryPrefix) {
            Brn quality;
            Brn key;
            didl.Append(Brn(kDidlPreamble));
            if (SplitQuality(aObjectID, kCountryPrefix, key, quality)) {
                iServer.BrowseQualityStations(iServer.CountryStations(key), quality, didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            }
            else {
                iServer.BrowseQualityContainers(aObjectID, didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            }
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID.Bytes() > kGenrePrefix.Bytes() &&
                 Brn(aObjectID.Ptr(), kGenrePrefix.Bytes()) == kGenrePrefix) {
            Brn quality;
            Brn key;
            didl.Append(Brn(kDidlPreamble));
            if (SplitQuality(aObjectID, kGenrePrefix, key, quality)) {
                iServer.BrowseQualityStations(iServer.GenreStations(key), quality, didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            }
            else {
                iServer.BrowseQualityContainers(aObjectID, didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            }
            didl.Append(Brn(kDidlEpilogue));
        }
        else if (aObjectID.Bytes() > kNamePrefix.Bytes() &&
                 Brn(aObjectID.Ptr(), kNamePrefix.Bytes()) == kNamePrefix) {
            Brn quality;
            Brn key;
            didl.Append(Brn(kDidlPreamble));
            if (SplitQuality(aObjectID, kNamePrefix, key, quality)) {
                iServer.BrowseQualityStations(iServer.LetterStations(key), quality, didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            }
            else {
                iServer.BrowseQualityContainers(aObjectID, didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
            }
            didl.Append(Brn(kDidlEpilogue));
        }
        else {
            // unknown object: empty result
        }
        }
    }
    catch (RadioBrowserClient::ApiError&) {
        Log::Print("RadioBrowserMediaServer: API request failed\n");
        didl.SetBytes(0);
    }
    catch (std::exception&) {
        Log::Print("RadioBrowserMediaServer: API request failed\n");
        didl.SetBytes(0);
    }

    aInvocation.StartResponse();
    aResult.Write(didl);
    aResult.WriteFlush();
    aNumberReturned.Write(numberReturned);
    aTotalMatches.Write(totalMatches);
    aUpdateID.Write(iServer.iSystemUpdateId);
    aInvocation.EndResponse();
}

void RadioBrowserMediaServer::ContentDirectory::Search(Net::IDvInvocation& aInvocation, const Brx& /*aContainerID*/, const Brx& aSearchCriteria,
                                                       const Brx& /*aFilter*/, TUint aStartingIndex, TUint aRequestedCount, const Brx& /*aSortCriteria*/,
                                                       Net::IDvInvocationResponseString& aResult, Net::IDvInvocationResponseUint& aNumberReturned,
                                                       Net::IDvInvocationResponseUint& aTotalMatches, Net::IDvInvocationResponseUint& aUpdateID)
{
    // The control point search box sends criteria like: dc:title contains "jazz"
    Bws<128> term;
    {
        static const Brn kContains("contains \"");
        const TByte* ptr = aSearchCriteria.Ptr();
        const TUint bytes = aSearchCriteria.Bytes();
        TUint i = 0;
        // find the opening quote of contains "..."
        for (; i + kContains.Bytes() <= bytes; i++) {
            if (Brn(ptr + i, kContains.Bytes()) == kContains) {
                i += kContains.Bytes();
                break;
            }
        }
        if (i < bytes) {
            for (; i < bytes; i++) {
                const TByte ch = ptr[i];
                if (ch == '"') {
                    break;
                }
                if (term.Bytes() < term.MaxBytes()) {
                    term.Append(ch);
                }
            }
        }
    }

    Bws<65536> didl;
    TUint numberReturned = 0;
    TUint totalMatches = 0;
    iServer.UpdateId();
    try {
        std::lock_guard<std::recursive_mutex> lock(iServer.iLock);
        iServer.RunSearch(term);
        didl.Append(Brn(kDidlPreamble));
        iServer.BrowseSearch(didl, aStartingIndex, aRequestedCount, numberReturned, totalMatches);
        didl.Append(Brn(kDidlEpilogue));
    }
    catch (RadioBrowserClient::ApiError&) {
        Log::Print("RadioBrowserMediaServer: search request failed\n");
    }
    catch (std::exception&) {
        Log::Print("RadioBrowserMediaServer: search request failed\n");
    }

    aInvocation.StartResponse();
    aResult.Write(didl);
    aResult.WriteFlush();
    aNumberReturned.Write(numberReturned);
    aTotalMatches.Write(totalMatches);
    aUpdateID.Write(iServer.iSystemUpdateId);
    aInvocation.EndResponse();
}

// RadioBrowserMediaServer::ConnectionManager

RadioBrowserMediaServer::ConnectionManager::ConnectionManager(Net::DvDeviceStandard& aDevice)
    : DvProviderUpnpOrgConnectionManager1(aDevice)
{
    // Generated actions bind output parameters to these properties; they must
    // exist and be initialised.
    static const Brn kProtocolInfo("*:*:*:*");
    EnablePropertySourceProtocolInfo();
    SetPropertySourceProtocolInfo(Brx::Empty());
    EnablePropertySinkProtocolInfo();
    SetPropertySinkProtocolInfo(kProtocolInfo);
    EnablePropertyCurrentConnectionIDs();
    SetPropertyCurrentConnectionIDs(Brx::Empty());
    EnableActionGetProtocolInfo();
    EnableActionGetCurrentConnectionIDs();
    EnableActionGetCurrentConnectionInfo();
}

void RadioBrowserMediaServer::ConnectionManager::GetProtocolInfo(Net::IDvInvocation& aInvocation, Net::IDvInvocationResponseString& aSource, Net::IDvInvocationResponseString& aSink)
{
    aInvocation.StartResponse();
    aSource.Write(Brx::Empty());
    aSource.WriteFlush();
    aSink.Write(kProtocolInfoSink);
    aSink.WriteFlush();
    aInvocation.EndResponse();
}

void RadioBrowserMediaServer::ConnectionManager::GetCurrentConnectionIDs(Net::IDvInvocation& aInvocation, Net::IDvInvocationResponseString& aConnectionIDs)
{
    aInvocation.StartResponse();
    aConnectionIDs.Write(Brx::Empty());
    aConnectionIDs.WriteFlush();
    aInvocation.EndResponse();
}

void RadioBrowserMediaServer::ConnectionManager::GetCurrentConnectionInfo(Net::IDvInvocation& aInvocation, TInt /*aConnectionID*/,
                                                                          Net::IDvInvocationResponseInt& aRcsID, Net::IDvInvocationResponseInt& aAVTransportID,
                                                                          Net::IDvInvocationResponseString& aProtocolInfo, Net::IDvInvocationResponseString& aPeerConnectionManager,
                                                                          Net::IDvInvocationResponseInt& aPeerConnectionID, Net::IDvInvocationResponseString& aDirection,
                                                                          Net::IDvInvocationResponseString& aStatus)
{
    aInvocation.StartResponse();
    aRcsID.Write(0);
    aAVTransportID.Write(0);
    aProtocolInfo.Write(Brx::Empty());
    aProtocolInfo.WriteFlush();
    aPeerConnectionManager.Write(Brx::Empty());
    aPeerConnectionManager.WriteFlush();
    aPeerConnectionID.Write(0);
    aDirection.Write(Brn("Output"));
    aDirection.WriteFlush();
    aStatus.Write(Brn("OK"));
    aStatus.WriteFlush();
    aInvocation.EndResponse();
}

// RadioBrowserMediaServer

RadioBrowserMediaServer::RadioBrowserMediaServer(Net::DvStack& aDvStack,
                                                 Environment& aEnv,
                                                 const Brx& aUdn,
                                                 const Brx& aFriendlyName,
                                                 const std::string& aCachePath)
    : iDevice(nullptr)
    , iClient(aEnv, Brx::Empty())
    , iSystemUpdateId(1)
    , iTopCacheTimeMs(0)
    , iSearchTerm(128)
    , iSearchCacheTimeMs(0)
    , iDiskCache(aCachePath)
{
    // Preload the persistent station cache into the memory cache so the very
    // first browse after a (re)start is served from disk - the point of the
    // cache is precisely to survive api outages and reboots.
    {
        std::vector<std::string> keys;
        std::vector<std::vector<RadioBrowserClient::Station>> lists;
        if (iDiskCache.Load(keys, lists)) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < keys.size() && i < lists.size(); i++) {
                const std::string& k = keys[i];
                if (k == "top") {
                    iTopCache = lists[i];
                    iTopCacheTimeMs = now; // consider fresh; normal expiry will re-fetch
                    continue;
                }
                if (iListCache.size() >= 8) {
                    break; // memory cache capacity
                }
                ListCacheEntry entry;
                entry.iKey.Replace(Brn(reinterpret_cast<const TByte*>(k.data()),
                                       static_cast<TUint>(k.size())));
                entry.iStations = std::move(lists[i]);
                entry.iTimeMs = now;
                iListCache.push_back(std::move(entry));
            }
        }
    }
    iDevice = new Net::DvDeviceStandard(aDvStack, aUdn);
    iDevice->SetAttribute("Upnp.Domain", "upnp.org");
    iDevice->SetAttribute("Upnp.Type", "MediaServer");
    iDevice->SetAttribute("Upnp.Version", "1");
    iDevice->SetAttribute("Upnp.Manufacturer", "OpenHome");
    iDevice->SetAttribute("Upnp.ModelName", "Radio Browser");
    iDevice->SetAttribute("Upnp.ModelDescription", "Community radio directory (radio-browser.info)");
    Bws<128> friendlyName("Radio Browser ");
    friendlyName.Append(aFriendlyName);
    friendlyName.PtrZ(); // ensure null termination for SetAttribute
    iDevice->SetAttribute("Upnp.FriendlyName", reinterpret_cast<const TChar*>(friendlyName.Ptr()));

    iContentDirectory = std::make_unique<ContentDirectory>(*iDevice, *this);
    iConnectionManager = std::make_unique<ConnectionManager>(*iDevice);

    // Short country / genre lists (defaults; configurable via web ui).
    iCountries = { Brn("FR"), Brn("ES"), Brn("GB"), Brn("DE"), Brn("IT"), Brn("US") };
    iGenres = { Brn("jazz"), Brn("rock"), Brn("classical"), Brn("news"), Brn("pop"), Brn("electronic") };
}

RadioBrowserMediaServer::~RadioBrowserMediaServer()
{
    delete iDevice;
}

void RadioBrowserMediaServer::Start()
{
    iDevice->SetEnabled();
}

void RadioBrowserMediaServer::SetCountryList(const std::vector<Brn>& aCountries)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    iCountries = aCountries;
    UpdateId();
}

void RadioBrowserMediaServer::SetGenreList(const std::vector<Brn>& aGenres)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    iGenres = aGenres;
    UpdateId();
}

void RadioBrowserMediaServer::UpdateId()
{
    iSystemUpdateId++;
}

void RadioBrowserMediaServer::BrowseRoot(Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    static const Brn kTopId("top");
    static const Brn kCountryId("country");
    static const Brn kGenreId("genre");
    static const Brn kNameId("name");
    static const Brn kTopTitle("Most popular");
    static const Brn kCountryTitle("By country");
    static const Brn kGenreTitle("By genre");
    static const Brn kNameTitle("By name");
    // Control points use upnp:childCount to decide whether a container is
    // worth browsing; announcing 0 makes the Linn app show an empty root.
    // Report realistic counts (top may not be cached yet at first browse;
    // use the cache size or the max as a hint).
    const TUint topCount = (iTopCache.empty() ? kMaxStations : (TUint)iTopCache.size());
    const TUint countryCount = (TUint)iCountries.size();
    const TUint genreCount = (TUint)iGenres.size();
    const TUint nameCount = 26; // A-Z
    struct Entry { const Brx* id; const Brx* title; TUint childCount; };
    const Entry entries[] = {
        { &kTopId, &kTopTitle, topCount },
        { &kCountryId, &kCountryTitle, countryCount },
        { &kGenreId, &kGenreTitle, genreCount },
        { &kNameId, &kNameTitle, nameCount },
    };
    const TUint total = sizeof(entries) / sizeof(entries[0]);
    aTotalMatches = total;
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < total && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        WriteContainerDidl(aDidl, *entries[i].id, *entries[i].title, entries[i].childCount);
        returned++;
    }
    aNumberReturned = returned;
}

void RadioBrowserMediaServer::BrowseTop(Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    const std::vector<RadioBrowserClient::Station>& stations = TopStations();
    aTotalMatches = stations.size();
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < stations.size() && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<2048> entry;
        WriteStationDidl(entry, stations[i], i);
        // Reserve room for the closing </DIDL-Lite> epilogue appended by
        // the handler after this loop; filling the buffer to the brim would
        // overflow on the epilogue append (Bwx::Append asserts).
        const TUint reserved = 64;
        if (aDidl.Bytes() + entry.Bytes() + reserved <= aDidl.MaxBytes()) {
            aDidl.Append(entry);
            returned++;
        }
        else {
            break;
        }
    }
    aNumberReturned = returned;
    // Do not advertise more than this page can serve: the didl buffer caps
    // the number of items per response; announcing the full list invites
    // control points to page through (each page = a fresh slow api call),
    // which times out. Clamp to what is here.
    if (returned < aRequestedCount || aRequestedCount == 0) {
        aTotalMatches = aStartingIndex + returned;
    }
}

void RadioBrowserMediaServer::BrowseCountry(Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    const TUint total = iCountries.size();
    aTotalMatches = total;
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < total && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<64> id("country/");
        id.Append(iCountries[i]);
        // hint at the number of stations; the exact count requires an api call
        WriteContainerDidl(aDidl, id, iCountries[i], kMaxStations);
        returned++;
    }
    aNumberReturned = returned;
}

void RadioBrowserMediaServer::BrowseGenre(Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    const TUint total = iGenres.size();
    aTotalMatches = total;
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < total && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<64> id("genre/");
        id.Append(iGenres[i]);
        WriteContainerDidl(aDidl, id, iGenres[i], kMaxStations);
        returned++;
    }
    aNumberReturned = returned;
}

void RadioBrowserMediaServer::BrowseName(Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    // A-Z, using the ascii codes 0x41..0x5A
    const TUint total = 26;
    aTotalMatches = total;
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < total && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<8> id("name/");
        Bws<2> letter;
        letter.Append(static_cast<TByte>('A' + i));
        id.Append(letter);
        WriteContainerDidl(aDidl, id, letter, kMaxStations);
        returned++;
    }
    aNumberReturned = returned;
}

void RadioBrowserMediaServer::BrowseQualityContainers(const Brx& aParentId, Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    // Three quality sub-containers, each with the parent's station count.
    static const Brn kHiTitle("Hi-Res (>=192 kbps)");
    static const Brn kMidTitle("Mid-Res (96-191 kbps)");
    static const Brn kLowTitle("Low-Res (<96 kbps)");
    struct Entry { const TChar* suffix; const Brx* title; };
    const Entry entries[] = {
        { "/hi", &kHiTitle },
        { "/mid", &kMidTitle },
        { "/low", &kLowTitle },
    };
    const TUint total = sizeof(entries) / sizeof(entries[0]);
    aTotalMatches = total;
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < total && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<128> id(aParentId);
        id.Append(Brn(entries[i].suffix));
        WriteContainerDidl(aDidl, id, *entries[i].title, kMaxStations);
        returned++;
    }
    aNumberReturned = returned;
}

// Simple case-insensitive latin sort key: uppercases ascii letters and
// maps common accented latin-1 letters to their base letter.
static TChar NormaliseSortChar(TChar aChar)
{
    if (aChar >= 'a' && aChar <= 'z') {
        return static_cast<TChar>(aChar - 'a' + 'A');
    }
    if (aChar >= 'A' && aChar <= 'Z') {
        return aChar;
    }
    if (aChar >= 0xC0) { // latin-1 supplement: accented letters are 2 bytes utf-8; treat non-ascii as neutral
        return aChar;
    }
    return aChar;
}

static TBool NameLess(const RadioBrowserClient::Station& aLhs, const RadioBrowserClient::Station& aRhs)
{
    const Brx& l = aLhs.Name();
    const Brx& r = aRhs.Name();
    const TUint bytes = (l.Bytes() < r.Bytes() ? l.Bytes() : r.Bytes());
    for (TUint i = 0; i < bytes; i++) {
        const TChar lc = NormaliseSortChar(l[i]);
        const TChar rc = NormaliseSortChar(r[i]);
        if (lc != rc) {
            return lc < rc;
        }
    }
    return l.Bytes() < r.Bytes();
}

void RadioBrowserMediaServer::BrowseQualityStations(const std::vector<RadioBrowserClient::Station>& aStations, const Brx& aQuality, Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    static const Brn kHi("/hi");
    static const Brn kMid("/mid");
    static const Brn kLow("/low");
    // bucket by bitrate (unknown bitrate, typically hls, counts as mid)
    std::vector<const RadioBrowserClient::Station*> bucket;
    for (const auto& station : aStations) {
        const TUint bitrate = station.Bitrate();
        if (aQuality == kHi) {
            if (bitrate >= 192) {
                bucket.push_back(&station);
            }
        }
        else if (aQuality == kMid) {
            if (bitrate == 0 || (bitrate >= 96 && bitrate < 192)) {
                bucket.push_back(&station);
            }
        }
        else if (aQuality == kLow) {
            if (bitrate > 0 && bitrate < 96) {
                bucket.push_back(&station);
            }
        }
    }
    std::sort(bucket.begin(), bucket.end(), [](const RadioBrowserClient::Station* a, const RadioBrowserClient::Station* b) {
        return NameLess(*a, *b);
    });
    aTotalMatches = bucket.size();
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < bucket.size() && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<2048> entry;
        WriteStationDidl(entry, *bucket[i], i);
        const TUint reserved = 64;
        if (aDidl.Bytes() + entry.Bytes() + reserved <= aDidl.MaxBytes()) {
            aDidl.Append(entry);
            returned++;
        }
        else {
            break;
        }
    }
    aNumberReturned = returned;
    if (returned < aRequestedCount || aRequestedCount == 0) {
        aTotalMatches = aStartingIndex + returned;
    }
}

void RadioBrowserMediaServer::BrowseSearch(Bws<65536>& aDidl, TUint aStartingIndex, TUint aRequestedCount, TUint& aNumberReturned, TUint& aTotalMatches)
{
    std::lock_guard<std::recursive_mutex> lock(iLock);
    aTotalMatches = iSearchCache.size();
    TUint returned = 0;
    for (TUint i = aStartingIndex; i < iSearchCache.size() && (aRequestedCount == 0 || returned < aRequestedCount); i++) {
        Bws<2048> entry;
        WriteStationDidl(entry, iSearchCache[i], i);
        const TUint reserved = 64;
        if (aDidl.Bytes() + entry.Bytes() + reserved <= aDidl.MaxBytes()) {
            aDidl.Append(entry);
            returned++;
        }
        else {
            break;
        }
    }
    aNumberReturned = returned;
    if (returned < aRequestedCount || aRequestedCount == 0) {
        aTotalMatches = aStartingIndex + returned;
    }
}

void RadioBrowserMediaServer::RunSearch(const Brx& aName)
{
    if (aName.Bytes() == 0) {
        return;
    }
    // refresh the search cache if the term changed or the cache is stale
    const TBool sameTerm = (iSearchTerm == aName);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (sameTerm && now - iSearchCacheTimeMs < kCacheLifetimeMs) {
        return;
    }
    iClient.SearchByName(aName, iSearchCache, kMaxStations);
    iSearchTerm.Replace(aName);
    iSearchCacheTimeMs = now;
}


std::vector<RadioBrowserClient::Station> RadioBrowserMediaServer::CachedStations(
    const Brx& aKey,
    std::function<void(std::vector<RadioBrowserClient::Station>&)> aFetcher)
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Fast path: consult the cache under the lock, WITHOUT fetching.
    TBool haveStale = false;
    std::vector<RadioBrowserClient::Station> out;
    {
        std::lock_guard<std::recursive_mutex> lock(iLock);
        for (const auto& entry : iListCache) {
            if (entry.iKey == aKey) {
                if (now - entry.iTimeMs < kCacheLifetimeMs) {
                    return entry.iStations; // fresh: serve as-is
                }
                if (!entry.iStations.empty()) {
                    out = entry.iStations; // stale copy to serve immediately
                    haveStale = true;
                }
                break;
            }
        }
    }

    // Fetch OUTSIDE the lock so concurrent browses of other containers are
    // never blocked by this http call.
    std::vector<RadioBrowserClient::Station> fetched;
    aFetcher(fetched);

    // Store under the lock. If another thread already refreshed the entry
    // while we fetched, prefer the fresher data and serve it.
    {
        std::lock_guard<std::recursive_mutex> lock(iLock);
        for (auto& entry : iListCache) {
            if (entry.iKey == aKey) {
                if (now - entry.iTimeMs < kCacheLifetimeMs) {
                    return entry.iStations; // refreshed by a concurrent fetch
                }
                if (!fetched.empty()) {
                    entry.iStations = fetched;
                    entry.iTimeMs = now;
                    iDiskCache.Update(CacheKeyToDiskKey(aKey), fetched);
                    return entry.iStations;
                }
                if (haveStale) {
                    entry.iTimeMs = now; // retry deadline; serve the stale copy
                    return out;
                }
                return out; // empty
            }
        }
        // New entry. ListCacheEntry is not move-assignable (ohNet buffers),
        // so replace the oldest entry in place instead of erasing.
        if (iListCache.empty()) {
            iListCache.emplace_back();
        }
        TUint oldest = 0;
        if (iListCache.size() >= 8) {
            for (TUint i = 1; i < iListCache.size(); i++) {
                if (iListCache[i].iTimeMs < iListCache[oldest].iTimeMs) {
                    oldest = i;
                }
            }
        }
        else {
            iListCache.emplace_back();
            oldest = (TUint)(iListCache.size() - 1);
        }
        auto& entry = iListCache[oldest];
        entry.iKey.Replace(aKey);
        entry.iStations = fetched;
        entry.iTimeMs = now;
        if (!fetched.empty()) {
            iDiskCache.Update(CacheKeyToDiskKey(aKey), fetched);
        }
        return entry.iStations;
    }
}

std::vector<RadioBrowserClient::Station> RadioBrowserMediaServer::TopStations()
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Fast path under the lock; serve the cache when fresh.
    {
        std::lock_guard<std::recursive_mutex> lock(iLock);
        if (now - iTopCacheTimeMs < kCacheLifetimeMs && !iTopCache.empty()) {
            return iTopCache;
        }
    }

    // Fetch outside the lock so a slow api call never blocks other browses.
    std::vector<RadioBrowserClient::Station> fetched;
    iClient.TopStations(fetched, kMaxStations);

    std::lock_guard<std::recursive_mutex> lock(iLock);
    if (now - iTopCacheTimeMs < kCacheLifetimeMs && !iTopCache.empty()) {
        return iTopCache; // refreshed concurrently; prefer the existing copy
    }
    if (!fetched.empty()) {
        iTopCache = fetched;
        iTopCacheTimeMs = now;
        iDiskCache.Update("top", fetched);
    }
    else if (!iTopCache.empty()) {
        iTopCacheTimeMs = now; // fetch failed; retry later, serve stale
    }
    return iTopCache;
}

std::vector<RadioBrowserClient::Station> RadioBrowserMediaServer::LetterStations(const Brx& aLetter)
{
    Bws<16> key("name:");
    key.Append(aLetter);
    return CachedStations(key, [this, &aLetter](std::vector<RadioBrowserClient::Station>& v) {
        std::vector<RadioBrowserClient::Station> raw;
        iClient.SearchByName(aLetter, raw, kMaxStations * 4);
        const TByte target = static_cast<TByte>(Ascii::ToUpperCase(aLetter.Bytes() > 0 ? aLetter[0] : ' '));
        for (const auto& station : raw) {
            if (station.Name().Bytes() > 0) {
                const TByte first = static_cast<TByte>(Ascii::ToUpperCase(station.Name()[0]));
                if (first == target) {
                    v.push_back(station);
                    if (v.size() >= kMaxStations) {
                        break;
                    }
                }
            }
        }
    });
}

std::vector<RadioBrowserClient::Station> RadioBrowserMediaServer::CountryStations(const Brx& aCountry)
{
    Bws<32> key("country:");
    key.Append(aCountry);
    return CachedStations(key, [this, &aCountry](std::vector<RadioBrowserClient::Station>& v) {
        iClient.ByCountryCode(aCountry, v, kMaxStations);
    });
}

std::vector<RadioBrowserClient::Station> RadioBrowserMediaServer::GenreStations(const Brx& aGenre)
{
    Bws<64> key("genre:");
    key.Append(aGenre);
    return CachedStations(key, [this, &aGenre](std::vector<RadioBrowserClient::Station>& v) {
        iClient.ByTag(aGenre, v, kMaxStations);
    });
}

void RadioBrowserMediaServer::WriteStationDidl(Bws<2048>& aDidl, const RadioBrowserClient::Station& aStation, TUint aIndex)
{
    // Write a bare <item> fragment; the caller wraps the response (once) in a
    // single DIDL-Lite document.
    WriterBuffer w(aDidl);
    w.Write(Brn("<item id=\"station-"));
    Ascii::StreamWriteUint(w, aIndex);
    w.Write(Brn("\" parentID=\"-1\" restricted=\"1\">"));
    w.Write(Brn("<dc:title>"));
    Converter::ToXmlEscaped(w, aStation.Name());
    w.Write(Brn("</dc:title>"));
    w.Write(Brn("<upnp:class>object.item.audioItem</upnp:class>"));
    if (aStation.ImageUri().Bytes() > 0) {
        w.Write(Brn("<upnp:albumArtURI>"));
        Converter::ToXmlEscaped(w, aStation.ImageUri());
        w.Write(Brn("</upnp:albumArtURI>"));
    }
    w.Write(Brn("<res protocolInfo=\""));
    w.Write(kProtocolInfoSink);
    w.Write(Brn("\">"));
    Converter::ToXmlEscaped(w, aStation.Uri());
    w.Write(Brn("</res>"));
    w.Write(Brn("</item>"));
}

void RadioBrowserMediaServer::WriteContainerDidl(Bws<65536>& aDidl, const Brx& aObjectId, const Brx& aTitle, TUint aChildCount)
{
    WriterBuffer w(aDidl);
    w.Write(Brn("<container id=\""));
    Converter::ToXmlEscaped(w, aObjectId);
    w.Write(Brn("\" parentID=\"0\" restricted=\"1\"><dc:title>"));
    Converter::ToXmlEscaped(w, aTitle);
    w.Write(Brn("</dc:title><upnp:class>object.container</upnp:class><upnp:childCount>"));
    Ascii::StreamWriteUint(w, aChildCount);
    w.Write(Brn("</upnp:childCount></container>"));
}

void RadioBrowserMediaServer::CacheMaintenance()
{
    iDiskCache.FlushIfDue();
}

std::string RadioBrowserMediaServer::CacheKeyToDiskKey(const Brx& aKey)
{
    // Disk keys match the in-memory cache keys exactly (e.g. "country:FR") -
    // ':' is perfectly legal in json string keys, and matching the memory key
    // avoids a lossy round-trip conversion.
    return std::string(reinterpret_cast<const TChar*>(aKey.Ptr()), aKey.Bytes());
}

} // namespace Av
} // namespace OpenHome
