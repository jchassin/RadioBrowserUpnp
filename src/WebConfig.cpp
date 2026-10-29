#include "WebConfig.h"
#include "RadioBrowserMediaServer.h"

#include <OpenHome/Private/Printer.h>

#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace OpenHome;

namespace OpenHome {
namespace Av {

// Preferences

static const char* kDefaultCountries[] = { "FR", "ES", "GB", "DE", "IT", "US" };
static const char* kDefaultGenres[] = { "jazz", "rock", "classical", "news", "pop", "electronic" };

Preferences::Preferences()
    : iApiServer("de1.api.radio-browser.info")
    , iWebPort(8028)
    , iRoom("Radio")
{
    for (const char* c : kDefaultCountries) {
        iCountries.push_back(c);
    }
    for (const char* g : kDefaultGenres) {
        iGenres.push_back(g);
    }
}

// Minimal json helpers: the config file is small and flat; parse by simple
// string scanning to avoid a json dependency.
static void WriteJsonString(std::ostream& os, const std::string& s)
{
    os << "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            os << '\\' << c;
        }
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            os << buf;
        }
        else {
            os << c;
        }
    }
    os << "\"";
}

static std::string JsonEscape(const std::string& s)
{
    std::ostringstream os;
    WriteJsonString(os, s);
    return os.str();
}

static bool FindValue(const std::string& doc, const std::string& key, std::string& value)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = doc.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = doc.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    pos++;
    while (pos < doc.size() && isspace(static_cast<unsigned char>(doc[pos]))) {
        pos++;
    }
    if (pos >= doc.size()) {
        return false;
    }
    if (doc[pos] == '"') {
        // find the closing quote (no escapes expected in practice)
        const size_t end = doc.find('"', pos + 1);
        if (end == std::string::npos) {
            return false;
        }
        value = doc.substr(pos + 1, end - pos - 1);
        return true;
    }
    // number or bare word until , } or end
    size_t end = pos;
    while (end < doc.size() && doc[end] != ',' && doc[end] != '}' && doc[end] != '\n') {
        end++;
    }
    value = doc.substr(pos, end - pos);
    // trim
    while (!value.empty() && isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return true;
}

static std::vector<std::string> FindArray(const std::string& doc, const std::string& key)
{
    std::vector<std::string> out;
    const std::string needle = "\"" + key + "\"";
    size_t pos = doc.find(needle);
    if (pos == std::string::npos) {
        return out;
    }
    pos = doc.find('[', pos + needle.size());
    if (pos == std::string::npos) {
        return out;
    }
    pos++;
    for (;;) {
        while (pos < doc.size() && isspace(static_cast<unsigned char>(doc[pos]))) {
            pos++;
        }
        if (pos >= doc.size() || doc[pos] == ']') {
            break;
        }
        if (doc[pos] == ',') {
            pos++;
            continue;
        }
        if (doc[pos] == '"') {
            const size_t end = doc.find('"', pos + 1);
            if (end == std::string::npos) {
                break;
            }
            out.push_back(doc.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        }
        else {
            pos++;
        }
    }
    return out;
}

void Preferences::Load(const std::string& aPath)
{
    std::ifstream in(aPath);
    if (!in.good()) {
        return; // defaults
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string doc = ss.str();
    std::string v;
    if (FindValue(doc, "api_server", v) && !v.empty()) {
        iApiServer = v;
    }
    if (FindValue(doc, "web_port", v)) {
        const int port = std::atoi(v.c_str());
        if (port > 0 && port < 65536) {
            iWebPort = static_cast<TUint>(port);
        }
    }
    if (FindValue(doc, "room", v) && !v.empty()) {
        iRoom = v;
    }
    if (FindValue(doc, "udn", v) && !v.empty()) {
        iUdn = v;
    }
    const std::vector<std::string> countries = FindArray(doc, "countries");
    if (!countries.empty()) {
        iCountries = countries;
    }
    const std::vector<std::string> genres = FindArray(doc, "genres");
    if (!genres.empty()) {
        iGenres = genres;
    }
}

void Preferences::Save(const std::string& aPath)
{
    std::ofstream out(aPath);
    if (!out.good()) {
        return;
    }
    out << "{\n";
    out << "  \"room\": " << JsonEscape(iRoom) << ",\n";
    out << "  \"udn\": " << JsonEscape(iUdn) << ",\n";
    out << "  \"api_server\": " << JsonEscape(iApiServer) << ",\n";
    out << "  \"web_port\": " << iWebPort << ",\n";
    out << "  \"countries\": [";
    for (size_t i = 0; i < iCountries.size(); i++) {
        out << (i == 0 ? "" : ", ") << JsonEscape(iCountries[i]);
    }
    out << "],\n";
    out << "  \"genres\": [";
    for (size_t i = 0; i < iGenres.size(); i++) {
        out << (i == 0 ? "" : ", ") << JsonEscape(iGenres[i]);
    }
    out << "]\n";
    out << "}\n";
}

void Preferences::SetCountries(const std::vector<std::string>& aCountries)
{
    iCountries.clear();
    for (TUint i = 0; i < aCountries.size() && i < kMaxCountries; i++) {
        iCountries.push_back(aCountries[i]);
    }
}

void Preferences::SetGenres(const std::vector<std::string>& aGenres)
{
    iGenres.clear();
    for (TUint i = 0; i < aGenres.size() && i < kMaxGenres; i++) {
        iGenres.push_back(aGenres[i]);
    }
}

void Preferences::SetApiServer(const std::string& aServer)
{
    if (!aServer.empty()) {
        iApiServer = aServer;
    }
}

void Preferences::SetWebPort(TUint aPort)
{
    if (aPort > 0 && aPort < 65536) {
        iWebPort = aPort;
    }
}

void Preferences::SetRoom(const std::string& aRoom)
{
    if (!aRoom.empty()) {
        iRoom = aRoom;
    }
}

void Preferences::SetUdn(const std::string& aUdn)
{
    if (!aUdn.empty()) {
        iUdn = aUdn;
    }
}

// WebConfig

class WebConfig::ThreadHandle
{
public:
    pthread_t iHandle;
    bool iStarted;
    ThreadHandle() : iStarted(false) {}
};

WebConfig::WebConfig(Environment& aEnv, Preferences& aPrefs, RadioBrowserMediaServer& aServer)
    : iEnv(aEnv)
    , iPrefs(aPrefs)
    , iServer(aServer)
    , iListenFd(-1)
    , iRunning(false)
    , iThread(new ThreadHandle)
{
}

WebConfig::~WebConfig()
{
    Stop();
    delete iThread;
}

void WebConfig::Start(TUint aPort)
{
    iListenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (iListenFd < 0) {
        Log::Print("WebConfig: cannot create socket\n");
        return;
    }
    const int one = 1;
    (void)::setsockopt(iListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(aPort));
    if (::bind(iListenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(iListenFd, 8) != 0) {
        Log::Print("WebConfig: cannot bind web ui port %u\n", aPort);
        ::close(iListenFd);
        iListenFd = -1;
        return;
    }
    iRunning = true;
    if (::pthread_create(&iThread->iHandle, nullptr, [](void* aArg) -> void* {
            static_cast<WebConfig*>(aArg)->Run();
            return nullptr;
        }, this) == 0) {
        iThread->iStarted = true;
    }
}

void WebConfig::Stop()
{
    if (!iRunning) {
        return;
    }
    iRunning = false;
    if (iListenFd >= 0) {
        ::shutdown(iListenFd, SHUT_RDWR);
        ::close(iListenFd);
        iListenFd = -1;
    }
    if (iThread->iStarted) {
        ::pthread_join(iThread->iHandle, nullptr);
        iThread->iStarted = false;
    }
}

void WebConfig::Run()
{
    while (iRunning) {
        const int fd = ::accept(iListenFd, nullptr, nullptr);
        if (fd < 0) {
            break;
        }
        HandleRequest(fd);
        ::close(fd);
    }
}

static void HttpSend(int aFd, const std::string& aStatus, const std::string& aContentType, const std::string& aBody)
{
    std::ostringstream os;
    os << "HTTP/1.1 " << aStatus << "\r\n"
       << "Content-Type: " << aContentType << "\r\n"
       << "Content-Length: " << aBody.size() << "\r\n"
       << "Connection: close\r\n"
       << "Cache-Control: no-store\r\n"
       << "\r\n" << aBody;
    const std::string out = os.str();
    size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t n = ::send(aFd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) {
            break;
        }
        sent += static_cast<size_t>(n);
    }
}

static std::string UrlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        }
        else if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i+1]);
            const int lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
            }
            else {
                out += s[i];
            }
        }
        else {
            out += s[i];
        }
    }
    return out;
}

void WebConfig::HandleRequest(int aFd)
{
    std::string req;
    char buf[2048];
    size_t contentLength = 0;
    for (;;) {
        const ssize_t n = ::recv(aFd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        req.append(buf, static_cast<size_t>(n));
        const size_t headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const std::string headers = req.substr(0, headerEnd);
            const size_t cl = headers.find("Content-Length:");
            if (cl != std::string::npos) {
                contentLength = static_cast<size_t>(std::atoi(headers.c_str() + cl + 15));
            }
            if (req.size() >= headerEnd + 4 + contentLength) {
                break;
            }
        }
        if (req.size() > 64 * 1024) {
            return;
        }
    }

    const bool isGet = (req.compare(0, 4, "GET ") == 0);
    const bool isPost = (req.compare(0, 5, "POST ") == 0);
    const bool isSave = isPost && req.find("POST /save") == 0;

    if (isSave) {
        const size_t headerEnd = req.find("\r\n\r\n");
        ApplyForm(req.substr(headerEnd + 4));
        HttpSend(aFd, "303 See Other", "text/plain", "");
        return;
    }
    if (isGet) {
        HttpSend(aFd, "200 OK", "text/html; charset=utf-8", Page());
        return;
    }
    HttpSend(aFd, "405 Method Not Allowed", "text/plain", "");
}

std::string WebConfig::Page() const
{
    std::ostringstream os;
    os << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
       << "<title>RadioBrowserUpnp</title>"
       << "<style>body{font-family:sans-serif;margin:2em}input{margin:0.3em}"
       "label{display:block;margin-top:0.6em}button{margin-top:1em;padding:0.4em 1.2em}</style>"
       << "</head><body><h1>RadioBrowserUpnp</h1>"
       << "<p>UPnP Media Server exposing the radio-browser.info community directory.</p>"
       << "<form method=\"post\" action=\"/save\">"
       << "<label>Room (friendly name suffix)<input name=\"room\" value=\"" << iPrefs.Room() << "\"></label>"
       << "<label>API server host<input name=\"api_server\" value=\"" << iPrefs.ApiServer() << "\"></label>"
       << "<label>Web ui port<input name=\"web_port\" value=\"" << iPrefs.WebPort() << "\"></label>"
       << "<label>Countries (comma-separated ISO codes)<input name=\"countries\" value=\"";
    for (size_t i = 0; i < iPrefs.Countries().size(); i++) {
        os << (i == 0 ? "" : ",") << iPrefs.Countries()[i];
    }
    os << "\"></label><label>Genres (comma-separated tags)<input name=\"genres\" value=\"";
    for (size_t i = 0; i < iPrefs.Genres().size(); i++) {
        os << (i == 0 ? "" : ",") << iPrefs.Genres()[i];
    }
    os << "\"></label><button type=\"submit\">Save</button></form>"
       << "<p>Changes apply immediately and are persisted to the config file.</p>"
       << "</body></html>\n";
    return os.str();
}

static void SplitCsv(const std::string& aCsv, std::vector<std::string>& aOut)
{
    aOut.clear();
    std::string current;
    for (char c : aCsv) {
        if (c == ',') {
            if (!current.empty()) {
                aOut.push_back(current);
            }
            current.clear();
        }
        else if (c != ' ' || !current.empty()) {
            current += c;
        }
    }
    if (!current.empty()) {
        aOut.push_back(current);
    }
}

static std::string FormValue(const std::string& aBody, const std::string& aKey)
{
    size_t pos = 0;
    while (pos < aBody.size()) {
        const size_t amp = aBody.find('&', pos);
        const std::string pair = aBody.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == aKey) {
            return UrlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return "";
}

void WebConfig::ApplyForm(const std::string& aBody)
{
    std::string v;
    v = FormValue(aBody, "room");
    if (!v.empty()) {
        iPrefs.SetRoom(v);
    }
    v = FormValue(aBody, "api_server");
    if (!v.empty()) {
        iPrefs.SetApiServer(v);
    }
    v = FormValue(aBody, "web_port");
    if (!v.empty()) {
        const int port = std::atoi(v.c_str());
        if (port > 0 && port < 65536) {
            iPrefs.SetWebPort(static_cast<TUint>(port));
        }
    }
    std::vector<std::string> countries;
    SplitCsv(FormValue(aBody, "countries"), countries);
    if (!countries.empty()) {
        iPrefs.SetCountries(countries);
    }
    std::vector<std::string> genres;
    SplitCsv(FormValue(aBody, "genres"), genres);
    if (!genres.empty()) {
        iPrefs.SetGenres(genres);
    }

    // Push to the running server (lists; api server change needs a restart of
    // the client but is applied on next launch for simplicity).
    std::vector<Brn> countryBuffers;
    for (const auto& c : iPrefs.Countries()) {
        countryBuffers.push_back(Brn(c.c_str()));
    }
    std::vector<Brn> genreBuffers;
    for (const auto& g : iPrefs.Genres()) {
        genreBuffers.push_back(Brn(g.c_str()));
    }
    iServer.SetCountryList(countryBuffers);
    iServer.SetGenreList(genreBuffers);
    Log::Print("WebConfig: preferences updated\n");
}

} // namespace Av
} // namespace OpenHome
