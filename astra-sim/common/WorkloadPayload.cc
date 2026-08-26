#include "astra-sim/common/WorkloadPayload.hh"

#include <cctype>
#include <stdexcept>

#include <json/json.hpp>

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

}  // namespace

bool try_parse_rank_et_payloads(
    const std::string& command,
    std::shared_ptr<const RankEtPayloads>* payloads) {
  constexpr const char* kPrefix = "ET_PAYLOADS ";
  if (command.compare(0, std::char_traits<char>::length(kPrefix), kPrefix) !=
      0) {
    return false;
  }

  const auto encoded =
      nlohmann::json::parse(command.substr(std::char_traits<char>::length(kPrefix)));
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
