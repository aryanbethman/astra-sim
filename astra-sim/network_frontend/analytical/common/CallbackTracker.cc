/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/CallbackTracker.hh"
#include <cassert>

using namespace AstraSimAnalytical;

CallbackTracker::CallbackTracker() noexcept {
    // initialize tracker
    tracker = {};
}

std::optional<CallbackTrackerEntry*> CallbackTracker::search_entry(
    const int tag,
    const int src,
    const int dest,
    const ChunkSize chunk_size,
    const int chunk_id) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);
    assert(chunk_id >= 0);

    // create key and search entry
    const auto key = std::make_tuple(tag, src, dest, chunk_size, chunk_id);
    const auto entry = tracker.find(key);

    // no entry exists
    if (entry == tracker.end()) {
        return std::nullopt;
    }

    // return pointer to entry
    return &(entry->second);
}

CallbackTrackerEntry* CallbackTracker::create_new_entry(
    const int tag,
    const int src,
    const int dest,
    const ChunkSize chunk_size,
    const int chunk_id) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);
    assert(chunk_id >= 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size, chunk_id);

    // create new emtpy entry
    const auto entry = tracker.emplace(key, CallbackTrackerEntry()).first;

    // return pointer to entry
    return &(entry->second);
}

void CallbackTracker::pop_entry(const int tag,
                                const int src,
                                const int dest,
                                const ChunkSize chunk_size,
                                const int chunk_id) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);
    assert(chunk_id >= 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size, chunk_id);

    // find entry
    const auto entry = tracker.find(key);
    assert(entry != tracker.end());  // entry must exist

    // erase entry from the tracker
    tracker.erase(entry);
}

bool CallbackTracker::has_entries(const int tag,
                                  const int src,
                                  const int dest,
                                  const ChunkSize chunk_size) const noexcept {
    // entries of one key are adjacent and ordered by chunk_id, which starts
    // at 0
    const auto entry =
        tracker.lower_bound(std::make_tuple(tag, src, dest, chunk_size, 0));
    return entry != tracker.end() && std::get<0>(entry->first) == tag &&
           std::get<1>(entry->first) == src &&
           std::get<2>(entry->first) == dest &&
           std::get<3>(entry->first) == chunk_size;
}
