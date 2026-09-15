/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/ChunkIdGenerator.hh"
#include <cassert>

using namespace AstraSimAnalytical;

ChunkIdGenerator::ChunkIdGenerator() noexcept {
    chunk_id_map = {};
}

int ChunkIdGenerator::create_send_chunk_id(
    const int tag,
    const int src,
    const int dest,
    const ChunkSize chunk_size) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size);

    // search whether the key exists
    auto entry = chunk_id_map.find(key);

    // if key doesn't exist, create new entry
    if (entry == chunk_id_map.end()) {
        entry = chunk_id_map.emplace(key, ChunkIdGeneratorEntry()).first;
    }

    // increment id and return
    entry->second.increment_send_id();
    return entry->second.get_send_id();
}

int ChunkIdGenerator::create_recv_chunk_id(
    const int tag,
    const int src,
    const int dest,
    const ChunkSize chunk_size) noexcept {
    assert(tag >= 0);
    assert(src >= 0);
    assert(dest >= 0);
    assert(chunk_size > 0);

    // create key
    const auto key = std::make_tuple(tag, src, dest, chunk_size);

    // search whether the key exists
    auto entry = chunk_id_map.find(key);

    // key doesn't exist, create new entry
    if (entry == chunk_id_map.end()) {
        entry = chunk_id_map.emplace(key, ChunkIdGeneratorEntry()).first;
    }

    // if key exists, increment send id and return
    entry->second.increment_recv_id();
    return entry->second.get_recv_id();
}

void ChunkIdGenerator::release_ids(const int tag,
                                   const int src,
                                   const int dest,
                                   const ChunkSize chunk_size) noexcept {
    const auto entry =
        chunk_id_map.find(std::make_tuple(tag, src, dest, chunk_size));
    assert(entry != chunk_id_map.end());

    // an unmatched send or recv still needs the next id on the other side
    if (entry->second.get_send_id() == entry->second.get_recv_id()) {
        chunk_id_map.erase(entry);
    }
}
