/*
Copyright 2023 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "sweeper.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "absl/container/btree_set.h"
#include "minimalloc.h"

namespace minimalloc {

namespace {

enum PointType { kRightGap, kRight, kLeft, kLeftGap };

struct Point {
  BufferIdx buffer_idx;
  TimeValue time_value;
  PointType point_type;
  std::optional<Window> window;
  bool operator<(const Point& x) const {
    // First, order by time, then direction (right vs. left), then buffer idx.
    if (time_value != x.time_value) return time_value < x.time_value;
    if (point_type != x.point_type) return point_type < x.point_type;
    return buffer_idx < x.buffer_idx;
  }
};

}  // namespace

bool SectionSpan::operator==(const SectionSpan& x) const {
  return section_range == x.section_range && window == x.window;
}

bool Partition::operator==(const Partition& x) const {
  return buffer_idxs == x.buffer_idxs &&
         section_range == x.section_range;
}

bool Overlap::operator==(const Overlap& x) const {
  return buffer_idx == x.buffer_idx && effective_size == x.effective_size;
}

bool Overlap::operator<(const Overlap& x) const {
  if (buffer_idx != x.buffer_idx) return buffer_idx < x.buffer_idx;
  return effective_size < x.effective_size;
}

bool BufferData::operator==(const BufferData& x) const {
  return section_spans == x.section_spans &&
         overlaps == x.overlaps;
}

bool SweepResult::operator==(const SweepResult& x) const {
  return sections == x.sections &&
         partitions == x.partitions &&
         buffer_data == x.buffer_data;
}

// For a given problem, places all buffer start times into a priority queue.
// Then, maintains an "active" set of buffers to determine disjoint partitions.
// For each partition, records the list of buffers + pairwise overlaps + unique
// cross sections.
SweepResult Sweep(const Problem& problem) {
  SweepResult result;
  std::vector<Point> points;
  const auto num_buffers = problem.buffers.size();
  points.reserve(num_buffers * 2);  // Reserve 2 spots per buffer.
  for (BufferIdx buffer_idx = 0; buffer_idx < num_buffers; ++buffer_idx) {
    const Buffer& buffer = problem.buffers[buffer_idx];
    points.push_back({.buffer_idx = buffer_idx,
                      .time_value = buffer.lifespan.lower(),
                      .point_type = PointType::kLeft});
    for (const Gap& gap : buffer.gaps) {
      points.push_back({.buffer_idx = buffer_idx,
                        .time_value = gap.lifespan.lower(),
                        .point_type = PointType::kRightGap,
                        .window = gap.window});
      points.push_back({.buffer_idx = buffer_idx,
                        .time_value = gap.lifespan.upper(),
                        .point_type = PointType::kLeftGap,
                        .window = gap.window});
    }
    points.push_back({.buffer_idx = buffer_idx,
                      .time_value = buffer.lifespan.upper(),
                      .point_type = PointType::kRight});
  }
  std::sort(points.begin(), points.end());
  Section actives, alive;
  TimeValue last_section_time = -1;
  SectionIdx last_section_idx = 0;
  // Create a reverse index (from buffers to sections) for quick lookup.
  result.buffer_data.resize(num_buffers);
  std::vector<SectionIdx> buffer_idx_to_section_start(num_buffers, -1);
  std::vector<Window> buffer_idx_to_window(num_buffers);
  for (auto buffer_idx = 0; buffer_idx < problem.buffers.size(); ++buffer_idx) {
    buffer_idx_to_window[buffer_idx] = {0, problem.buffers[buffer_idx].size};
  }
  for (const Point& point : points) {
    const BufferIdx buffer_idx = point.buffer_idx;
    const TimeValue time_value = point.time_value;
    const PointType point_type = point.point_type;
    const std::optional<Window> window = point.window;
    const Buffer& buffer = problem.buffers[buffer_idx];
    const bool isLeft = point_type == kLeft;
    const bool isRight = point_type == kRight;
    const bool isEmptyLeftGap = point_type == kLeftGap && !window;
    const bool isEmptyRightGap = point_type == kRightGap && !window;
    const bool isWindowedLeftGap = point_type == kLeftGap && window;
    const bool isWindowedRightGap = point_type == kRightGap && window;
    if (last_section_time == -1) last_section_time = time_value;
    // If it's a right endpoint, remove it from the set of active buffers.
    if (isWindowedRightGap) {
      buffer_idx_to_window[buffer_idx] = {0, buffer.size};
    }
    if (isRight || isEmptyRightGap || isWindowedRightGap || isWindowedLeftGap) {
      // Create a new cross section of buffers if one doesn't yet exist.
      if (last_section_time < time_value) {
        last_section_time = time_value;
        result.sections.push_back(actives);
      }
    }
    if (isRight || isEmptyRightGap) {
      actives.erase(buffer_idx);
    }
    if (isRight) {
      alive.erase(buffer_idx);
    }
    if (isRight || isEmptyRightGap || isWindowedRightGap || isWindowedLeftGap) {
      if (buffer_idx_to_section_start[buffer_idx] != -1) {
        const SectionRange section_range =
            {buffer_idx_to_section_start[buffer_idx],
             (int)result.sections.size()};
        const SectionSpan section_span =
            {.section_range = section_range,
             .window = buffer_idx_to_window[buffer_idx]};
        result.buffer_data[buffer_idx].section_spans.push_back(section_span);
        // If the alives are empty, the span of this partition is now known.
        if (alive.empty()) {
          result.partitions.back().section_range =
              {last_section_idx, (int)result.sections.size()};
          last_section_idx = result.sections.size();
        }
        buffer_idx_to_section_start[buffer_idx] = -1;
      }
    }
    if (isWindowedLeftGap) {
      buffer_idx_to_window[buffer_idx] = *window;
    }
    if (isLeft || isEmptyLeftGap) {
      // If it's a left endpoint, check if a new partition should be established
      if (alive.empty()) result.partitions.push_back(Partition());
    }
    if (isLeft) {
      // Record any overlaps, and then add this buffer to the set of actives.
      result.partitions.back().buffer_idxs.push_back(buffer_idx);
    }
    if (isLeft || isEmptyLeftGap) {
      for (auto active_idx : actives) {
        const Buffer& active = problem.buffers[active_idx];
        auto active_effective_size = active.effective_size(buffer);
        if (active_effective_size) {
          result.buffer_data[active_idx].overlaps.insert(
              {buffer_idx, *active_effective_size});
        }
        auto effective_size = buffer.effective_size(active);
        if (effective_size) {
          result.buffer_data[buffer_idx].overlaps.insert({active_idx,
                                                          *effective_size});
        }
      }
    }
    if (isLeft || isEmptyLeftGap) {
      actives.insert(buffer_idx);
    }
    if (isLeft) {
      // Mutants OK for following line; performance tweak to prevent reinsertion
      alive.insert(buffer_idx);
    }
    if (isLeft || isEmptyLeftGap || isWindowedLeftGap || isWindowedRightGap) {
      buffer_idx_to_section_start[buffer_idx] = result.sections.size();
    }
  }
  return result;
}

// Calculates the number of "cuts," i.e., buffers that are active between
// adjacent sections.  If there are zero cuts between sections i and i+1, it
// implies that the sections {..., i-2, i-1, i} and {i+1, i+2, ...} may be
// solved separately.
std::vector<CutCount> SweepResult::CalculateCuts() const {
  std::vector<CutCount> cuts(sections.size() - 1);
  for (const BufferData& buffer_data : buffer_data) {
    const std::vector<SectionSpan>& section_spans = buffer_data.section_spans;
    for (SectionIdx s_idx = section_spans.front().section_range.lower();
        s_idx + 1 < section_spans.back().section_range.upper(); ++s_idx) {
      ++cuts[s_idx];
    }
  }
  return cuts;
}

}  // namespace minimalloc
