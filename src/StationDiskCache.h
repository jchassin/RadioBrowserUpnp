#pragma once

// Persistent on-disk cache of the station lists that have been BROWSED.
//
// Purpose: keep the media server usable when the radio-browser.info api is
// down (it has no real load-balancing; outages happen). Lists that a user
// actually visited are saved to /data and reloaded at startup, so a reboot
// during an api outage still serves real stations.
//
// Design:
// - one json file, one entry per cache key (top / country:X / genre:X /
//   name:X / search) with a fetched_at timestamp
// - written atomically (.tmp + rename) and debounced (no more than one
//   write per interval) to be gentle on nas flash storage
// - only browsed lists are stored (no speculative prefetching)
// - stale data is still served: a fetched_at of days ago beats an empty list

#include "RadioBrowserClient.h"

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace OpenHome {
namespace Av {

class StationDiskCache
{
public:
    static const TUint kMaxLists = 16;      // safety cap on stored lists
    static const TUint64 kWriteIntervalMs = 10 * 60 * 1000; // debounce writes
public:
    StationDiskCache(const std::string& aPath);

    // Load from disk at startup. Returns false if no/invalid cache.
    // Populates aKeys/aLists (key -> stations) in file order.
    TBool Load(std::vector<std::string>& aKeys,
               std::vector<std::vector<RadioBrowserClient::Station>>& aLists);

    // Record a successful fetch. The list is queued for a debounced write.
    void Update(const std::string& aKey,
                const std::vector<RadioBrowserClient::Station>& aStations);

    // Flush pending changes if the debounce interval has elapsed. Call
    // periodically from the main loop (cheap no-op when nothing is pending).
    void FlushIfDue();

    // Flush now (e.g. on shutdown).
    void FlushNow();

private:
    void Write();

private:
    const std::string iPath;
    TBool iDirty;
    TUint64 iLastWriteMs;
    // serialized pending content (key -> json snippet)
    std::vector<std::pair<std::string, std::string>> iPending;
};

} // namespace Av
} // namespace OpenHome
