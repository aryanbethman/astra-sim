#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace AstraSim {

using RankEtPayloads =
    std::unordered_map<int, std::shared_ptr<const std::string>>;

/** Parse an ET_PAYLOADS controller command into rank-indexed ET bytes. */
bool try_parse_rank_et_payloads(
    const std::string& command,
    std::shared_ptr<const RankEtPayloads>* payloads);

}  // namespace AstraSim
