#include "astra-sim/common/WorkloadPayload.hh"

#include <cctype>
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

  int value = 0;
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

void append_varint(std::string* output, size_t value) {
  while (value > 0x7f) {
    output->push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  output->push_back(static_cast<char>(value));
}

void append_frame(std::string* output, const std::string& payload) {
  append_varint(output, payload.size());
  output->append(payload);
}

using TemplateNodes = std::vector<std::string>;
using TemplateCache = std::unordered_map<std::string,
                                        std::shared_ptr<const TemplateNodes>>;

TemplateCache& template_cache() {
  static TemplateCache cache;
  return cache;
}

void cache_templates(const nlohmann::json& templates) {
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
      nodes->push_back(decode_base64(encoded_node.get<std::string>()));
    }
    const auto cached = cache.find(it.key());
    if (cached == cache.end()) {
      cache.emplace(it.key(), std::move(nodes));
    } else if (*cached->second != *nodes) {
      throw std::invalid_argument("template ID collision");
    }
  }
}

std::string materialise_binding(const nlohmann::json& binding) {
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

  std::string output;
  append_frame(&output, decode_base64(binding.at("metadata").get<std::string>()));
  for (size_t node_index = 0; node_index < cached->second->size(); ++node_index) {
    ChakraProtoMsg::Node node;
    if (!node.ParseFromString(cached->second->at(node_index))) {
      throw std::invalid_argument("invalid serialized template node");
    }
    const auto node_key = std::to_string(node_index);
    const auto overlay_it = binding.at("nodes").find(node_key);
    if (overlay_it != binding.at("nodes").end()) {
      const auto& overlay = *overlay_it;
      if (overlay.contains("name") && !overlay.at("name").is_null()) {
        node.set_name(overlay.at("name").get<std::string>());
      }
      if (!overlay.contains("attrs") || !overlay.at("attrs").is_array()) {
        throw std::invalid_argument("template node overlay attrs must be an array");
      }
      std::unordered_map<int, ChakraProtoMsg::AttributeProto> replacements;
      for (const auto& entry : overlay.at("attrs")) {
        if (!entry.is_array() || entry.size() != 2 || !entry.at(0).is_number_integer() ||
            !entry.at(1).is_string()) {
          throw std::invalid_argument("invalid template node attribute overlay");
        }
        ChakraProtoMsg::AttributeProto attribute;
        if (!attribute.ParseFromString(decode_base64(entry.at(1).get<std::string>()))) {
          throw std::invalid_argument("invalid serialized rank attribute");
        }
        if (!replacements.emplace(entry.at(0).get<int>(), std::move(attribute)).second) {
          throw std::invalid_argument("duplicate template node attribute position");
        }
      }
      if (!replacements.empty()) {
        std::vector<ChakraProtoMsg::AttributeProto> retained;
        retained.reserve(node.attr_size());
        for (const auto& attribute : node.attr()) {
          retained.push_back(attribute);
        }
        const int total = static_cast<int>(retained.size() + replacements.size());
        node.clear_attr();
        size_t retained_index = 0;
        for (int position = 0; position < total; ++position) {
          const auto replacement = replacements.find(position);
          if (replacement != replacements.end()) {
            node.add_attr()->CopyFrom(replacement->second);
          } else if (retained_index < retained.size()) {
            node.add_attr()->CopyFrom(retained.at(retained_index++));
          } else {
            throw std::invalid_argument("invalid template node attribute position");
          }
        }
      }
    }
    append_frame(&output, node.SerializeAsString());
  }
  return output;
}

std::shared_ptr<const RankEtPayloads> parse_template_bundle(
    const nlohmann::json& bundle) {
  if (!bundle.is_object() || !bundle.contains("templates") ||
      !bundle.contains("bindings") || !bundle.at("bindings").is_object()) {
    throw std::invalid_argument("invalid ET template bundle");
  }
  cache_templates(bundle.at("templates"));
  auto payloads = std::make_shared<RankEtPayloads>();
  for (auto it = bundle.at("bindings").begin(); it != bundle.at("bindings").end(); ++it) {
    const int rank = std::stoi(it.key());
    if (rank < 0) {
      throw std::invalid_argument("invalid template binding rank");
    }
    payloads->emplace(rank,
                      std::make_shared<const std::string>(materialise_binding(it.value())));
  }
  if (payloads->empty()) {
    throw std::invalid_argument("ET template bundle is empty");
  }
  return payloads;
}

}  // namespace

bool try_parse_rank_et_payloads(
    const std::string& command,
    std::shared_ptr<const RankEtPayloads>* payloads) {
  constexpr const char* kPayloadPrefix = "ET_PAYLOADS ";
  constexpr const char* kTemplatePrefix = "ET_TEMPLATE_BUNDLE ";
  if (command.compare(0, std::char_traits<char>::length(kTemplatePrefix),
                      kTemplatePrefix) == 0) {
    *payloads = parse_template_bundle(nlohmann::json::parse(
        command.substr(std::char_traits<char>::length(kTemplatePrefix))));
    return true;
  }
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

}  // namespace AstraSim
