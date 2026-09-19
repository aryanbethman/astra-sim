#include "astra-sim/common/WorkloadPayload.hh"

#include <chrono>
#include <cctype>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <json/json.hpp>
#include <et_def.pb.h>

namespace AstraSim {
namespace {

std::string decode_base64(const std::string& encoded) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string decoded;
  decoded.reserve((encoded.size() * 3) / 4);

  // Unsigned: the accumulator keeps shifting old sextets out of the top, and
  // a signed left shift into the sign bit is undefined (UBSan flags it).
  unsigned int value = 0;
  int bits = -8;
  bool padding_seen = false;
  for (unsigned char character : encoded) {
    if (std::isspace(character)) {
      continue;
    }
    if (character == '=') {
      padding_seen = true;
      continue;
    }
    if (padding_seen) {
      throw std::invalid_argument("base64 data after padding");
    }
    const auto index = alphabet.find(character);
    if (index == std::string::npos) {
      throw std::invalid_argument("invalid base64 character");
    }
    value = (value << 6) + static_cast<int>(index);
    bits += 6;
    if (bits >= 0) {
      decoded.push_back(static_cast<char>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return decoded;
}

using TemplateNodes =
    std::vector<std::shared_ptr<const ChakraProtoMsg::Node>>;

struct TemplateCacheEntry {
  std::shared_ptr<const TemplateNodes> nodes;
  uint64_t last_use = 0;
};

using TemplateCache = std::unordered_map<std::string, TemplateCacheEntry>;

TemplateCache& template_cache() {
  static TemplateCache cache;
  return cache;
}

size_t& template_cache_max_entries() {
  static size_t max_entries = 0;
  return max_entries;
}

uint64_t& template_cache_clock() {
  static uint64_t clock = 0;
  return clock;
}

TemplateCacheStats& template_cache_stats() {
  static TemplateCacheStats stats;
  return stats;
}

std::vector<std::string>& released_template_ids() {
  static std::vector<std::string> released;
  return released;
}

size_t cached_node_count() {
  size_t nodes = 0;
  for (const auto& entry : template_cache()) {
    nodes += entry.second.nodes->size();
  }
  return nodes;
}

void update_template_cache_high_water() {
  auto& stats = template_cache_stats();
  stats.entries = template_cache().size();
  stats.nodes = cached_node_count();
  stats.high_water_entries = std::max(stats.high_water_entries, stats.entries);
  stats.high_water_nodes = std::max(stats.high_water_nodes, stats.nodes);
}

void touch_template(TemplateCache::iterator entry) {
  entry->second.last_use = ++template_cache_clock();
}

void reclaim_inactive_templates() {
  const size_t max_entries = template_cache_max_entries();
  if (max_entries == 0) {
    update_template_cache_high_water();
    return;
  }

  auto& cache = template_cache();
  auto& stats = template_cache_stats();
  while (cache.size() > max_entries) {
    auto victim = cache.end();
    for (auto entry = cache.begin(); entry != cache.end(); ++entry) {
      // The cache itself is the sole owner only after every active/pending
      // rank feeder released this immutable structure.
      if (entry->second.nodes.use_count() == 1 &&
          (victim == cache.end() ||
           entry->second.last_use < victim->second.last_use)) {
        victim = entry;
      }
    }
    if (victim == cache.end()) {
      ++stats.blocked_evictions;
      break;
    }
    released_template_ids().push_back(victim->first);
    cache.erase(victim);
    ++stats.evictions;
  }
  update_template_cache_high_water();
}

void cache_templates(const nlohmann::json& templates) {
  const auto started = std::chrono::steady_clock::now();
  if (!templates.is_object()) {
    throw std::invalid_argument("template bundle templates must be an object");
  }
  auto& cache = template_cache();
  for (auto it = templates.begin(); it != templates.end(); ++it) {
    if (!it.value().is_array()) {
      throw std::invalid_argument("template payload must be an array");
    }
    auto nodes = std::make_shared<TemplateNodes>();
    nodes->reserve(it.value().size());
    for (const auto& encoded_node : it.value()) {
      if (!encoded_node.is_string()) {
        throw std::invalid_argument("template node must be base64 text");
      }
      auto node = std::make_shared<ChakraProtoMsg::Node>();
      if (!node->ParseFromString(decode_base64(encoded_node.get<std::string>()))) {
        throw std::invalid_argument("invalid serialized template node");
      }
      nodes->push_back(std::move(node));
    }
    const auto cached = cache.find(it.key());
    if (cached == cache.end()) {
      auto inserted = cache.emplace(
          it.key(), TemplateCacheEntry{std::move(nodes), 0});
      touch_template(inserted.first);
    } else if (cached->second.nodes->size() != nodes->size()) {
      throw std::invalid_argument("template ID collision");
    } else {
      for (size_t index = 0; index < nodes->size(); ++index) {
        if (cached->second.nodes->at(index)->SerializeAsString() !=
            nodes->at(index)->SerializeAsString()) {
          throw std::invalid_argument("template ID collision");
        }
      }
      touch_template(cached);
    }
  }
  update_template_cache_high_water();
  template_cache_stats().template_decode_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count();
}

std::shared_ptr<const RankEtTemplate> parse_template_binding(
    const nlohmann::json& binding) {
  if (!binding.is_object() || !binding.contains("template_id") ||
      !binding.contains("metadata") || !binding.contains("nodes")) {
    throw std::invalid_argument("invalid template binding");
  }
  const std::string template_id = binding.at("template_id").get<std::string>();
  const auto cached = template_cache().find(template_id);
  if (cached == template_cache().end()) {
    throw std::invalid_argument("template binding refers to unknown template");
  }
  if (!binding.at("nodes").is_object()) {
    throw std::invalid_argument("template binding nodes must be an object");
  }

  // Metadata is intentionally decoded for wire-format validation.  ETFeeder
  // does not consume it after trace initialization, so a direct template does
  // not retain or serialize it for every rank.
  (void)decode_base64(binding.at("metadata").get<std::string>());
  auto rank_template = std::make_shared<RankEtTemplate>();
  rank_template->nodes = cached->second.nodes;
  touch_template(cached);
  for (auto it = binding.at("nodes").begin(); it != binding.at("nodes").end(); ++it) {
    const size_t node_index = std::stoull(it.key());
    if (node_index >= rank_template->nodes->size() || !it.value().is_object()) {
      throw std::invalid_argument("invalid template node overlay");
    }
    const auto& encoded_overlay = it.value();
    TemplateNodeOverlay overlay;
    if (encoded_overlay.contains("name") && !encoded_overlay.at("name").is_null()) {
      if (!encoded_overlay.at("name").is_string()) {
        throw std::invalid_argument("template node name must be text");
      }
      overlay.has_name = true;
      overlay.name = encoded_overlay.at("name").get<std::string>();
    }
    if (!encoded_overlay.contains("attrs") || !encoded_overlay.at("attrs").is_array()) {
      throw std::invalid_argument("template node overlay attrs must be an array");
    }
    std::unordered_map<int, bool> positions;
    for (const auto& entry : encoded_overlay.at("attrs")) {
      if (!entry.is_array() || entry.size() != 2 || !entry.at(0).is_number_integer() ||
          !entry.at(1).is_string()) {
        throw std::invalid_argument("invalid template node attribute overlay");
      }
      const int position = entry.at(0).get<int>();
      if (position < 0 || !positions.emplace(position, true).second) {
        throw std::invalid_argument("duplicate template node attribute position");
      }
      ChakraProtoMsg::AttributeProto attribute;
      if (!attribute.ParseFromString(decode_base64(entry.at(1).get<std::string>()))) {
        throw std::invalid_argument("invalid serialized rank attribute");
      }
      overlay.attributes.emplace_back(position, std::move(attribute));
    }
    rank_template->overlays.emplace(node_index, std::move(overlay));
  }
  return rank_template;
}

std::shared_ptr<const RankEtTemplates> parse_template_bundle(
    const nlohmann::json& bundle) {
  const auto started = std::chrono::steady_clock::now();
  if (!bundle.is_object() || !bundle.contains("templates") ||
      !bundle.contains("bindings") || !bundle.at("bindings").is_object()) {
    throw std::invalid_argument("invalid ET template bundle");
  }
  cache_templates(bundle.at("templates"));
  auto templates = std::make_shared<RankEtTemplates>();
  for (auto it = bundle.at("bindings").begin(); it != bundle.at("bindings").end(); ++it) {
    const int rank = std::stoi(it.key());
    if (rank < 0) {
      throw std::invalid_argument("invalid template binding rank");
    }
    templates->emplace(rank, parse_template_binding(it.value()));
  }
  if (templates->empty()) {
    throw std::invalid_argument("ET template bundle is empty");
  }
  // Active RankEtTemplate/ETFeeder instances keep shared ownership of nodes.
  // Only map-only entries can be released, so cache eviction cannot alter an
  // in-flight simulated graph.
  reclaim_inactive_templates();
  template_cache_stats().binding_parse_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count();
  return templates;
}

}  // namespace

bool try_parse_rank_et_payloads(
    const std::string& command,
    std::shared_ptr<const RankEtPayloads>* payloads) {
  constexpr const char* kPayloadPrefix = "ET_PAYLOADS ";
  if (command.compare(0, std::char_traits<char>::length(kPayloadPrefix),
                      kPayloadPrefix) != 0) {
    return false;
  }

  const auto encoded =
      nlohmann::json::parse(command.substr(std::char_traits<char>::length(kPayloadPrefix)));
  if (!encoded.is_object()) {
    throw std::invalid_argument("ET payload bundle must be a JSON object");
  }

  auto decoded = std::make_shared<RankEtPayloads>();
  for (auto it = encoded.begin(); it != encoded.end(); ++it) {
    const int rank = std::stoi(it.key());
    if (rank < 0 || !it.value().is_string()) {
      throw std::invalid_argument("invalid rank ET payload entry");
    }
    const std::string bytes = decode_base64(it.value().get<std::string>());
    if (bytes.empty()) {
      throw std::invalid_argument("rank ET payload is empty");
    }
    decoded->emplace(rank, std::make_shared<const std::string>(bytes));
  }
  if (decoded->empty()) {
    throw std::invalid_argument("ET payload bundle is empty");
  }
  *payloads = decoded;
  return true;
}

bool try_parse_rank_et_templates(
    const std::string& command,
    std::shared_ptr<const RankEtTemplates>* templates) {
  constexpr const char* kTemplatePrefix = "ET_TEMPLATE_BUNDLE ";
  if (command.compare(0, std::char_traits<char>::length(kTemplatePrefix),
                      kTemplatePrefix) != 0) {
    return false;
  }
  *templates = parse_template_bundle(nlohmann::json::parse(
      command.substr(std::char_traits<char>::length(kTemplatePrefix))));
  return true;
}

void configure_template_cache_max_entries(size_t max_entries) {
  template_cache_max_entries() = max_entries;
  reclaim_inactive_templates();
}

std::vector<std::string> take_released_template_ids() {
  auto& released = released_template_ids();
  std::vector<std::string> result;
  result.swap(released);
  return result;
}

TemplateCacheStats get_template_cache_stats() {
  update_template_cache_high_water();
  return template_cache_stats();
}

void record_direct_template_feeder_init(uint64_t duration_ns) {
  auto& stats = template_cache_stats();
  stats.direct_feeder_init_ns += duration_ns;
  ++stats.direct_feeder_inits;
}

}  // namespace AstraSim
