/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/
#pragma once

#include "mcts/node.h"
#include "neural/network.h"
#include "utils/cache.h"
#include "utils/pfloat16.h"
#include "utils/smallarray.h"

namespace lczero {

struct CachedNNRequest {
  CachedNNRequest(size_t size = 0) : p(size) {}
  float q;
  float d;
  float m;
  // Store p only for valid moves.
  std::vector<pfloat16> p;
};

typedef HashKeyedCache<CachedNNRequest> NNCache;
typedef HashKeyedCacheLock<CachedNNRequest> NNCacheLock;

// Wraps around NetworkComputation and caches result.
// While it mostly repeats NetworkComputation interface, it's not derived
// from it, as AddInput() needs hash and index of probabilities to store.
class CachingComputation {
 public:
  CachingComputation(std::unique_ptr<NetworkComputation> parent,
                     pblczero::NetworkFormat::InputFormat input_format,
                     lczero::FillEmptyHistory history_fill, float softmax_temp,
                     int history_length, NNCache* cache);

  // How many inputs are not found in cache and will be forwarded to a wrapped
  // computation.
  int GetCacheMisses() const;
  // Total number of times AddInput/AddInputByHash were (successfully) called.
  int GetBatchSize() const;
  // Check if entry is in the cache.
  bool CacheLookup(const PositionHistory& history, const MoveList& moves = {},
                   CachedNNRequest* entry = nullptr);
  // Adds a sample to the batch. Also calls EncodePositionForNN() if needed.
  // @hash is a hash to store/lookup it in the cache.
  void AddInput(const PositionHistory& history, const MoveList& moves);
  // Undos last AddInput. If it was a cache miss, the it's actually not removed
  // from parent's batch.
  void PopLastInputHit();
  // Do the computation.
  void ComputeBlocking();
  // Returns Q value of @sample.
  float GetQVal(int sample) const;
  // Returns probability of draw if NN has WDL value head.
  float GetDVal(int sample) const;
  // Returns estimated remaining moves.
  float GetMVal(int sample) const;
  // Returns compressed P value @move_id of @sample.
  pfloat16 GetPVal(int sample, int move_ct) const;
  // Pops last input from the computation. Only allowed for inputs which were
  // cached.
  void PopCacheHit();

  // Can be used to avoid repeated reallocations internally while adding itemms.
  void Reserve(int batch_size) { batch_.reserve(batch_size); }

 private:
  struct WorkItem {
    uint64_t hash;
    NNCacheLock lock;
    int idx_in_parent = -1;
    // Initially the move indices, after computation the policy values.
    std::vector<uint16_t> probabilities_to_cache;
  };

  std::unique_ptr<NetworkComputation> parent_;
  pblczero::NetworkFormat::InputFormat input_format_;
  lczero::FillEmptyHistory history_fill_;
  float softmax_temp_;
  int history_length_;
  NNCache* cache_;
  std::vector<WorkItem> batch_;
};

}  // namespace lczero
