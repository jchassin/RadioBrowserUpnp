#include "StationDiskCache.h"

#include <OpenHome/Private/Printer.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace OpenHome;

namespace OpenHome {
namespace Av {

// --- json helpers (shared style with WebConfig) ---

static void JsonEscapeInto(std::string& aOut, const std::string& aIn)
{
    aOut += '"';
    for (char c : aIn) {
        switch (c) {
        case '"':  aOut += "\\\""; break;
        case '\\': aOut += "\\\\"; break;
        case '\n': aOut += "\\n"; break;
        case '\r': aOut += "\\r"; break;
        case '\t': aOut += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                aOut += buf;
            }
            else {
                aOut += c;
            }
        }
    }
    aOut += '"';
}

static std::string JsonUnescape(const std::string& aIn)
{
    std::string out;
    out.reserve(aIn.size());
    for (size_t i = 0; i < aIn.size(); i++) {
        if (aIn[i] == '\\' && i + 1 < aIn.size()) {
            const char c = aIn[++i];
            switch (c) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'u': {
                if (i + 4 < aIn.size()) {
                    unsigned v = 0;
                    for (int k = 1; k <= 4; k++) {
                        const char h = aIn[i + k];
                        v *= 16;
                        if (h >= '0' && h <= '9') v += h - '0';
                        else if (h >= 'a' && h <= 'f') v += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') v += h - 'A' + 10;
                        else { v = 0; break; }
                    }
                    i += 4;
                    // utf-8 encode (stations names are utf-8 in practice)
                    if (v < 0x80) {
                        out += static_cast<char>(v);
                    }
                    else if (v < 0x800) {
                        out += static_cast<char>(0xc0 | (v >> 6));
                        out += static_cast<char>(0x80 | (v & 0x3f));
                    }
                    else {
                        out += static_cast<char>(0xe0 | (v >> 12));
                        out += static_cast<char>(0x80 | ((v >> 6) & 0x3f));
                        out += static_cast<char>(0x80 | (v & 0x3f));
                    }
                }
                break;
            }
            default: out += c; break;
            }
        }
        else {
            out += aIn[i];
        }
    }
    return out;
}

// Parse a single station object {"name":"..","uri":"..",...}
// Tolerant: unknown members are skipped; missing members default.
static TBool ParseStation(const std::string& aJson, RadioBrowserClient::Station& aOut)
{
    // simple member scanner: relies on flat object, string or numeric values
    size_t pos = 0;
    auto skipWs = [&](size_t& p) { while (p < aJson.size() && (aJson[p]==' '||aJson[p]=='\t'||aJson[p]=='\n'||aJson[p]=='\r')) p++; };
    skipWs(pos);
    if (pos >= aJson.size() || aJson[pos] != '{') return false;
    pos++;
    for (;;) {
        skipWs(pos);
        if (pos >= aJson.size()) return false;
        if (aJson[pos] == '}') return true;
        if (aJson[pos] == ',') { pos++; continue; }
        if (aJson[pos] != '"') return false;
        // member name
        size_t nameEnd = aJson.find('"', pos + 1);
        if (nameEnd == std::string::npos) return false;
        const std::string name = JsonUnescape(aJson.substr(pos + 1, nameEnd - pos - 1));
        pos = nameEnd + 1;
        skipWs(pos);
        if (pos >= aJson.size() || aJson[pos] != ':') return false;
        pos++;
        skipWs(pos);
        if (pos >= aJson.size()) return false;
        if (aJson[pos] == '"') {
            size_t vEnd = aJson.find('"', pos + 1);
            if (vEnd == std::string::npos) return false;
            const std::string value = JsonUnescape(aJson.substr(pos + 1, vEnd - pos - 1));
            pos = vEnd + 1;
            if (name == "name") aOut.SetName(value);
            else if (name == "uri") aOut.SetUri(value);
            else if (name == "image") aOut.SetImageUri(value);
            else if (name == "tags") aOut.SetTags(value);
            else if (name == "country") aOut.SetCountryCode(value);
            else if (name == "codec") aOut.SetCodec(value);
        }
        else {
            // number
            size_t vEnd = pos;
            while (vEnd < aJson.size() && aJson[vEnd] != ',' && aJson[vEnd] != '}') vEnd++;
            const std::string value = aJson.substr(pos, vEnd - pos);
            pos = vEnd;
            if (name == "bitrate") aOut.SetBitrate(static_cast<TUint>(std::atoi(value.c_str())));
            else if (name == "hls") aOut.SetHls(value != "0" && !value.empty());
            else if (name == "ok") aOut.SetLastCheckOk(value != "0" && !value.empty());
        }
    }
}

static void SerialiseStation(std::string& aOut, const RadioBrowserClient::Station& aStation)
{
    aOut += "{\"name\":";
    JsonEscapeInto(aOut, aStation.NameStr());
    aOut += ",\"uri\":";
    JsonEscapeInto(aOut, aStation.UriStr());
    aOut += ",\"image\":";
    JsonEscapeInto(aOut, aStation.ImageStr());
    aOut += ",\"tags\":";
    JsonEscapeInto(aOut, aStation.TagsStr());
    aOut += ",\"country\":";
    JsonEscapeInto(aOut, aStation.CountryStr());
    aOut += ",\"codec\":";
    JsonEscapeInto(aOut, aStation.CodecStr());
    char buf[96];
    std::snprintf(buf, sizeof(buf), ",\"bitrate\":%u,\"hls\":%d,\"ok\":%d}",
                  aStation.Bitrate(), aStation.Hls() ? 1 : 0, aStation.LastCheckOk() ? 1 : 0);
    aOut += buf;
}

// --- StationDiskCache ---

StationDiskCache::StationDiskCache(const std::string& aPath)
    : iPath(aPath)
    , iDirty(false)
    , iLastWriteMs(0)
{
}

TBool StationDiskCache::Load(std::vector<std::string>& aKeys,
                             std::vector<std::vector<RadioBrowserClient::Station>>& aLists)
{
    aKeys.clear();
    aLists.clear();
    std::ifstream in(iPath);
    if (!in.good()) {
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string doc = ss.str();

    // helpers: skip ws; read a string literal at pos; skip any value with
    // depth tracking (objects/arrays may nest).
    auto skipWs = [&](size_t& p) {
        while (p < doc.size() && (doc[p]==' '||doc[p]=='\t'||doc[p]=='\n'||doc[p]=='\r')) p++;
    };
    auto readString = [&](size_t& p, std::string& aOut) -> bool {
        skipWs(p);
        if (p >= doc.size() || doc[p] != '"') return false;
        size_t e = p + 1;
        while (e < doc.size() && doc[e] != '"') {
            if (doc[e] == '\\') e++; // skip escaped char
            e++;
        }
        if (e >= doc.size()) return false;
        aOut = JsonUnescape(doc.substr(p + 1, e - p - 1));
        p = e + 1;
        return true;
    };
    auto skipValue = [&](size_t& p) -> bool {
        skipWs(p);
        if (p >= doc.size()) return false;
        const char c = doc[p];
        if (c == '"') {
            std::string discard;
            return readString(p, discard);
        }
        if (c == '{' || c == '[') {
            int depth = 0;
            while (p < doc.size()) {
                const char ch = doc[p];
                if (ch == '"') { // skip string contents inside
                    p++;
                    while (p < doc.size() && doc[p] != '"') {
                        if (doc[p] == '\\') p++;
                        p++;
                    }
                }
                else if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']') {
                    depth--;
                    if (depth == 0) { p++; return true; }
                }
                p++;
            }
            return false;
        }
        // number / literal
        while (p < doc.size() && doc[p] != ',' && doc[p] != '}' && doc[p] != ']') p++;
        return true;
    };

    // parse the top-level object; find the "lists" member.
    size_t p = 0;
    skipWs(p);
    if (p >= doc.size() || doc[p] != '{') return false;
    p++;
    TBool foundLists = false;
    for (;;) {
        skipWs(p);
        if (p >= doc.size()) return false;
        if (doc[p] == '}') break;
        if (doc[p] == ',') { p++; continue; }
        std::string member;
        if (!readString(p, member)) return false;
        skipWs(p);
        if (p >= doc.size() || doc[p] != ':') return false;
        p++;
        if (member == "lists") {
            foundLists = true;
            break; // p points at the lists value
        }
        if (!skipValue(p)) return false;
    }
    if (!foundLists) return false;

    // parse the lists object: "<key>": { "fetched_at":N, "stations":[...] }
    skipWs(p);
    if (p >= doc.size() || doc[p] != '{') return false;
    p++;
    for (;;) {
        skipWs(p);
        if (p >= doc.size()) break;
        if (doc[p] == '}') break;
        if (doc[p] == ',') { p++; continue; }
        std::string key;
        if (!readString(p, key)) break;
        skipWs(p);
        if (p >= doc.size() || doc[p] != ':') break;
        p++;
        skipWs(p);
        if (p >= doc.size() || doc[p] != '{') { if (!skipValue(p)) break; continue; }
        p++; // enter list object
        // members: fetched_at (skip), stations (parse)
        std::vector<RadioBrowserClient::Station> stations;
        for (;;) {
            skipWs(p);
            if (p >= doc.size()) return false;
            if (doc[p] == '}') { p++; break; }
            if (doc[p] == ',') { p++; continue; }
            std::string m;
            if (!readString(p, m)) return false;
            skipWs(p);
            if (p >= doc.size() || doc[p] != ':') return false;
            p++;
            if (m == "stations") {
                skipWs(p);
                if (p >= doc.size() || doc[p] != '[') { if (!skipValue(p)) break; continue; }
                p++; // enter array
                for (;;) {
                    skipWs(p);
                    if (p >= doc.size()) return false;
                    if (doc[p] == ']') { p++; break; }
                    if (doc[p] == ',') { p++; continue; }
                    // one station object: find its end with depth tracking,
                    // then parse the substring
                    size_t sStart = p;
                    if (!skipValue(p)) break;
                    RadioBrowserClient::Station st;
                    if (ParseStation(doc.substr(sStart, p - sStart), st)) {
                        stations.push_back(st);
                    }
                    if (stations.size() >= 250) { skipWs(p); while (p < doc.size() && doc[p] != ']') p++; if (p < doc.size()) p++; break; }
                }
            }
            else {
                if (!skipValue(p)) break;
            }
        }
        if (!stations.empty() && aKeys.size() < kMaxLists) {
            aKeys.push_back(key);
            aLists.push_back(std::move(stations));
        }
    }
    Log::Print("StationDiskCache: loaded %d list(s) from disk\n", (int)aKeys.size());
    return !aKeys.empty();
}

void StationDiskCache::Update(const std::string& aKey,
                              const std::vector<RadioBrowserClient::Station>& aStations)
{
    // serialise the list now (cheap: <100KB) so the pending buffer always
    // holds ready-to-write json
    std::string snippet = "{\"stations\":[";
    for (size_t i = 0; i < aStations.size(); i++) {
        if (i > 0) snippet += ",";
        SerialiseStation(snippet, aStations[i]);
    }
    snippet += "]}";

    for (auto& e : iPending) {
        if (e.first == aKey) {
            e.second = snippet;
            iDirty = true;
            return;
        }
    }
    if (iPending.size() >= kMaxLists) {
        iPending.erase(iPending.begin()); // drop oldest pending entry
    }
    iPending.emplace_back(aKey, snippet);
    iDirty = true;
}

void StationDiskCache::FlushIfDue()
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (iDirty && now - iLastWriteMs >= kWriteIntervalMs) {
        Write();
    }
}

void StationDiskCache::FlushNow()
{
    if (iDirty) {
        Write();
    }
}

void StationDiskCache::Write()
{
    if (iPending.empty()) {
        iDirty = false;
        return;
    }
    std::string doc = "{\"lists\":{";
    for (size_t i = 0; i < iPending.size(); i++) {
        if (i > 0) doc += ",";
        doc += "\"";
        JsonEscapeInto(doc, iPending[i].first);
        doc += "\":";
        doc += iPending[i].second;
    }
    doc += "}}";

    const std::string tmp = iPath + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.good()) {
            Log::Print("StationDiskCache: cannot write %s\n", tmp.c_str());
            return;
        }
        out << doc;
    }
    if (std::rename(tmp.c_str(), iPath.c_str()) != 0) {
        Log::Print("StationDiskCache: rename failed\n");
        return;
    }
    iLastWriteMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    iDirty = false;
    Log::Print("StationDiskCache: wrote %d list(s) to disk\n", (int)iPending.size());
}

} // namespace Av
} // namespace OpenHome
