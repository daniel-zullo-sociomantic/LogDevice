/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "RandomCrossDomainNodeSetSelector.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <numeric>
#include <random>

#include <folly/Memory.h>
#include <folly/String.h>

#include "logdevice/common/configuration/NodeLocation.h"
#include "logdevice/common/debug.h"
#include "logdevice/include/types.h"

namespace facebook { namespace logdevice {

int RandomCrossDomainNodeSetSelector::buildDomainMap(
    const std::shared_ptr<ServerConfig>& cfg,
    NodeLocationScope sync_replication_scope,
    const Options* options,
    DomainMap* map) {
  ld_check(map);
  ld_check(sync_replication_scope > NodeLocationScope::NODE &&
           sync_replication_scope < NodeLocationScope::ROOT);

  map->clear();
  for (const auto& it : cfg->getNodes()) {
    node_index_t i = it.first;
    const Configuration::Node* node = &it.second;
    ld_check(node != nullptr);
    if (!node->location.hasValue()) {
      ld_error("Node %d (%s) does not have location information, cross-domain "
               "selection cannot continue.",
               i,
               node->address.toString().c_str());
      return -1;
    }

    const NodeLocation& location = node->location.value();
    ld_check(!location.isEmpty());
    if (!location.scopeSpecified(sync_replication_scope)) {
      ld_error("Node %d (%s) does not have location scope %s specified in "
               "its location %s. Abort.",
               i,
               node->address.toString().c_str(),
               NodeLocation::scopeNames()[sync_replication_scope].c_str(),
               location.toString().c_str());
      return -1;
    }

    // filter nodes excluded from @param options
    if (options != nullptr && options->exclude_nodes.count(i)) {
      // skip the node
      continue;
    }

    // filter non-storage nodes
    if (!node->includeInNodesets()) {
      continue;
    }

    // use the domain name in the sync_replication_scope as the key
    (*map)[location.getDomain(sync_replication_scope)].push_back(i);
  }

  return 0;
}

ReplicationProperty::OldRepresentation
RandomCrossDomainNodeSetSelector::convertReplicationProperty(
    ReplicationProperty replication) {
  return ReplicationProperty::OldRepresentation(
      replication.getReplicationFactor(),
      replication.getDistinctReplicationFactors()[0].first);
}

storage_set_size_t RandomCrossDomainNodeSetSelector::getStorageSetSize(
    logid_t log_id,
    const std::shared_ptr<Configuration>& cfg,
    folly::Optional<int> storage_set_size_target,
    ReplicationProperty replication_property,
    const Options* options) {
  auto replication = convertReplicationProperty(replication_property);
  return getStorageSetSizeImpl(log_id,
                               cfg,
                               storage_set_size_target,
                               replication.sync_replication_scope,
                               replication.replication_factor,
                               nullptr /* domain_map */,
                               options);
}

storage_set_size_t RandomCrossDomainNodeSetSelector::getStorageSetSizeImpl(
    logid_t log_id,
    const std::shared_ptr<Configuration>& cfg,
    folly::Optional<int> storage_set_size_target,
    NodeLocationScope sync_replication_scope,
    int replication_factor,
    DomainMap* domain_map,
    const Options* options) {
  if (sync_replication_scope == NodeLocationScope::NODE) {
    ld_debug("Log %lu is not configured to use cross-domain replication, "
             "fallback to random nodeset selection instead.",
             log_id.val_);
    return RandomNodeSetSelector::getStorageSetSize(
        log_id,
        cfg,
        storage_set_size_target,
        ReplicationProperty({{NodeLocationScope::NODE, replication_factor}}),
        options);
  }

  DomainMap local_domain_map;
  if (!domain_map) {
    domain_map = &local_domain_map;
    if (buildDomainMap(
            cfg->serverConfig(), sync_replication_scope, options, domain_map) !=
        0) {
      err = E::FAILED;
      return 0;
    }
  }
  DomainMap best_domain_map;
  nodeset_size_t best_nodeset_size = 0;
  bool changed_domain_map = false;
  bool retry;
  do {
    retry = false;
    size_t cluster_size = 0;
    size_t min_domain_size = std::numeric_limits<size_t>::max();
    for (auto& domain : *domain_map) {
      cluster_size += domain.second.size();
      min_domain_size = std::min(min_domain_size, domain.second.size());
    }
    const size_t num_domains = domain_map->size();
    size_t nodeset_size = storage_set_size_target.hasValue()
        ? storage_set_size_target.value()
        : cluster_size;
    if (nodeset_size % num_domains != 0 || nodeset_size < replication_factor ||
        nodeset_size > cluster_size ||
        nodeset_size > min_domain_size * num_domains) {
      // ensuring we have at least r nodes in the resulting nodeset
      const ssize_t min_nodes_per_domain =
          std::ceil(double(replication_factor) / num_domains);
      // ensuring we don't select more than cluster size
      const ssize_t max_nodes_per_domain = cluster_size / num_domains;

      // selecting the closest number of nodes per domain to what the
      // nodeset_size setting suggests, within bounds
      size_t closest_nodes_per_domain =
          std::max(min_nodes_per_domain,
                   std::min(max_nodes_per_domain,
                            std::lround(double(nodeset_size) / num_domains)));
      if (closest_nodes_per_domain > min_domain_size) {
        // We have a small domain that is limiting the number of nodes we can
        // select into a nodeset, limiting the closest_nodes_per_domain to it
        closest_nodes_per_domain = min_domain_size;
        retry = true;
      }
      const size_t new_nodeset_size = closest_nodes_per_domain * num_domains;

      std::string reason;
      if (nodeset_size % num_domains != 0) {
        reason = "not divisible by the number of domains(" +
            std::to_string(num_domains) + ")";
      } else if (nodeset_size < replication_factor) {
        reason = "smaller than replication_factor (" +
            std::to_string(replication_factor) + ")";
      } else if (nodeset_size > cluster_size) {
        reason = "larger than the number of nodes in the cluster (" +
            std::to_string(cluster_size) + ")";
      } else {
        ld_check(nodeset_size > min_domain_size * num_domains);
        reason = "can't be satisfied due to small domain(s) in the tier "
                 "(domain size == ==" +
            std::to_string(min_domain_size) + ")";
      }
      RATELIMIT_WARNING(std::chrono::seconds(10),
                        10,
                        "NodeSet size (%lu) for log %lu is %s, using nodeset "
                        "size %lu instead",
                        nodeset_size,
                        log_id.val_,
                        reason.c_str(),
                        new_nodeset_size);
      nodeset_size = new_nodeset_size;
    }
    if (!changed_domain_map ||
        (nodeset_size > best_nodeset_size + num_domains)) {
      best_nodeset_size = nodeset_size;
      best_domain_map = *domain_map;
      changed_domain_map = true;
    }
    if (retry) {
      // We were limited by domain(s) with size == min_domain_size. Let's try to
      // remove them from the domain map and see if we end up with a better
      // nodeset.
      for (auto it = domain_map->begin(); it != domain_map->end();) {
        if (it->second.size() == min_domain_size) {
          it = domain_map->erase(it);
        } else {
          ++it;
        }
      }
      if (domain_map->empty()) {
        break;
      }
    }
  } while (retry);
  if (changed_domain_map) {
    *domain_map = best_domain_map;
  }
  return best_nodeset_size;
}

std::tuple<NodeSetSelector::Decision, std::unique_ptr<StorageSet>>
RandomCrossDomainNodeSetSelector::getStorageSet(
    logid_t log_id,
    const std::shared_ptr<Configuration>& cfg,
    const StorageSet* prev,
    const Options* options) {
  auto logcfg = cfg->getLogGroupByIDShared(log_id);
  if (!logcfg) {
    err = E::NOTFOUND;
    return std::make_tuple(Decision::FAILED, nullptr);
  }

  ReplicationProperty replication_property =
      ReplicationProperty::fromLogAttributes(logcfg->attrs());
  auto replication = convertReplicationProperty(replication_property);

  if (replication.sync_replication_scope == NodeLocationScope::NODE) {
    ld_debug("Log %lu is not configured to use cross-domain replication, "
             "fallback to random nodeset selection instead.",
             log_id.val_);
    return RandomNodeSetSelector::getStorageSet(log_id, cfg, prev, options);
  }

  if (replication.sync_replication_scope >= NodeLocationScope::ROOT) {
    ld_error(
        "Cannot select node set for log %lu: invalid sync replication scope: "
        "%s "
        "property: %s",
        log_id.val_,
        NodeLocation::scopeNames()[replication.sync_replication_scope].c_str(),
        replication_property.toString().c_str());
    return std::make_tuple(Decision::FAILED, nullptr);
  }

  // TODO: cache the domain_map if it becomes performance bottleneck
  DomainMap domain_map;
  if (buildDomainMap(cfg->serverConfig(),
                     replication.sync_replication_scope,
                     options,
                     &domain_map) != 0) {
    return std::make_tuple(Decision::FAILED, nullptr);
  }

  size_t nodeset_size =
      getStorageSetSizeImpl(log_id,
                            cfg,
                            *logcfg->attrs().nodeSetSize(),
                            replication.sync_replication_scope,
                            replication.replication_factor,
                            &domain_map, // can be modified
                            options);

  const size_t num_domains = domain_map.size();
  ld_check(num_domains > 0);

  const size_t nodes_per_domain = nodeset_size / num_domains;
  auto result = std::make_unique<StorageSet>();

  for (const auto& kv : domain_map) {
    const auto& domain_nodes = kv.second;
    if (domain_nodes.size() < nodes_per_domain) {
      ld_error("There are not enough nodes in domain %s, required %lu, "
               "actual %lu, logid %lu, nodeset_size %lu, num_domains %lu.",
               kv.first.c_str(),
               nodes_per_domain,
               domain_nodes.size(),
               log_id.val_,
               nodeset_size,
               num_domains);
      return std::make_tuple(Decision::FAILED, nullptr);
    }

    // 1. when selecting nodes from a domain, we still prefer positive weight
    // nodes over zero-weight ones
    // 2. With too many zero-weight nodes, it is possible that the nodeset
    // selected does not satisfy the replication requirement. To prevent the
    // loss of write availability, consider the nodeset selection failed.
    auto selected_nodes = randomlySelectNodes(
        log_id, cfg, domain_nodes, nodes_per_domain, options);

    if (selected_nodes == nullptr) {
      ld_error(
          "Not enough positive weight nodes in domain %s", kv.first.c_str());
      return std::make_tuple(Decision::FAILED, nullptr);
    }

    ld_check(selected_nodes->size() == nodes_per_domain);
    result->reserve(result->size() + selected_nodes->size());
    result->insert(
        result->end(), selected_nodes->begin(), selected_nodes->end());
  }

  ld_check(result->size() == nodeset_size);
  std::sort(result->begin(), result->end());

  // as we do not consider weight in nodeset selection, it is possible that
  // the final nodeset does not satisfied the replication requirement in case
  // many nodes are of 0 weight. To prevent the loss of write availability,
  // fail the nodeset selection.
  const auto& all_nodes = cfg->serverConfig()->getNodes();
  if (!ServerConfig::validStorageSet(
          all_nodes, *result, replication_property)) {
    ld_error("Invalid nodeset %s for log %lu, check nodes weights.",
             toString(*result).c_str(),
             log_id.val_);
    return std::make_tuple(Decision::FAILED, nullptr);
  }

  if (prev && *prev == *result) {
    return std::make_tuple(Decision::KEEP, nullptr);
  }

  return std::make_tuple(Decision::NEEDS_CHANGE, std::move(result));
}

}} // namespace facebook::logdevice
