#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <et_def.pb.h>

namespace AstraSim {

using RankEtPayloads =
    std::unordered_map<int, std::shared_ptr<const std::string>>;

/** Rank-varying fields over an immutable, shared structural ET node. */
struct TemplateNodeOverlay {
  bool has_name = false;
  std::string name;
  std::vector<std::pair<int, ChakraProtoMsg::AttributeProto>> attributes;
};

/** A rank's view of a cached ET structure, without a serialized ET stream. */
struct RankEtTemplate {
  std::shared_ptr<const std::vector<std::shared_ptr<const ChakraProtoMsg::Node>>>
      nodes;
  std::unordered_map<size_t, TemplateNodeOverlay> overlays;
};

/** Aggregate state for ASTRA's immutable structural-template cache. */
struct TemplateCacheStats {
  size_t entries = 0;
  size_t nodes = 0;
  size_t high_water_entries = 0;
  size_t high_water_nodes = 0;
  size_t evictions = 0;
  size_t blocked_evictions = 0;
};

using RankEtTemplates =
    std::unordered_map<int, std::shared_ptr<const RankEtTemplate>>;

/** Parse an ET_PAYLOADS controller command into rank-indexed ET bytes. */
bool try_parse_rank_et_payloads(
    const std::string& command,
    std::shared_ptr<const RankEtPayloads>* payloads);

/** Parse an ET_TEMPLATE_BUNDLE into shared structural nodes and rank bindings. */
bool try_parse_rank_et_templates(
    const std::string& command,
    std::shared_ptr<const RankEtTemplates>* templates);

/** Bound inactive cached structures; zero preserves the legacy unbounded cache. */
void configure_template_cache_max_entries(size_t max_entries);

/** IDs evicted since the last call, for controller-side cache invalidation. */
std::vector<std::string> take_released_template_ids();

/** Current and high-water template-cache statistics for streamed metrics. */
TemplateCacheStats get_template_cache_stats();

}  // namespace AstraSim
