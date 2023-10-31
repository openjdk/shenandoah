/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */
#include "precompiled.hpp"


#include "gc/shared/gcCause.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahSpaceInfo.hpp"
#include "gc/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/quickSort.hpp"

// These constants are used to adjust the margin of error for the moving
// average of the allocation rate and cycle time. The units are standard
// deviations.
const double ShenandoahAdaptiveHeuristics::FULL_PENALTY_SD = 0.2;
const double ShenandoahAdaptiveHeuristics::DEGENERATE_PENALTY_SD = 0.1;

// These are used to decide if we want to make any adjustments at all
// at the end of a successful concurrent cycle.
const double ShenandoahAdaptiveHeuristics::LOWEST_EXPECTED_AVAILABLE_AT_END = -0.5;
const double ShenandoahAdaptiveHeuristics::HIGHEST_EXPECTED_AVAILABLE_AT_END = 0.5;

// These values are the confidence interval expressed as standard deviations.
// At the minimum confidence level, there is a 25% chance that the true value of
// the estimate (average cycle time or allocation rate) is not more than
// MINIMUM_CONFIDENCE standard deviations away from our estimate. Similarly, the
// MAXIMUM_CONFIDENCE interval here means there is a one in a thousand chance
// that the true value of our estimate is outside the interval. These are used
// as bounds on the adjustments applied at the outcome of a GC cycle.
const double ShenandoahAdaptiveHeuristics::MINIMUM_CONFIDENCE = 0.319; // 25%
const double ShenandoahAdaptiveHeuristics::MAXIMUM_CONFIDENCE = 3.291; // 99.9%

// Given that we ask should_start_gc() approximately once per ms, this sample size corresponds to spikes seen in 16 ms time span.
const size_t ShenandoahAdaptiveHeuristics::SPIKE_ACCELERATION_SAMPLE_SIZE = 16;

// Even if we do not detect acceleration, we may detect a "momentary" spike.  This momentary spike is represented by the average
// allocation rate calculated for the most recently collected SPIKE_DETECTION_SAMPLE_SIZE rates.
//
// SPIKE_DETECTION_SAMPLE_SIZE must be less than or equal to SPIKE_ACCELERATION_SAMPLE_SIZE
const size_t ShenandoahAdaptiveHeuristics::SPIKE_DETECTION_SAMPLE_SIZE = 3;

// Separately, we keep track of the average gc time.  We track the most recent GC_TIME_SAMPLE_SIZE GC times in order to
// detect changing trends in the time required to perform GC.  If the number of samples is too large, we will not be as
// responsive to change trends, as the best-fit line will look more like an average.
const size_t ShenandoahAdaptiveHeuristics::GC_TIME_SAMPLE_SIZE = 4;

const size_t ShenandoahAdaptiveHeuristics::HISTORICAL_PERIOD_SAMPLE_SIZE = 128;

ShenandoahAdaptiveHeuristics::ShenandoahAdaptiveHeuristics(ShenandoahSpaceInfo* space_info) :
  ShenandoahHeuristics(space_info),
  _margin_of_error_sd(ShenandoahAdaptiveInitialConfidence),
  _spike_threshold_sd(ShenandoahAdaptiveInitialSpikeThreshold),
  _last_trigger(OTHER),
  _available(Moving_Average_Samples, ShenandoahAdaptiveDecayFactor),
  _freeset(nullptr),
  _regulator_thread(nullptr),
  _previous_total_allocations(0),
  _previous_allocation_timestamp(0.0),
  _gc_time_first_sample_index(0),
  _gc_time_num_samples(0),
  _gc_time_timestamps(NEW_C_HEAP_ARRAY(double, GC_TIME_SAMPLE_SIZE, mtGC)),
  _gc_time_samples(NEW_C_HEAP_ARRAY(double, GC_TIME_SAMPLE_SIZE, mtGC)),
  _gc_time_xy(NEW_C_HEAP_ARRAY(double, GC_TIME_SAMPLE_SIZE, mtGC)),
  _gc_time_xx(NEW_C_HEAP_ARRAY(double, GC_TIME_SAMPLE_SIZE, mtGC)),
  _gc_time_sum_of_timestamps(0),
  _gc_time_sum_of_samples(0),
  _gc_time_sum_of_xy(0),
  _gc_time_sum_of_xx(0),
  _gc_time_m(0.0),
  _gc_time_b(0.0),
  _gc_time_sd(0.0),
  _spike_acceleration_first_sample_index(0),
  _spike_acceleration_num_samples(0),
  _spike_acceleration_rate_samples(NEW_C_HEAP_ARRAY(double, SPIKE_ACCELERATION_SAMPLE_SIZE, mtGC)),
  _spike_acceleration_rate_timestamps(NEW_C_HEAP_ARRAY(double, SPIKE_ACCELERATION_SAMPLE_SIZE, mtGC)) { }

ShenandoahAdaptiveHeuristics::~ShenandoahAdaptiveHeuristics() {
  FREE_C_HEAP_ARRAY(double, _spike_acceleration_rate_samples);
  FREE_C_HEAP_ARRAY(double, _spike_acceleration_rate_timestamps);
  FREE_C_HEAP_ARRAY(double, _gc_time_timestamps);
  FREE_C_HEAP_ARRAY(double, _gc_time_samples);
  FREE_C_HEAP_ARRAY(double, _gc_time_xy);
  FREE_C_HEAP_ARRAY(double, _gc_time_xx);
}

void ShenandoahAdaptiveHeuristics::initialize() {
  _freeset = ShenandoahHeap::heap()->free_set();
  _regulator_thread = ShenandoahHeap::heap()->regulator_thread();
  _allocation_cliff = ShenandoahHeap::heap()->young_generation()->available();
}

void ShenandoahAdaptiveHeuristics::choose_collection_set_from_regiondata(ShenandoahCollectionSet* cset,
                                                                         RegionData* data, size_t size,
                                                                         size_t actual_free) {
  size_t garbage_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;

  // The logic for cset selection in adaptive is as follows:
  //
  //   1. We cannot get cset larger than available free space. Otherwise we guarantee OOME
  //      during evacuation, and thus guarantee full GC. In practice, we also want to let
  //      application allocate during concurrent GC. This is why we limit CSet to some fraction of
  //      available space. In non-overloaded heap, max_cset would contain all plausible candidates
  //      over garbage threshold.
  //
  //   2. We should not get cset too low so that free threshold would not be met right
  //      after the cycle. Otherwise we get back-to-back cycles for no reason if heap is
  //      too fragmented. In non-overloaded non-fragmented heap min_garbage would be around zero.
  //
  // Therefore, we start by sorting the regions by garbage. Then we unconditionally add the best candidates
  // before we meet min_garbage. Then we add all candidates that fit with a garbage threshold before
  // we hit max_cset. When max_cset is hit, we terminate the cset selection. Note that in this scheme,
  // ShenandoahGarbageThreshold is the soft threshold which would be ignored until min_garbage is hit.

  size_t capacity    = _space_info->soft_max_capacity();
  size_t max_cset    = (size_t)((1.0 * capacity / 100 * ShenandoahEvacReserve) / ShenandoahEvacWaste);
  size_t free_target = (capacity * ShenandoahMinFreeThreshold) / 100 + max_cset;
  size_t min_garbage = (free_target > actual_free) ? (free_target - actual_free) : 0;

  log_info(gc, ergo)("Adaptive CSet Selection. Target Free: " SIZE_FORMAT "%s, Actual Free: "
                     SIZE_FORMAT "%s, Max Evacuation: " SIZE_FORMAT "%s, Min Garbage: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(free_target), proper_unit_for_byte_size(free_target),
                     byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free),
                     byte_size_in_proper_unit(max_cset),    proper_unit_for_byte_size(max_cset),
                     byte_size_in_proper_unit(min_garbage), proper_unit_for_byte_size(min_garbage));

  // Better select garbage-first regions
  QuickSort::sort<RegionData>(data, (int)size, compare_by_garbage, false);

  size_t cur_cset = 0;
  size_t cur_garbage = 0;

  // Regions are sorted in terms of decreasing garbage
  for (size_t idx = 0; idx < size; idx++) {
    ShenandoahHeapRegion* r = data[idx]._region;

    size_t new_cset    = cur_cset + r->get_live_data_bytes();
    size_t new_garbage = cur_garbage + r->garbage();

    if (new_cset > max_cset) {
      // TODO: This region has too much live data, so cannot be included in CSET.  It is still possible that some
      // other region that has less garbage would also have less live data (i.e. total usage is smaller) and so could
      // still be included in the CSET.  Change this to continue.

      break;
    }

    if ((new_garbage < min_garbage) || (r->garbage() > garbage_threshold)) {
      cset->add_region(r);
      cur_cset = new_cset;
      cur_garbage = new_garbage;
    }
  }
}

void ShenandoahAdaptiveHeuristics::record_degenerated_cycle_start(bool out_of_cycle) {
  ShenandoahHeuristics::record_degenerated_cycle_start(out_of_cycle);
  _allocation_rate.allocation_counter_reset();
}

void ShenandoahAdaptiveHeuristics::record_cycle_start() {
  ShenandoahHeuristics::record_cycle_start();
  _allocation_rate.allocation_counter_reset();
}

void ShenandoahAdaptiveHeuristics::add_degenerated_gc_time(double timestamp, double gc_time) {
  // Conservatively add sample into linear model If this time is above the predicted concurrent gc time
  if (predict_gc_time(timestamp) < gc_time) {
    add_gc_time(timestamp, gc_time);
  }
}

void ShenandoahAdaptiveHeuristics::add_gc_time(double timestamp, double gc_time) {
  // Update best-fit linear predictor of GC time
  uint index = (_gc_time_first_sample_index + _gc_time_num_samples) % GC_TIME_SAMPLE_SIZE;
  if (_gc_time_num_samples == GC_TIME_SAMPLE_SIZE) {
    _gc_time_sum_of_timestamps -= _gc_time_timestamps[index];
    _gc_time_sum_of_samples -= _gc_time_samples[index];
    _gc_time_sum_of_xy -= _gc_time_xy[index];
    _gc_time_sum_of_xx -= _gc_time_xx[index];
  }
  _gc_time_timestamps[index] = timestamp;
  _gc_time_samples[index] = gc_time;
  _gc_time_xy[index] = timestamp * gc_time;
  _gc_time_xx[index] = timestamp * timestamp;

  _gc_time_sum_of_timestamps += _gc_time_timestamps[index];
  _gc_time_sum_of_samples += _gc_time_samples[index];
  _gc_time_sum_of_xy += _gc_time_xy[index];
  _gc_time_sum_of_xx += _gc_time_xx[index];

  if (_gc_time_num_samples < GC_TIME_SAMPLE_SIZE) {
    _gc_time_num_samples++;
  } else {
    _gc_time_first_sample_index = (_gc_time_first_sample_index + 1) % GC_TIME_SAMPLE_SIZE;
  }

  if (_gc_time_num_samples == 1) {
    // The predictor is constant (horizontal line)
    _gc_time_m = 0;
    _gc_time_b = gc_time;
    _gc_time_sd = 0.0;
  } else if (_gc_time_num_samples == 2) {
    // Two points define a line
    double delta_y = gc_time - _gc_time_samples[_gc_time_first_sample_index];
    double delta_x = timestamp - _gc_time_timestamps[_gc_time_first_sample_index];

    _gc_time_m = delta_y / delta_x;

    // y = mx + b
    // so b = y0 - mx0
    _gc_time_b = gc_time - _gc_time_m * timestamp;
    _gc_time_sd = 0.0;
  } else {
    _gc_time_m = ((_gc_time_num_samples * _gc_time_sum_of_xy - _gc_time_sum_of_timestamps * _gc_time_sum_of_samples) /
                  (_gc_time_num_samples * _gc_time_sum_of_xx - _gc_time_sum_of_timestamps * _gc_time_sum_of_timestamps));
    _gc_time_b = (_gc_time_sum_of_samples - _gc_time_m * _gc_time_sum_of_timestamps) / _gc_time_num_samples;
    double sum_of_squared_deviations = 0.0;
    for (size_t i = 0; i < _gc_time_num_samples; i++) {
      uint index = (_gc_time_first_sample_index + i) % GC_TIME_SAMPLE_SIZE;
      double x = _gc_time_timestamps[index];
      double predicted_y = _gc_time_m * x + _gc_time_b;
      double deviation = predicted_y - _gc_time_samples[index];
      sum_of_squared_deviations = deviation * deviation;
    }
    _gc_time_sd = sqrt(sum_of_squared_deviations / _gc_time_num_samples);
  }
}

double ShenandoahAdaptiveHeuristics::predict_gc_time(double timestamp_at_start) {
  double result = _gc_time_m * timestamp_at_start + _gc_time_b + _gc_time_sd * _margin_of_error_sd;;
  return result;
}

void ShenandoahAdaptiveHeuristics::add_rate_to_acceleration_history(double timestamp, double rate) {
  uint new_sample_index =
    (_spike_acceleration_first_sample_index + _spike_acceleration_num_samples) % SPIKE_ACCELERATION_SAMPLE_SIZE;
  _spike_acceleration_rate_timestamps[new_sample_index] = timestamp;
  _spike_acceleration_rate_samples[new_sample_index] = rate;
  if (_spike_acceleration_num_samples == SPIKE_ACCELERATION_SAMPLE_SIZE) {
    _spike_acceleration_first_sample_index++;
    if (_spike_acceleration_first_sample_index == SPIKE_ACCELERATION_SAMPLE_SIZE) {
      _spike_acceleration_first_sample_index = 0;
    }
  } else {
    _spike_acceleration_num_samples++;
  }
}

void ShenandoahAdaptiveHeuristics::record_success_concurrent(bool abbreviated) {
  ShenandoahHeuristics::record_success_concurrent(abbreviated);
  double now = os::elapsedTime();

  if (!abbreviated) {
    add_gc_time(_cycle_start, elapsed_cycle_time());
  }
  size_t available = _space_info->available();

  double z_score = 0.0;
  double available_sd = _available.sd();
  if (available_sd > 0) {
    double available_avg = _available.avg();
    z_score = (double(available) - available_avg) / available_sd;
    log_debug(gc, ergo)("%s Available: " SIZE_FORMAT " %sB, z-score=%.3f. Average available: %.1f %sB +/- %.1f %sB.",
                        _space_info->name(),
                        byte_size_in_proper_unit(available), proper_unit_for_byte_size(available),
                        z_score,
                        byte_size_in_proper_unit(available_avg), proper_unit_for_byte_size(available_avg),
                        byte_size_in_proper_unit(available_sd), proper_unit_for_byte_size(available_sd));
  }

  _available.add(double(available));

  // In the case when a concurrent GC cycle completes successfully but with an
  // unusually small amount of available memory we will adjust our trigger
  // parameters so that they are more likely to initiate a new cycle.
  // Conversely, when a GC cycle results in an above average amount of available
  // memory, we will adjust the trigger parameters to be less likely to initiate
  // a GC cycle.
  //
  // The z-score we have computed is in no way statistically related to the
  // trigger parameters, but it has the nice property that worse z-scores for
  // available memory indicate making larger adjustments to the trigger
  // parameters. It also results in fewer adjustments as the application
  // stabilizes.
  //
  // In order to avoid making endless and likely unnecessary adjustments to the
  // trigger parameters, the change in available memory (with respect to the
  // average) at the end of a cycle must be beyond these threshold values.
  if (z_score < LOWEST_EXPECTED_AVAILABLE_AT_END ||
      z_score > HIGHEST_EXPECTED_AVAILABLE_AT_END) {
    // The sign is flipped because a negative z-score indicates that the
    // available memory at the end of the cycle is below average. Positive
    // adjustments make the triggers more sensitive (i.e., more likely to fire).
    // The z-score also gives us a measure of just how far below normal. This
    // property allows us to adjust the trigger parameters proportionally.
    //
    // The `100` here is used to attenuate the size of our adjustments. This
    // number was chosen empirically. It also means the adjustments at the end of
    // a concurrent cycle are an order of magnitude smaller than the adjustments
    // made for a degenerated or full GC cycle (which themselves were also
    // chosen empirically).
    adjust_last_trigger_parameters(z_score / -100);
  }
}

void ShenandoahAdaptiveHeuristics::record_success_degenerated() {
  ShenandoahHeuristics::record_success_degenerated();

  add_degenerated_gc_time(_precursor_cycle_start, elapsed_degenerated_cycle_time());

  // Adjust both trigger's parameters in the case of a degenerated GC because
  // either of them should have triggered earlier to avoid this case.
  adjust_margin_of_error(DEGENERATE_PENALTY_SD);
  adjust_spike_threshold(DEGENERATE_PENALTY_SD);
}

void ShenandoahAdaptiveHeuristics::record_success_full() {
  this->ShenandoahHeuristics::record_success_full();

  // Adjust both trigger's parameters in the case of a full GC because
  // either of them should have triggered earlier to avoid this case.
  adjust_margin_of_error(FULL_PENALTY_SD);
  adjust_spike_threshold(FULL_PENALTY_SD);
}

static double saturate(double value, double min, double max) {
  return MAX2(MIN2(value, max), min);
}

#undef KELVIN_NEEDS_TO_SEE

void ShenandoahAdaptiveHeuristics::start_idle_span(size_t mutator_available) {
  size_t capacity = _space_info->soft_max_capacity();
  size_t total_allocations = _freeset->get_mutator_allocations();
  size_t spike_headroom = capacity / 100 * ShenandoahAllocSpikeFactor;
  size_t penalties      = capacity / 100 * _gc_time_penalties;

  // make headroom adjustments
  size_t headroom_adjustments = spike_headroom + penalties;

  if (mutator_available >= headroom_adjustments) {
    mutator_available -= headroom_adjustments;;
  } else {
    mutator_available = 0;
  }

  assert(!strcmp(_space_info->name(), "YOUNG"), "Assume young space");
  log_info(gc)("At start of idle gc span for %s, mutator available set to: " SIZE_FORMAT "%s"
               " after adjusting for spike_headroom: " SIZE_FORMAT "%s"
               " and penalties: " SIZE_FORMAT "%s", _space_info->name(),
               byte_size_in_proper_unit(mutator_available),   proper_unit_for_byte_size(mutator_available),
               byte_size_in_proper_unit(spike_headroom),      proper_unit_for_byte_size(spike_headroom),
               byte_size_in_proper_unit(penalties),           proper_unit_for_byte_size(penalties));

  _most_recent_headroom_at_start_of_idle = mutator_available;
  _allocation_cliff = total_allocations + mutator_available;
}

void ShenandoahAdaptiveHeuristics::adjust_penalty(intx step) {
  assert(0 <= _gc_time_penalties && _gc_time_penalties <= 100,
         "In range before adjustment: " INTX_FORMAT, _gc_time_penalties);

  intx new_val = _gc_time_penalties + step;

  // Do not penalize beyond what was within "our" power to manage.  The reason we degenerated may have been that
  // we were dealt a bad hand.  Excessive penalization will cause overly aggressive triggering of young, which
  // will result in starvation of old collections, resulting in inefficient utilization of memory.
  size_t capacity = _space_info->soft_max_capacity();
  size_t anticipated_capacity = _space_info->soft_max_capacity();
  size_t anticipated_penalties      = capacity / 100 * new_val;;
  size_t previous_penalties         = capacity / 100 * _gc_time_penalties;;
  if (anticipated_penalties - previous_penalties > _most_recent_headroom_at_start_of_idle) {
    size_t maximum_penalties = _most_recent_headroom_at_start_of_idle + previous_penalties;
    new_val = maximum_penalties * 100 / capacity;
  }
  if (new_val < 0) {
    new_val = 0;
  }
  if (new_val > 100) {
    new_val = 100;
  }

  _gc_time_penalties = new_val;

  assert(0 <= _gc_time_penalties && _gc_time_penalties <= 100,
         "In range after adjustment: " INTX_FORMAT, _gc_time_penalties);
}

bool ShenandoahAdaptiveHeuristics::should_start_gc() {

#ifdef KELVIN_DEPRECATE
  size_t allocation_headroom = available;
  size_t spike_headroom = capacity / 100 * ShenandoahAllocSpikeFactor;
  size_t penalties      = capacity / 100 * _gc_time_penalties;

  allocation_headroom -= MIN2(allocation_headroom, spike_headroom);
  allocation_headroom -= MIN2(allocation_headroom, penalties);

#else
  size_t total_allocations = _freeset->get_mutator_allocations();
  size_t allocated_since_last_sample = total_allocations - _previous_total_allocations;
  size_t allocated_since_idle = total_allocations - _total_allocations_at_start_of_idle;
  size_t allocatable = (total_allocations > _allocation_cliff)? 0: _allocation_cliff - total_allocations;
#endif

  // Track allocation rate even if we decide to start a cycle for other reasons.
  size_t capacity = _space_info->soft_max_capacity();
  size_t allocated = _space_info->bytes_allocated_since_gc_start();
  size_t available = _space_info->soft_available();

  log_debug(gc)("should_start_gc (%s)? available: " SIZE_FORMAT ", soft_max_capacity: " SIZE_FORMAT
                ", allocated: " SIZE_FORMAT,
                _space_info->name(), available, capacity, allocated);

  double rate = _allocation_rate.sample(allocated);
  double now =  _regulator_thread->get_most_recent_regulator_wake_time();
  double avg_cycle_time = _gc_cycle_time_history->davg() + (_margin_of_error_sd * _gc_cycle_time_history->dsd());
  double avg_alloc_rate = _allocation_rate.upper_bound(_margin_of_error_sd);

  double acceleration = 0.0;
  double current_alloc_rate = 0.0;
  double predicted_gc_time = predict_gc_time(now);
  double planned_gc_time;
  bool planned_gc_time_is_average;
  if (predicted_gc_time > avg_cycle_time) {
    planned_gc_time = predicted_gc_time;
    planned_gc_time_is_average = false;
  } else {
    planned_gc_time = avg_cycle_time;
    planned_gc_time_is_average = true;
  }

  double predicted_future_gc_time = predict_gc_time(now + _regulator_thread->planned_sleep_interval() / 1000.0);
  double future_planned_gc_time;
  bool future_planned_gc_time_is_average;
  if (predicted_future_gc_time > avg_cycle_time) {
    future_planned_gc_time = predicted_gc_time;
    future_planned_gc_time_is_average = false;
  } else {
    future_planned_gc_time = avg_cycle_time;
    future_planned_gc_time_is_average = true;
  }

  _last_trigger = OTHER;

  double instantaneous_rate = allocated_since_last_sample / (now - _previous_allocation_timestamp);
  _previous_total_allocations = total_allocations;
  _previous_allocation_timestamp = now;

  add_rate_to_acceleration_history(now, instantaneous_rate);
  size_t consumption_accelerated = accelerated_consumption(acceleration, current_alloc_rate, future_planned_gc_time);
  size_t min_threshold = min_free_threshold();
  if (available < min_threshold) {
    log_info(gc)("Trigger (%s): Free (" SIZE_FORMAT "%s) is below minimum threshold (" SIZE_FORMAT "%s)", _space_info->name(),
                 byte_size_in_proper_unit(available), proper_unit_for_byte_size(available),
                 byte_size_in_proper_unit(min_threshold), proper_unit_for_byte_size(min_threshold));
    return true;
  }

  // Check if we need to learn a bit about the application
  const size_t max_learn = ShenandoahLearningSteps;
  if (_gc_times_learned < max_learn) {
    size_t init_threshold = capacity / 100 * ShenandoahInitFreeThreshold;
    if (available < init_threshold) {
      log_info(gc)("Trigger (%s): Learning " SIZE_FORMAT " of " SIZE_FORMAT ". Free (" SIZE_FORMAT "%s) is below initial threshold (" SIZE_FORMAT "%s)",
                   _space_info->name(), _gc_times_learned + 1, max_learn,
                   byte_size_in_proper_unit(available), proper_unit_for_byte_size(available),
                   byte_size_in_proper_unit(init_threshold), proper_unit_for_byte_size(init_threshold));
      return true;
    }
  }

  //  Rationale:
  //    The idea is that there is an average allocation rate and there are occasional abnormal bursts (or spikes) of
  //    allocations that exceed the average allocation rate.  What do these spikes look like?
  //
  //    1. At certain phase changes, we may discard large amounts of data and replace it with large numbers of newly
  //       allocated objects.  This "spike" looks more like a phase change.  We were in steady state at M bytes/sec
  //       allocation rate and now we are in a "reinitialization phase" that looks like N bytes/sec.  We need the "spike"
  //       accommodation to give us enough runway to recalibrate our "average allocation rate".
  //
  //   2. The typical workload changes.  "Suddenly", our typical workload of N TPS increases to N+delta TPS.  This means
  //       our average allocation rate needs to be adjusted.  Once again, we need the "spike" accomodation to give us
  //       enough runway to recalibrate our "average allocation rate".
  //
  //    3. Though there is an "average" allocation rate, a given workload demand for allocation may be very bursty.  We
  //       allocate a bunch of LABs during the 5 ms that follow completion of a GC, then we perform no more allocations for
  //       the next 150 ms.  It seems we want the "spike" to represent the maximum divergence from average within the
  //       period of time between consecutive evaluation of the should_start_gc() service.  Here is the thinking:
  //
  //       a) Between now and the next time I ask whether should_start_gc(), we might experience a spike representing
  //          the anticipated burst of allocations.  If that would put us over budget, then we should start GC immediately.
  //       b) Between now and the anticipated depletion of allocation pool, there may be two or more bursts of allocations.
  //          If there are more than one of these bursts, we can "approximate" that these will be separated by spans of
  //          time with very little or no allocations so the "average" allocation rate should be a suitable approximation
  //          of how this will behave.
  //
  //    For cases 1 and 2, we need to "quickly" recalibrate the average allocation rate whenever we detect a change
  //    in operation mode.  We want some way to decide that the average rate has changed.  Make average allocation rate
  //    computations an independent effort.
  // Check if allocation headroom is still okay. This also factors in:
  //   1. Some space to absorb allocation spikes (ShenandoahAllocSpikeFactor)
  //   2. Accumulated penalties from Degenerated and Full GC

  log_debug(gc)("%s: average GC time: %.2f ms, predicted GC time: %.2f ms, allocation rate: %.0f %s/s",
                _space_info->name(), avg_cycle_time * 1000, predicted_gc_time * 1000,
                byte_size_in_proper_unit(avg_alloc_rate), proper_unit_for_byte_size(avg_alloc_rate));

  if (planned_gc_time > allocatable / avg_alloc_rate) {
    log_info(gc)("Trigger (%s): Planned %s GC time (%.2f ms) is above the time for average allocation rate (%.0f %sB/s)"
                 " to deplete free headroom (" SIZE_FORMAT "%s) (margin of error = %.2f)",
                 _space_info->name(), planned_gc_time_is_average? "(from average)": "(by linear prediction)",
                 planned_gc_time * 1000,
                 byte_size_in_proper_unit(avg_alloc_rate), proper_unit_for_byte_size(avg_alloc_rate),
                 byte_size_in_proper_unit(allocatable), proper_unit_for_byte_size(allocatable),
                 _margin_of_error_sd);
    _last_trigger = RATE;
    return true;
  }

  bool is_spiking = _allocation_rate.is_spiking(rate, _spike_threshold_sd);
  if (is_spiking && planned_gc_time > allocatable / rate) {
    log_info(gc)("Trigger (%s): Planned %s GC time (%.2f ms) is above the time for instantaneous allocation rate (%.0f %sB/s)"
                 " to deplete free headroom (" SIZE_FORMAT "%s) (spike threshold = %.2f)",
                 _space_info->name(), planned_gc_time_is_average? "(from average)": "(by linear prediction)",
                 planned_gc_time * 1000,
                 byte_size_in_proper_unit(rate), proper_unit_for_byte_size(rate),
                 byte_size_in_proper_unit(allocatable), proper_unit_for_byte_size(allocatable),
                 _spike_threshold_sd);
    _last_trigger = SPIKE;
    return true;
  }

  // Allocation rates may accelerate quickly during certain execution phase changes or due to unexpected growth in client demand
  // for a service.  While unbounded quadratic growth of consumption does not fully model this scenario, it is a much better
  // approximation than constant allocation rate within the domain of interest.
  //
  // The SPIKE trigger above is not robust against rapidly changing allocation rates.  We have observed situations
  // such as the following:
  //
  //    Sample Time (s)      Allocation Rate (MB/s)       Headroom (GB)
  //       101.807                       0.0                  26.93
  //       101.907                     477.6                  26.85
  //       102.007                   3,206.0                  26.35
  //       102.108                  23,797.8                  24.19   <--- accelerated spike triggers here
  //       102.208                  24,164.5                  21.83
  //       102.309                  23,965.0                  19.47
  //       102.409                  24,624.35                 17.05   <--- without accelerated spike detection, we trigger here
  //
  // The late trigger results in degenerated GC
  //
  // The domain of interest is the sampling interval (in this case 100 ms) plus the average gc cycle time (in this case 750 ms).
  // The question we can ask at time 102.108 is:
  //
  //    Assume allocation rate is accelerating at a constant rate.  If we postpone the spike trigger until the subsequent
  //    sample point, will there be enough memory to satisfy allocations that occur during the anticipated concurrent GC
  //    cycle?  If not, we should trigger right now.
  //
  // Outline of this heuristic triggering technique:
  //
  //  1. We remember the three most recent samples of spike allocation rate r0, r1, r2 samples at t0, t1, and t2
  //  2. if r1 < r0 or r2 < r1, approximate Acceleration = 0.0, Rate = Max(r0, r1, r2)
  //  3. Otherwise, use least squares method to compute best-fit line through rate vs time
  //  4. The slope of this line represents Acceleration. The y-intercept of this line represents "initial rate"
  //  5. Calculate modeled CurrentRate by substituting (t2 - t0) for t in the computed best-fit lint
  //  6. Use Consumption = CurrentRate * GCTime + 1/2 * Acceleration * GCTime * GCTime
  //     (See High School physics discussions on constant acceleration: D = v0 * t + 1/2 * a * t^2)
  //  7. if Consumption exceeds headroom, trigger now
  //
  // Though larger sample size would improve quality of predictor, it would delay our trigger response as well.

  if (consumption_accelerated > allocatable) {
    size_t size_t_acceleration = (size_t) acceleration;
    size_t size_t_alloc_rate = (size_t) current_alloc_rate;
    log_info(gc)("Trigger (%s): Accelerated consumption (" SIZE_FORMAT "%s) exceeds free headroom (" SIZE_FORMAT "%s) at "
                 "current rate (" SIZE_FORMAT "%s/s) with acceleration (" SIZE_FORMAT "%s/s/s) for planned %s GC time (%.2f ms)",
                 _space_info->name(),
                 byte_size_in_proper_unit(consumption_accelerated), proper_unit_for_byte_size(consumption_accelerated),
                 byte_size_in_proper_unit(allocatable), proper_unit_for_byte_size(allocatable),
                 byte_size_in_proper_unit(size_t_alloc_rate), proper_unit_for_byte_size(size_t_alloc_rate),
                 byte_size_in_proper_unit(size_t_acceleration), proper_unit_for_byte_size(size_t_acceleration),
                 future_planned_gc_time_is_average? "(from average)": "(by linear prediction)", future_planned_gc_time * 1000);
    _spike_acceleration_num_samples = 0;
    _spike_acceleration_first_sample_index = 0;

    // Count this as a form of RATE trigger for purposes of adjusting heuristic triggering configuration because this
    // trigger is influenced more by margin_of_error_sd than by spike_threshold_sd.
    _last_trigger = RATE;
    return true;
  }

  if (ShenandoahHeuristics::should_start_gc()) {
    _spike_acceleration_num_samples = 0;
    _spike_acceleration_first_sample_index = 0;
    return true;
  } else {
    return false;
  }
}

void ShenandoahAdaptiveHeuristics::adjust_last_trigger_parameters(double amount) {
  switch (_last_trigger) {
    case RATE:
      adjust_margin_of_error(amount);
      break;
    case SPIKE:
      adjust_spike_threshold(amount);
      break;
    case OTHER:
      // nothing to adjust here.
      break;
    default:
      ShouldNotReachHere();
  }
}

// This is only called if a new rate sample has been gathered (e.g. ten times per second).
// There is no adjustment for standard deviation of the accelerated rate prediction.
size_t ShenandoahAdaptiveHeuristics::accelerated_consumption(double& acceleration, double& current_rate,
                                                             double predicted_cycle_time) const
{
  double *x_array = (double *) alloca(SPIKE_ACCELERATION_SAMPLE_SIZE * sizeof(double));
  double *y_array = (double *) alloca(SPIKE_ACCELERATION_SAMPLE_SIZE * sizeof(double));
  double x_sum = 0.0;
  double y_sum = 0.0;
  double y_avg;

  assert(_spike_acceleration_num_samples > 0, "At minimum, we should have sample from this period");

  size_t count_zeroes = 0;
  bool non_zero_decreases = false;
  double largest_rate_seen = 0.0;
  for (uint i = 0; i < _spike_acceleration_num_samples; i++) {
    uint index = (_spike_acceleration_first_sample_index + i) % SPIKE_ACCELERATION_SAMPLE_SIZE;
    x_array[i] = _spike_acceleration_rate_timestamps[index];
    x_sum += x_array[i];
    y_array[i] = _spike_acceleration_rate_samples[index];
    if (y_array[i] == 0) {
      count_zeroes++;
    } else {
      if (y_array[i] < largest_rate_seen) {
        non_zero_decreases = true;
      } else {
        largest_rate_seen = y_array[i];
      }
      y_sum += y_array[i];
    }
  }

  if (_spike_acceleration_num_samples >= SPIKE_DETECTION_SAMPLE_SIZE) {
    double sum_for_average = 0.0;
    for (uint i = _spike_acceleration_num_samples - SPIKE_DETECTION_SAMPLE_SIZE; i < _spike_acceleration_num_samples; i++) {
      sum_for_average += y_array[i];
    }
    // Note that y_avg is approximate, because it is not weighted for reality that some samples span more time than others.
    // Unless demonstrated to the contrary, assume this approximation is good enought.
    y_avg = sum_for_average / SPIKE_DETECTION_SAMPLE_SIZE;
  } else {
    y_avg = 0.0;
  }

  // By default, use y_avg for current rate and zero acceleration. Overwrite iff best-fit line has positive slope.
  current_rate = y_avg;
  acceleration = 0.0;

  if (_spike_acceleration_num_samples >= SPIKE_ACCELERATION_SAMPLE_SIZE) {

    // It is sometimes difficult to distinguish between "random" noise and a meaningful acceleration trend.
    //
    // For this reason, we disqualify the immediate acceleration trigger if:
    //
    //  1. the number of zeroes is > 3 (more than half of sample size 6), or if
    //
    //  2. the number of zeroes is 3, and the non-zero entries are not strictly increasing
    //
    // Otherwise, we end up with way too many unproductive acceleration triggers.  Samples that are
    // disqualified in this invocation of should_start_gc() may be combined with additional samples
    // gathered for a subsequent should_start_gc() evaluation that is not disqualified.

    double *xy_array = (double *) alloca(SPIKE_ACCELERATION_SAMPLE_SIZE * sizeof(double));
    double *x2_array = (double *) alloca(SPIKE_ACCELERATION_SAMPLE_SIZE * sizeof(double));
    double xy_sum = 0.0;
    double x2_sum = 0.0;
    for (uint i = 0; i < SPIKE_ACCELERATION_SAMPLE_SIZE; i++) {
      xy_array[i] = x_array[i] * y_array[i];
      xy_sum += xy_array[i];
      x2_array[i] = x_array[i] * x_array[i];
      x2_sum += x2_array[i];
    }
    // Find the best-fit least-squares linear representation of rate vs time
    double m;                 /* slope */
    double b;                 /* y-intercept */

    m = (SPIKE_ACCELERATION_SAMPLE_SIZE * xy_sum - x_sum * y_sum) / (SPIKE_ACCELERATION_SAMPLE_SIZE * x2_sum - x_sum * x_sum);
    b = (y_sum - m * x_sum) / SPIKE_ACCELERATION_SAMPLE_SIZE;

    if (m > 0) {
      double proposed_current_rate = m * x_array[SPIKE_ACCELERATION_SAMPLE_SIZE - 1] + b;

#ifdef KELVIN_NEEDS_TO_SEE
      log_info(gc)("Calculating acceleration to be %.3f, with current rate: %.3f", m, proposed_current_rate);
#endif
      // Measure goodness of fit
      double sum_of_squared_differences = 0;
      for (size_t i = 0; i < SPIKE_ACCELERATION_SAMPLE_SIZE; i++) {
        double t = x_array[i];
        double measured = y_array[i];
        double predicted = m * t + b;
        double delta = predicted - measured;
        sum_of_squared_differences += delta * delta;
#ifdef KELVIN_NEEDS_TO_SEE
        log_info(gc)("@ time: %.3f, rate: %.3f, predicted rate: %.3f", t, measured, predicted);
#endif
      }
      // This representation of goodness is not exactly standard deviation or chi-square value, but is similar.
      const double MAXIMUM_GOODNESS_DEVIATION = 0.15;

      double goodness = sqrt(sum_of_squared_differences / SPIKE_ACCELERATION_SAMPLE_SIZE);
      assert(goodness / proposed_current_rate > 0, "proposed_current_rate should be positive because m is positive");
      bool is_good_predictor = (goodness / proposed_current_rate) <= MAXIMUM_GOODNESS_DEVIATION;;
#ifdef KELVIN_NEEDS_TO_SEE
      log_info(gc)("goodness is: %.3f which is %s predictor because ratio is %.1f out of proposed rate: %.3f",
                   goodness, is_good_predictor? "good": "not good", (goodness / proposed_current_rate), proposed_current_rate);
#endif

      if (is_good_predictor) {
        acceleration = m;
        current_rate = proposed_current_rate;
      }
      // else, leave current_rate = y_max, acceleration = 0
    }
    // else, leave current_rate = y_max, acceleration = 0
  }
  // and here also, leave current_rate = y_max, acceleration = 0

  double time_delta = _regulator_thread->planned_sleep_interval() / 1000.0 + predicted_cycle_time;
  size_t bytes_to_be_consumed = (size_t) (current_rate * time_delta + 0.5 * acceleration * time_delta * time_delta);
  return bytes_to_be_consumed;
}

void ShenandoahAdaptiveHeuristics::adjust_margin_of_error(double amount) {
  _margin_of_error_sd = saturate(_margin_of_error_sd + amount, MINIMUM_CONFIDENCE, MAXIMUM_CONFIDENCE);
  log_debug(gc, ergo)("Margin of error now %.2f", _margin_of_error_sd);
}

void ShenandoahAdaptiveHeuristics::adjust_spike_threshold(double amount) {
  _spike_threshold_sd = saturate(_spike_threshold_sd - amount, MINIMUM_CONFIDENCE, MAXIMUM_CONFIDENCE);
  log_debug(gc, ergo)("Spike threshold now: %.2f", _spike_threshold_sd);
}

size_t ShenandoahAdaptiveHeuristics::min_free_threshold() {
  // Note that soft_max_capacity() / 100 * min_free_threshold is smaller than max_capacity() / 100 * min_free_threshold.
  // We want to behave conservatively here, so use max_capacity().  By returning a larger value, we cause the GC to
  // trigger when the remaining amount of free shrinks below the larger threshold.
  return _space_info->max_capacity() / 100 * ShenandoahMinFreeThreshold;
}

ShenandoahAllocationRate::ShenandoahAllocationRate() :
  _last_sample_time(os::elapsedTime()),
  _last_sample_value(0),
  _interval_sec(1.0 / ShenandoahAdaptiveSampleFrequencyHz),
  _rate(int(ShenandoahAdaptiveSampleSizeSeconds * ShenandoahAdaptiveSampleFrequencyHz), ShenandoahAdaptiveDecayFactor),
  _rate_avg(int(ShenandoahAdaptiveSampleSizeSeconds * ShenandoahAdaptiveSampleFrequencyHz), ShenandoahAdaptiveDecayFactor) {
}

double ShenandoahAllocationRate::sample(size_t allocated) {
  double now = os::elapsedTime();
  double rate = 0.0;
  if (now - _last_sample_time > _interval_sec) {
    if (allocated >= _last_sample_value) {
      rate = instantaneous_rate(now, allocated);
      _rate.add(rate);
      _rate_avg.add(_rate.avg());
    }

    _last_sample_time = now;
    _last_sample_value = allocated;
  }
  return rate;
}

double ShenandoahAllocationRate::upper_bound(double sds) const {
  // Here we are using the standard deviation of the computed running
  // average, rather than the standard deviation of the samples that went
  // into the moving average. This is a much more stable value and is tied
  // to the actual statistic in use (moving average over samples of averages).
  return _rate.davg() + (sds * _rate_avg.dsd());
}

void ShenandoahAllocationRate::allocation_counter_reset() {
  _last_sample_time = os::elapsedTime();
  _last_sample_value = 0;
}

bool ShenandoahAllocationRate::is_spiking(double rate, double threshold) const {
  if (rate <= 0.0) {
    return false;
  }

  double sd = _rate.sd();
  if (sd > 0) {
    // There is a small chance that that rate has already been sampled, but it seems not to matter in practice.
    // z_score reports how close this measure is to the average.  A value between -1 and 1 means we are within 1
    // standard deviation.  A value between -3 and +3 means we are within 3 standard deviations.  We care only if
    //  the spike is above the mean.
    double z_score = (rate - _rate.avg()) / sd;
    if (z_score > threshold) {
      return true;
    }
  }
  return false;
}

double ShenandoahAllocationRate::instantaneous_rate(double time, size_t allocated) const {
  size_t last_value = _last_sample_value;
  double last_time = _last_sample_time;
  size_t allocation_delta = (allocated > last_value) ? (allocated - last_value) : 0;
  double time_delta_sec = time - last_time;
  return (time_delta_sec > 0)  ? (allocation_delta / time_delta_sec) : 0;
}

