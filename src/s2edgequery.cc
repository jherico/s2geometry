// Copyright 2013 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Author: ericv@google.com (Eric Veach)

#include "s2edgequery.h"

#include <algorithm>
#include <utility>

#include <glog/logging.h>
#include "r1interval.h"
#include "r2rect.h"
#include "s2cellid.h"
#include "s2edgeutil.h"
#include "s2paddedcell.h"

using std::vector;

bool S2EdgeQuery::GetCandidates(S2Point const& a, S2Point const& b,
                                S2Shape const* shape, vector<int>* edges) {
  // For small loops it is faster to use brute force.  The threshold below was
  // determined using the benchmarks in the unit test.
  static int const kMaxBruteForceEdges = 27;
  edges->clear();
  int max_edges = shape->num_edges();
  if (max_edges <= kMaxBruteForceEdges) {
    edges->reserve(max_edges);
    for (int i = 0; i < max_edges; ++i) edges->push_back(i);
    return true;
  }
  // Compute the set of index cells intersected by the query edge.
  GetCells(a, b);
  if (cells_.empty()) return false;

  // Gather all the edges that intersect those cells and sort them.
  int shape_id = shape->id();
  for (int i = 0; i < cells_.size(); ++i) {
    S2ShapeIndexCell const* cell = cells_[i];
    S2ClippedShape const* clipped = cell->find_clipped(shape_id);
    if (clipped == NULL) continue;
    edges->reserve(edges->size() + clipped->num_edges());
    for (int j = 0; j < clipped->num_edges(); ++j) {
      edges->push_back(clipped->edge(j));
    }
  }
  if (cells_.size() > 1) {
    std::sort(edges->begin(), edges->end());
    edges->erase(std::unique(edges->begin(), edges->end()), edges->end());
  }
  return !edges->empty();
}

bool S2EdgeQuery::GetCandidates(S2Point const& a, S2Point const& b,
                                EdgeMap* edge_map) {
  // If there are only a few edges then it's faster to use brute force.  We
  // only bother with this optimization when there is a single shape, since
  // then we can also use some tricks to avoid reallocating the EdgeMap.
  if (index_->num_shape_ids() == 1) {
    // Typically this method is called many times, so it is worth checking
    // whether the EdgeMap is empty or already consists of a single entry for
    // this shape, and skip clearing "edge_map" in that case.
    S2Shape const* shape = index_->shape(0);
    vector<int>* edges = &(*edge_map)[shape];
    if (edge_map->size() != 1) {
      // "edge_map" must have been used to query some other S2ShapeIndex, so
      // we need to clear its current contents.
      edge_map->clear();
      edges = &(*edge_map)[shape];
    }
    // Note that we leave "edge_map" non-empty even if there are no candidates
    // (i.e., there is a single entry with an empty set of edges).  This is an
    // advantage for efficiency since it avoids memory reallocation.
    return GetCandidates(a, b, shape, edges);
  }
  // Compute the set of index cells intersected by the query edge.
  GetCells(a, b);
  edge_map->clear();
  if (cells_.empty()) return false;

  // Gather all the edges that intersect those cells and sort them.
  for (int i = 0; i < cells_.size(); ++i) {
    S2ShapeIndexCell const* cell = cells_[i];
    for (int s = 0; s < cell->num_shapes(); ++s) {
      S2ClippedShape const& clipped = cell->clipped(s);
      vector<int>* edges = &(*edge_map)[index_->shape(clipped.shape_id())];
      edges->reserve(edges->size() + clipped.num_edges());
      for (int j = 0; j < clipped.num_edges(); ++j) {
        edges->push_back(clipped.edge(j));
      }
    }
  }
  if (cells_.size() > 1) {
    for (EdgeMap::iterator i = edge_map->begin(); i != edge_map->end(); ++i) {
      vector<int>* edges = &i->second;
      std::sort(edges->begin(), edges->end());
      edges->erase(std::unique(edges->begin(), edges->end()), edges->end());
    }
  }
  return !edge_map->empty();
}

// Set cells_ to the set of index cells intersected by an edge AB.
void S2EdgeQuery::GetCells(S2Point const& a, S2Point const& b) {
  cells_.clear();
  S2EdgeUtil::FaceSegmentVector segments;
  S2EdgeUtil::GetFaceSegments(a, b, &segments);
  for (int i = 0; i < segments.size(); ++i) {
    a_ = segments[i].a;
    b_ = segments[i].b;

    // Optimization: rather than always starting the recursive subdivision at
    // the top level face cell, instead we start at the smallest S2CellId that
    // contains the edge (the "edge root cell").  This typically lets us skip
    // quite a few levels of recursion since most edges are short.
    R2Rect edge_bound = R2Rect::FromPointPair(a_, b_);
    S2PaddedCell pcell(S2CellId::FromFace(segments[i].face), 0);
    S2CellId edge_root = pcell.ShrinkToFit(edge_bound);

    // Now we need to determine how the edge root cell is related to the cells
    // in the spatial index (cell_map_).  There are three cases:
    //
    //  1. edge_root is an index cell or is contained within an index cell.
    //     In this case we only need to look at the contents of that cell.
    //  2. edge_root is subdivided into one or more index cells.  In this case
    //     we recursively subdivide to find the cells intersected by AB.
    //  3. edge_root does not intersect any index cells.  In this case there
    //     is nothing to do.
    S2ShapeIndex::CellRelation relation = iter_.Locate(edge_root);
    if (relation == S2ShapeIndex::INDEXED) {
      // edge_root is an index cell or is contained by an index cell (case 1).
      DCHECK(iter_.id().contains(edge_root));
      cells_.push_back(iter_.cell());
    } else if (relation == S2ShapeIndex::SUBDIVIDED) {
      // edge_root is subdivided into one or more index cells (case 2).  We
      // find the cells intersected by AB using recursive subdivision.
      if (!edge_root.is_face()) pcell = S2PaddedCell(edge_root, 0);
      GetCells(pcell, edge_bound);
    }
  }
}

bool S2EdgeQuery::GetCells(S2Point const& a, S2Point const& b,
                           S2PaddedCell const& root,
                           vector<S2ShapeIndexCell const*>* cells) {
  cells_.clear();
  if (S2EdgeUtil::ClipToFace(a, b, root.id().face(), &a_, &b_)) {
    R2Rect edge_bound = R2Rect::FromPointPair(a_, b_);
    if (root.bound().Intersects(edge_bound)) {
      GetCells(root, edge_bound);
    }
  }
  if (cells_.empty()) return false;
  cells->assign(cells_.begin(), cells_.end());
  return true;
}

// Compute the index cells intersected by the current edge that are
// descendants of "pcell" and add them to cells_.
//
// WARNING: This function is recursive with a maximum depth of 30.  The frame
// size is about 2K in versions of GCC prior to 4.7 due to poor overlapping
// of storage for temporaries.  This is fixed in GCC 4.7, reducing the frame
// size to about 350 bytes (i.e., worst-case total stack usage of about 10K).
void S2EdgeQuery::GetCells(S2PaddedCell const& pcell,
                           R2Rect const& edge_bound) {
  iter_.Seek(pcell.id().range_min());
  if (iter_.Done() || iter_.id() > pcell.id().range_max()) {
    // The index does not contain "pcell" or any of its descendants.
    return;
  }
  if (iter_.id() == pcell.id()) {
    // The index contains this cell exactly.
    cells_.push_back(iter_.cell());
    return;
  }

  // Otherwise, split the edge among the four children of "pcell".
  R2Point center = pcell.middle().lo();
  if (edge_bound[0].hi() < center[0]) {
    // Edge is entirely contained in the two left children.
    ClipVAxis(edge_bound, center[1], 0, pcell);
  } else if (edge_bound[0].lo() >= center[0]) {
    // Edge is entirely contained in the two right children.
    ClipVAxis(edge_bound, center[1], 1, pcell);
  } else {
    R2Rect child_bounds[2];
    SplitUBound(edge_bound, center[0], child_bounds);
    if (edge_bound[1].hi() < center[1]) {
      // Edge is entirely contained in the two lower children.
      GetCells(S2PaddedCell(pcell, 0, 0), child_bounds[0]);
      GetCells(S2PaddedCell(pcell, 1, 0), child_bounds[1]);
    } else if (edge_bound[1].lo() >= center[1]) {
      // Edge is entirely contained in the two upper children.
      GetCells(S2PaddedCell(pcell, 0, 1), child_bounds[0]);
      GetCells(S2PaddedCell(pcell, 1, 1), child_bounds[1]);
    } else {
      // The edge bound spans all four children.  The edge itself intersects
      // at most three children (since no padding is being used).
      ClipVAxis(child_bounds[0], center[1], 0, pcell);
      ClipVAxis(child_bounds[1], center[1], 1, pcell);
    }
  }
}

// Given either the left (i=0) or right (i=1) side of a padded cell "pcell",
// determine whether the current edge intersects the lower child, upper child,
// or both children, and call GetCells() recursively on those children.
// "center" is the v-coordinate at the center of "pcell".
inline void S2EdgeQuery::ClipVAxis(R2Rect const& edge_bound, double center,
                                   int i, S2PaddedCell const& pcell) {
  if (edge_bound[1].hi() < center) {
    // Edge is entirely contained in the lower child.
    GetCells(S2PaddedCell(pcell, i, 0), edge_bound);
  } else if (edge_bound[1].lo() >= center) {
    // Edge is entirely contained in the upper child.
    GetCells(S2PaddedCell(pcell, i, 1), edge_bound);
  } else {
    // The edge intersects both children.
    R2Rect child_bounds[2];
    SplitVBound(edge_bound, center, child_bounds);
    GetCells(S2PaddedCell(pcell, i, 0), child_bounds[0]);
    GetCells(S2PaddedCell(pcell, i, 1), child_bounds[1]);
  }
}

// Given an edge and an interval "middle" along the v-axis, clip the edge
// against the boundaries of "middle" and insert the edge into the
// corresponding children.
// Given an edge, clip the given endpoint (lo=0, hi=1) of the u-axis so that
// it does not extend past the given value.
// Given an edge and two bound endpoints that need to be updated, allocate and
// return a new edge with the updated bound.

// Split the current edge into two child edges at the given u-value "u" and
// return the bound for each child.
void S2EdgeQuery::SplitUBound(R2Rect const& edge_bound, double u,
                              R2Rect child_bounds[2]) const {
  // See comments in S2ShapeIndex::ClipUBound.
  double v = edge_bound[1].ClampPoint(
      S2EdgeUtil::InterpolateDouble(u, a_[0], b_[0], a_[1], b_[1]));

  // "diag_" indicates which diagonal of the bounding box is spanned by AB:
  // it is 0 if AB has positive slope, and 1 if AB has negative slope.
  int diag = (a_[0] > b_[0]) != (a_[1] > b_[1]);
  SplitBound(edge_bound, 0, u, diag, v, child_bounds);
}

// Split the current edge into two child edges at the given v-value "v" and
// return the bound for each child.
void S2EdgeQuery::SplitVBound(R2Rect const& edge_bound, double v,
                              R2Rect child_bounds[2]) const {
  double u = edge_bound[0].ClampPoint(
      S2EdgeUtil::InterpolateDouble(v, a_[1], b_[1], a_[0], b_[0]));
  int diag = (a_[0] > b_[0]) != (a_[1] > b_[1]);
  SplitBound(edge_bound, diag, u, 0, v, child_bounds);
}

// Split the current edge into two child edges at the given point (u,v) and
// return the bound for each child.  "u_end" and "v_end" indicate which bound
// endpoints of child 1 will be updated.
inline void S2EdgeQuery::SplitBound(R2Rect const& edge_bound,
                                    int u_end, double u, int v_end, double v,
                                    R2Rect child_bounds[2]) {
  child_bounds[0] = edge_bound;
  child_bounds[0][0][1-u_end] = u;
  child_bounds[0][1][1-v_end] = v;
  DCHECK(!child_bounds[0].is_empty());
  DCHECK(edge_bound.Contains(child_bounds[0]));

  child_bounds[1] = edge_bound;
  child_bounds[1][0][u_end] = u;
  child_bounds[1][1][v_end] = v;
  DCHECK(!child_bounds[1].is_empty());
  DCHECK(edge_bound.Contains(child_bounds[1]));
}
