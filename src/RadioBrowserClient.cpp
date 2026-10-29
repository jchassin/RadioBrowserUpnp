#include "RadioBrowserClient.h"

#include <OpenHome/Private/Network.h>
#include <OpenHome/Private/Stream.h> // ReaderError (thrown on end of stream)
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/Converter.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/Stream.h>

#include <algorithm>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

using namespace OpenHome;

namespace OpenHome {
namespace Av {

static const Brn kDefaultServer("de1.api.radio-browser.info");
static const TUint kHttpPort = 80;
static const TUint kResponseCapBytes = 4 * 1024 * 1024;

// radio-browser.info requires clients to identify themselves; requests
// without a User-Agent are rejected.
static const Brn kDefaultUserAgent("RadioBrowserUpnp/1.0 (github.com/jchassin/RadioBrowserUpnp)");

// Normalised comparison key for stream urls: scheme, host case and any
// query string/fragment are ignored, so the same stream submitted as e.g.
// http://Host/stream.aac and https://host/stream.aac deduplicates to one.
static void AppendUriKey(Bwx& aKey, const Brx& aUri)
{
    TUint start = 0;
    if (aUri.Bytes() > 7 && Brn(aUri.Ptr(), 7) == Brn("http://")) {
        start = 7;
    }
    else if (aUri.Bytes() > 8 && Brn(aUri.Ptr(), 8) == Brn("https://")) {
        start = 8;
    }
    TUint end = aUri.Bytes();
    for (TUint i = start; i < aUri.Bytes(); i++) {
        const TByte ch = aUri[i];
        if (ch == '?' || ch == '#') {
            end = i;
            break;
        }
    }
    for (TUint i = start; i < end; i++) {
        TByte ch = aUri[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<TByte>(ch - 'A' + 'a');
        }
        aKey.Append(ch);
    }
}

// Minimal JSON value extraction: walk the array of station objects and pull
// the few string/number members we need. Avoids a full JSON dependency while
// tolerating unknown members and ordering changes.
namespace {

class JsonCursor
{
public:
    JsonCursor(const TByte* aData, TUint aBytes)
        : iData(aData), iBytes(aBytes), iPos(0) {}

    void SkipWhitespace()
    {
        while (iPos < iBytes) {
            const TByte ch = iData[iPos];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                iPos++;
            }
            else {
                break;
            }
        }
    }

    TBool AtEnd() const { return iPos >= iBytes; }
    TByte Peek()
    {
        SkipWhitespace();
        if (iPos >= iBytes) {
            throw RadioBrowserClient::ApiError("json: unexpected end");
        }
        return iData[iPos];
    }

    void Consume()
    {
        if (iPos < iBytes) {
            iPos++;
        }
    }

    void Expect(TByte aCh)
    {
        if (Peek() != aCh) {
            char buf[96];
            const TUint ctx = (iBytes - iPos < 32 ? iBytes - iPos : 32);
            std::string around(reinterpret_cast<const char*>(iData + iPos), ctx);
            std::snprintf(buf, sizeof(buf), "json: expected '%c' at %u near: %.40s", aCh, iPos, around.c_str());
            throw RadioBrowserClient::ApiError(buf);
        }
        iPos++;
    }

    // Reads a JSON string (assumes the cursor is on the opening quote).
    void ReadString(Bwx& aDest, TUint aMaxBytes)
    {
        Expect('"');
        aDest.SetBytes(0);
        for (;;) {
            if (iPos >= iBytes) {
                throw RadioBrowserClient::ApiError("json: unterminated string");
            }
            TByte ch = iData[iPos++];
            if (ch == '"') {
                return;
            }
            if (ch == '\\') {
                if (iPos >= iBytes) {
                    throw RadioBrowserClient::ApiError("json: bad escape");
                }
                ch = iData[iPos++];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'u': {
                    if (iPos + 4 > iBytes) {
                        throw RadioBrowserClient::ApiError("json: bad \\u escape");
                    }
                    TUint val = 0;
                    for (TUint i = 0; i < 4; i++) {
                        const TByte hex = iData[iPos++];
                        val = val * 16;
                        if (hex >= '0' && hex <= '9') { val += hex - '0'; }
                        else if (hex >= 'a' && hex <= 'f') { val += hex - 'a' + 10; }
                        else if (hex >= 'A' && hex <= 'F') { val += hex - 'A' + 10; }
                        else { throw RadioBrowserClient::ApiError("json: bad hex"); }
                    }
                    // encode as utf-8
                    if (aDest.Bytes() + 3 <= aMaxBytes) {
                        if (val < 0x80) {
                            aDest.Append(static_cast<TByte>(val));
                        }
                        else if (val < 0x800) {
                            aDest.Append(static_cast<TByte>(0xc0 | (val >> 6)));
                            aDest.Append(static_cast<TByte>(0x80 | (val & 0x3f)));
                        }
                        else {
                            aDest.Append(static_cast<TByte>(0xe0 | (val >> 12)));
                            aDest.Append(static_cast<TByte>(0x80 | ((val >> 6) & 0x3f)));
                            aDest.Append(static_cast<TByte>(0x80 | (val & 0x3f)));
                        }
                    }
                    continue;
                }
                default: break; // \" \\ \/ pass through
                }
            }
            if (aDest.Bytes() < aMaxBytes) {
                aDest.Append(ch);
            }
        }
    }

    // Reads a number literal (integer part is enough for our use).
    TUint ReadNumber()
    {
        TUint val = 0;
        TBool any = false;
        for (;;) {
            if (iPos >= iBytes) {
                break;
            }
            const TByte ch = iData[iPos];
            if (ch >= '0' && ch <= '9') {
                val = val * 10 + (ch - '0');
                any = true;
                iPos++;
            }
            else if (!any && (ch == '-' || ch == '+')) {
                iPos++; // sign; bitrate is never negative
            }
            else {
                break;
            }
        }
        return val;
    }

    // Skips any value (string, number, object, array, literal).
    void SkipValue()
    {
        const TByte ch = Peek();
        if (ch == '"') {
            Bwh discard(512);
            ReadString(discard, discard.MaxBytes());
        }
        else if (ch == '{' || ch == '[') {
            SkipContainer();
        }
        else {
            // number, true, false, null
            while (iPos < iBytes) {
                const TByte c = iData[iPos];
                if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                    c == 'e' || c == 'E' || c == 't' || c == 'r' || c == 'u' ||
                    c == 'f' || c == 'a' || c == 'l' || c == 's' || c == 'n') {
                    iPos++;
                }
                else {
                    break;
                }
            }
        }
    }

    void SkipContainer()
    {
        const TByte open = Peek();
        const TByte close = (open == '{' ? '}' : ']');
        iPos++; // consume opener
        for (;;) {
            SkipWhitespace();
            if (iPos >= iBytes) {
                throw RadioBrowserClient::ApiError("json: unterminated container");
            }
            if (iData[iPos] == close) {
                iPos++;
                return;
            }
            if (iData[iPos] == ',') {
                iPos++;
                continue;
            }
            if (open == '{') {
                // member name then value
                Bwh discardKey(256);
                ReadString(discardKey, discardKey.MaxBytes());
                Expect(':');
            }
            SkipValue();
        }
    }

private:
    const TByte* iData;
    TUint iBytes;
    TUint iPos;
};

} // namespace

// RadioBrowserClient::Station

RadioBrowserClient::Station::Station()
    : iName(kNameMaxBytes)
    , iUri(kUriMaxBytes)
    , iImage(kImageMaxBytes)
    , iTags(kTagsMaxBytes)
    , iCountry(8)
    , iCodec(16)
    , iBitrate(0)
    , iHls(false)
    , iLastCheckOk(false)
{
}

// RadioBrowserClient

RadioBrowserClient::RadioBrowserClient(Environment& aEnv, const Brx& aUserAgent)
    : iEnv(aEnv)
    , iServer(kUriMaxBytes)
    , iRequestHost(kUriMaxBytes)
    , iUserAgent(128)
{
    if (aUserAgent.Bytes() > 0) {
        iUserAgent.Replace(aUserAgent);
    }
    else {
        iUserAgent.Replace(kDefaultUserAgent);
    }
    SetServer(kDefaultServer);
}

RadioBrowserClient::~RadioBrowserClient()
{
}

void RadioBrowserClient::SetServer(const Brx& aServer)
{
    if (aServer.Bytes() > 0 && aServer.Bytes() <= iServer.MaxBytes()) {
        iServer.Replace(aServer);
    }
}

void RadioBrowserClient::SearchByName(const Brx& aName, std::vector<Station>& aResults,
                                      TUint aMaxResults)
{
    // Name may contain spaces; the api expects them percent-encoded or '+'
    Bwh query(kUriMaxBytes);
    query.Append(Brn("/json/stations/search?name="));
    for (TUint i = 0; i < aName.Bytes(); i++) {
        const TByte ch = aName[i];
        if (ch == ' ') {
            query.Append('+');
        }
        else if (Ascii::IsDigit(ch) || Ascii::IsAlphabetic(ch) || ch == '-' || ch == '_') {
            query.Append(ch);
        }
        // other characters are dropped to keep the request valid
        if (query.Bytes() == query.MaxBytes()) {
            break;
        }
    }
    query.Append(Brn("&order=clickcount&reverse=true&hidebroken=true"));
    Execute(query, aResults, aMaxResults);
}

void RadioBrowserClient::TopStations(std::vector<Station>& aResults, TUint aMaxResults)
{
    Execute(Brn("/json/stations/topclick?hidebroken=true"), aResults, aMaxResults);
}

void RadioBrowserClient::ByCountryCode(const Brx& aCountryCode, std::vector<Station>& aResults,
                                       TUint aMaxResults)
{
    Bwh query(128);
    query.Append(Brn("/json/stations/bycountrycodeexact/"));
    query.Append(aCountryCode);
    query.Append(Brn("?order=clickcount&reverse=true&hidebroken=true"));
    Execute(query, aResults, aMaxResults);
}

void RadioBrowserClient::ByTag(const Brx& aTag, std::vector<Station>& aResults,
                               TUint aMaxResults)
{
    Bwh query(256);
    query.Append(Brn("/json/stations/bytagexact/"));
    // tags in urls: spaces -> %20
    for (TUint i = 0; i < aTag.Bytes(); i++) {
        const TByte ch = aTag[i];
        if (ch == ' ') {
            query.Append(Brn("%20"));
        }
        else {
            query.Append(ch);
        }
    }
    query.Append(Brn("?order=clickcount&reverse=true&hidebroken=true"));
    Execute(query, aResults, aMaxResults);
}

// Simple blocking http GET over ohNet's SocketTcpClient; DNS via getaddrinfo.
static TIpAddress ResolveHost(const Brx& aHost)
{
    Bws<256> hostBuf(aHost);
    hostBuf.PtrZ();
    addrinfo hints;
    ::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const TInt rc = ::getaddrinfo(reinterpret_cast<const TChar*>(hostBuf.Ptr()), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        throw RadioBrowserClient::ApiError("dns: cannot resolve api host");
    }

    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    const TUint32 ipv4 = addr->sin_addr.s_addr; // network order, as ohNet expects
    ::freeaddrinfo(res);
    TIpAddress out;
    out.iFamily = kFamilyV4;
    out.iV4 = ipv4;
    return out;
}

void RadioBrowserClient::Execute(const Brx& aPathAndQuery, std::vector<Station>& aResults,
                                 TUint aMaxResults)
{
    aResults.clear();

    // The whole fetch (dns + connect + request + response) may be slow; it
    // runs on an upnp invoker thread and must never leak exceptions into it.
    try {
        const TIpAddress ip = ResolveHost(iServer);
        Endpoint ep(kHttpPort, ip);
        SocketTcpClient sock;
        sock.Open(iEnv);
        sock.Connect(ep, 15 * 1000);

        // Build the request
        Bws<1536> req;
        WriterBuffer w(req);
        w.Write(Brn("GET "));
        w.Write(aPathAndQuery);
        w.Write(Brn(" HTTP/1.1\r\nHost: "));
        w.Write(iServer);
        w.Write(Brn("\r\nUser-Agent: "));
        w.Write(iUserAgent);
        w.Write(Brn("\r\nAccept: application/json\r\nConnection: close\r\n\r\n"));
        sock.Write(req);
        sock.WriteFlush();

        // Read the whole response (Connection: close => read to eof).
        // SocketTcp::Read throws ReaderError when the remote closes (end of
        // stream); that is the normal termination of the response body.
        std::vector<TByte> data;
        data.reserve(64 * 1024);
        try {
            for (;;) {
                Bws<4096> chunk;
                sock.Read(chunk);
                const TUint bytes = chunk.Bytes();
                if (bytes == 0) {
                    break;
                }
                data.insert(data.end(), chunk.Ptr(), chunk.Ptr() + bytes);
                if (data.size() > kResponseCapBytes) {
                    Log::Print("RadioBrowserClient: response exceeds 4MB cap\n");
                    break;
                }
            }
        }
        catch (ReaderError&) {
            // end of stream
        }
        sock.Close();

        // Split headers / body
        TUint bodyStart = 0;
        for (TUint i = 0; i + 3 < data.size(); i++) {
            if (data[i] == '\r' && data[i+1] == '\n' && data[i+2] == '\r' && data[i+3] == '\n') {
                bodyStart = i + 4;
                break;
            }
        }
        if (bodyStart == 0) {
            Log::Print("RadioBrowserClient: malformed http response\n");
            return;
        }
        // basic status check: expect "HTTP/1.1 200"
        if (data.size() < 12 || ::memcmp(data.data(), "HTTP/1.", 7) != 0) {
            Log::Print("RadioBrowserClient: not an http response\n");
            return;
        }
        if (data[9] != '2') {
            Log::Print("RadioBrowserClient: http status not 2xx\n");
            return;
        }

        // Headers (lowercased scan for transfer-encoding / content-length)
        const std::string headers(reinterpret_cast<const char*>(data.data()), bodyStart - 4);
        std::string headersLower(headers);
        std::transform(headersLower.begin(), headersLower.end(), headersLower.begin(), ::tolower);
        const TBool chunked = (headersLower.find("transfer-encoding:") != std::string::npos &&
                               headersLower.find("chunked") != std::string::npos);
        std::vector<TByte> body;
        if (chunked) {
            // dechunk: sequence of "<hex-size>\r\n<data>\r\n" until a 0-size chunk
            TUint pos = bodyStart;
            for (;;) {
                // read the chunk size line
                TUint lineEnd = pos;
                while (lineEnd + 1 < data.size() &&
                       !(data[lineEnd] == '\r' && data[lineEnd + 1] == '\n')) {
                    lineEnd++;
                }
                if (lineEnd + 1 >= data.size()) {
                    break; // truncated
                }
                TUint size = 0;
                TBool any = false;
                for (TUint i = pos; i < lineEnd; i++) {
                    const TByte ch = data[i];
                    TUint v;
                    if (ch >= '0' && ch <= '9') { v = ch - '0'; }
                    else if (ch >= 'a' && ch <= 'f') { v = ch - 'a' + 10; }
                    else if (ch >= 'A' && ch <= 'F') { v = ch - 'A' + 10; }
                    else { v = 0; }
                    size = size * 16 + v;
                    any = true;
                }
                (void)any;
                pos = lineEnd + 2;
                if (size == 0) {
                    break; // final chunk
                }
                if (pos + size > data.size()) {
                    break; // truncated
                }
                body.insert(body.end(), data.begin() + pos, data.begin() + pos + size);
                pos += size + 2; // skip chunk data and trailing crlf
                if (body.size() > kResponseCapBytes) {
                    Log::Print("RadioBrowserClient: response exceeds 4MB cap\n");
                    break;
                }
            }
        }
        else {
            body.assign(data.begin() + bodyStart, data.end());
        }
        ParseStations(body, aResults, aMaxResults);
    }
    catch (ApiError&) {
        Log::Print("RadioBrowserClient: api error\n");
        aResults.clear();
    }
    catch (std::exception&) {
        Log::Print("RadioBrowserClient: network error\n");
        aResults.clear();
    }
    catch (...) {
        Log::Print("RadioBrowserClient: unexpected error\n");
        aResults.clear();
    }
}

void RadioBrowserClient::ParseStations(const std::vector<TByte>& aData,
                                       std::vector<Station>& aResults, TUint aMaxResults)
{
    aResults.clear();
    if (aData.empty()) {
        return;
    }

    try {
        JsonCursor json(aData.data(), static_cast<TUint>(aData.size()));
        json.Expect('[');
        for (;;) {
            json.SkipWhitespace();
            if (json.AtEnd()) {
                break;
            }
            const TByte ch = json.Peek();
            if (ch == ']') {
                break;
            }
            if (ch == ',') {
                json.Consume(); // separator between array elements
                continue;
            }
            if (ch != '{') {
                throw ApiError("json: expected object in array");
            }
            // parse one station object
            json.Expect('{');
            Station s;
            TBool haveUri = false;
            Bwh memberName(64);
            for (;;) {
                json.SkipWhitespace();
                if (json.Peek() == '}') {
                    break;
                }
                if (json.Peek() == ',') {
                    json.Consume();
                    json.SkipWhitespace();
                    if (json.Peek() == '}') {
                        break;
                    }
                }
                json.ReadString(memberName, memberName.MaxBytes());
                json.Expect(':');
                if (memberName == Brn("url_resolved")) {
                    Bwh url(kUriMaxBytes);
                    json.ReadString(url, url.MaxBytes());
                    if (url.Bytes() > 0) {
                        s.iUri.Replace(url); // resolved url wins over the raw one
                        haveUri = true;
                    }
                }
                else if (memberName == Brn("url")) {
                    Bwh url(kUriMaxBytes);
                    json.ReadString(url, url.MaxBytes());
                    if (!haveUri && url.Bytes() > 0) {
                        s.iUri.Replace(url);
                        haveUri = true;
                    }
                }
                else if (memberName == Brn("name")) {
                    json.ReadString(s.iName, s.iName.MaxBytes());
                }
                else if (memberName == Brn("favicon")) {
                    json.ReadString(s.iImage, s.iImage.MaxBytes());
                }
                else if (memberName == Brn("tags")) {
                    json.ReadString(s.iTags, s.iTags.MaxBytes());
                }
                else if (memberName == Brn("countrycode")) {
                    json.ReadString(s.iCountry, s.iCountry.MaxBytes());
                }
                else if (memberName == Brn("codec")) {
                    json.ReadString(s.iCodec, s.iCodec.MaxBytes());
                }
                else if (memberName == Brn("bitrate")) {
                    s.iBitrate = json.ReadNumber();
                }
                else if (memberName == Brn("hls")) {
                    s.iHls = (json.ReadNumber() != 0);
                }
                else if (memberName == Brn("lastcheckok")) {
                    s.iLastCheckOk = (json.ReadNumber() != 0);
                }
                else {
                    json.SkipValue();
                }
            }
            json.Expect('}');

            if (!haveUri || s.iUri.Bytes() == 0) {
                continue; // unusable entry
            }
            if (!s.iLastCheckOk) {
                continue; // station known to be down at last check
            }
            // Deduplicate streams resolving to the same url (popularity
            // ordered; the first occurrence is kept).
            Bws<kUriMaxBytes * 2> newKey;
            AppendUriKey(newKey, s.Uri());
            TBool duplicate = false;
            for (TUint k = 0; k < aResults.size(); k++) {
                Bws<kUriMaxBytes * 2> existingKey;
                AppendUriKey(existingKey, aResults[k].Uri());
                if (existingKey == newKey) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            aResults.push_back(s);
            if (aResults.size() >= aMaxResults) {
                break;
            }
        }
    }
    catch (ApiError& aErr) {
        Log::Print("RadioBrowserClient: %s\n", aErr.what());
        aResults.clear();
    }
}

} // namespace Av
} // namespace OpenHome
