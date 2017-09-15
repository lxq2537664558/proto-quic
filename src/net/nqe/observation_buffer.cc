// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/nqe/observation_buffer.h"

#include <float.h>

#include <algorithm>
#include <utility>

#include "base/macros.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "net/nqe/weighted_observation.h"

namespace net {

namespace nqe {

namespace internal {

ObservationBuffer::ObservationBuffer(double weight_multiplier_per_second,
                                     double weight_multiplier_per_signal_level)
    : weight_multiplier_per_second_(weight_multiplier_per_second),
      weight_multiplier_per_signal_level_(weight_multiplier_per_signal_level),
      tick_clock_(new base::DefaultTickClock()) {
  static_assert(kMaximumObservationsBufferSize > 0U,
                "Minimum size of observation buffer must be > 0");
  DCHECK_LE(0.0, weight_multiplier_per_second_);
  DCHECK_GE(1.0, weight_multiplier_per_second_);
  DCHECK_LE(0.0, weight_multiplier_per_signal_level_);
  DCHECK_GE(1.0, weight_multiplier_per_signal_level_);
}

ObservationBuffer::~ObservationBuffer() {}

void ObservationBuffer::AddObservation(const Observation& observation) {
  DCHECK_LE(observations_.size(), kMaximumObservationsBufferSize);
  // Evict the oldest element if the buffer is already full.
  if (observations_.size() == kMaximumObservationsBufferSize)
    observations_.pop_front();

  observations_.push_back(observation);
  DCHECK_LE(observations_.size(), kMaximumObservationsBufferSize);
}

base::Optional<int32_t> ObservationBuffer::GetPercentile(
    base::TimeTicks begin_timestamp,
    const base::Optional<int32_t>& current_signal_strength,
    int percentile,
    const std::vector<NetworkQualityObservationSource>&
        disallowed_observation_sources) const {
  // Stores weighted observations in increasing order by value.
  std::vector<WeightedObservation> weighted_observations;

  // Total weight of all observations in |weighted_observations|.
  double total_weight = 0.0;

  ComputeWeightedObservations(begin_timestamp, current_signal_strength,
                              &weighted_observations, &total_weight,
                              disallowed_observation_sources);
  if (weighted_observations.empty())
    return base::nullopt;

  double desired_weight = percentile / 100.0 * total_weight;

  double cumulative_weight_seen_so_far = 0.0;
  for (const auto& weighted_observation : weighted_observations) {
    cumulative_weight_seen_so_far += weighted_observation.weight;
    if (cumulative_weight_seen_so_far >= desired_weight)
      return weighted_observation.value;
  }

  // Computation may reach here due to floating point errors. This may happen
  // if |percentile| was 100 (or close to 100), and |desired_weight| was
  // slightly larger than |total_weight| (due to floating point errors).
  // In this case, we return the highest |value| among all observations.
  // This is same as value of the last observation in the sorted vector.
  return weighted_observations.at(weighted_observations.size() - 1).value;
}

base::Optional<int32_t> ObservationBuffer::GetWeightedAverage(
    base::TimeTicks begin_timestamp,
    const base::Optional<int32_t>& current_signal_strength,
    const std::vector<NetworkQualityObservationSource>&
        disallowed_observation_sources) const {
  // Stores weighted observations in increasing order by value.
  std::vector<WeightedObservation> weighted_observations;

  // Total weight of all observations in |weighted_observations|.
  double total_weight = 0.0;

  ComputeWeightedObservations(begin_timestamp, current_signal_strength,
                              &weighted_observations, &total_weight,
                              disallowed_observation_sources);
  if (weighted_observations.empty())
    return base::nullopt;

  // Weighted average is the sum of observations times their respective
  // weights, divided by the sum of the weights of all observations.
  double total_weight_times_value = 0.0;
  for (const auto& weighted_observation : weighted_observations) {
    total_weight_times_value +=
        (weighted_observation.weight * weighted_observation.value);
  }

  return static_cast<int32_t>(total_weight_times_value / total_weight);
}

base::Optional<int32_t> ObservationBuffer::GetUnweightedAverage(
    base::TimeTicks begin_timestamp,
    const base::Optional<int32_t>& current_signal_strength,
    const std::vector<NetworkQualityObservationSource>&
        disallowed_observation_sources) const {
  // Stores weighted observations in increasing order by value.
  std::vector<WeightedObservation> weighted_observations;

  // Total weight of all observations in |weighted_observations|.
  double total_weight = 0.0;

  ComputeWeightedObservations(begin_timestamp, current_signal_strength,
                              &weighted_observations, &total_weight,
                              disallowed_observation_sources);
  if (weighted_observations.empty())
    return base::nullopt;

  // The unweighted average is the sum of all observations divided by the
  // number of observations.
  double total_value = 0.0;
  for (const auto& weighted_observation : weighted_observations)
    total_value += weighted_observation.value;

  return total_value / weighted_observations.size();
}

void ObservationBuffer::ComputeWeightedObservations(
    const base::TimeTicks& begin_timestamp,
    const base::Optional<int32_t>& current_signal_strength,
    std::vector<WeightedObservation>* weighted_observations,
    double* total_weight,
    const std::vector<NetworkQualityObservationSource>&
        disallowed_observation_sources) const {
  DCHECK_GE(Capacity(), Size());

  weighted_observations->clear();
  double total_weight_observations = 0.0;
  base::TimeTicks now = tick_clock_->NowTicks();

  for (const auto& observation : observations_) {
    if (observation.timestamp < begin_timestamp)
      continue;
    bool disallowed = false;
    for (const auto& disallowed_source : disallowed_observation_sources) {
      if (disallowed_source == observation.source)
        disallowed = true;
    }
    if (disallowed)
      continue;
    base::TimeDelta time_since_sample_taken = now - observation.timestamp;
    double time_weight =
        pow(weight_multiplier_per_second_, time_since_sample_taken.InSeconds());

    double signal_strength_weight = 1.0;
    if (current_signal_strength && observation.signal_strength) {
      int32_t signal_strength_weight_diff =
          std::abs(current_signal_strength.value() -
                   observation.signal_strength.value());
      signal_strength_weight =
          pow(weight_multiplier_per_signal_level_, signal_strength_weight_diff);
    }

    double weight = time_weight * signal_strength_weight;

    weight = std::max(DBL_MIN, std::min(1.0, weight));

    weighted_observations->push_back(
        WeightedObservation(observation.value, weight));
    total_weight_observations += weight;
  }

  // Sort the samples by value in ascending order.
  std::sort(weighted_observations->begin(), weighted_observations->end());
  *total_weight = total_weight_observations;

  DCHECK_LE(0.0, *total_weight);
  DCHECK(weighted_observations->empty() || 0.0 < *total_weight);

  // |weighted_observations| may have a smaller size than |observations_|
  // since the former contains only the observations later than
  // |begin_timestamp|.
  DCHECK_GE(observations_.size(), weighted_observations->size());
}

}  // namespace internal

}  // namespace nqe

}  // namespace net