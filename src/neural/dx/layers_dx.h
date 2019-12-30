/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2019 The LCZero Authors

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
*/

#include <dxgi.h>
#include "dx_common.h"
#include "shader_wrapper.h"

namespace lczero {
class DxContext;

namespace dx_backend {
constexpr int kMaxSupportedBatchSize = 256;

// The Layer objects only hold memory for weights, biases, etc
// memory for input and output tensors is provided by caller of Eval.

class BaseLayer {
 public:
  int GetC() const { return C; }
  int GetH() const { return H; }
  int GetW() const { return W; }

  BaseLayer(int c, int h, int w, BaseLayer* ip, DxContext* pContext, bool fp16);
  virtual ~BaseLayer() = default;
  size_t GetOutputSize(int N) const {
    return (fp16_ ? sizeof(dx_half) : sizeof(float)) * N * C * H * W;
  }

  // input2 is optional (skip connection).
  virtual void Eval(int N, DXAlloc output, DXAlloc input, DXAlloc input2,
                    DXAlloc scratch, DXAlloc scratc2,
                    ID3D12GraphicsCommandList5* pCL) = 0;

 protected:
  BaseLayer* input_;
  DxContext* dx_context_;

  bool fp16_;

  // Output tensor dimensions.
  int C;  
  int H;
  int W;
};

// Holds Metacommand objects and their scratch space for all allowed batch sizes.
class GemmMetaCommand {
 private:
  // Need to create a Metacommand object for each batch size unfortunately!
  // Some hw vendors don't support arbitary sizes anyway, so we create only 
  // multiples of 8 in no. of rows (when M is 0).

  static constexpr int kMetacommandGranulity = 8;
  static constexpr int kMaxMetacommands =
      (kMaxSupportedBatchSize * 4) / kMetacommandGranulity;
  ID3D12MetaCommand* meta_commands_[kMaxMetacommands];

  DXAlloc scratch_data_persistent_[kMaxMetacommands];
  DXAlloc scratch_data_temporary_[kMaxMetacommands];

  bool rows_known_;
 public:
  GemmMetaCommand(DxContext* pContext, int M, int N, int K, int gemm_batch,
                  bool fp16, bool ATranspose, bool BTranspose);
  ~GemmMetaCommand();

  void PerformGemm(int rows, DXAlloc A, DXAlloc B, DXAlloc Output,
                   ID3D12GraphicsCommandList5* command_list);
};

class ConvLayer : public BaseLayer {
  using BaseLayer::C;
  using BaseLayer::GetC;
  using BaseLayer::GetH;
  using BaseLayer::GetW;
  using BaseLayer::H;
  using BaseLayer::W;

 public:
  ConvLayer(bool fp16, GemmMetaCommand* pMetaCommand, DxContext* pContext,
            BaseLayer* ip, int C, int H, int W, int size, int Cin, bool bias,
            bool relu, bool skipAdd = false);
  ~ConvLayer();

  // returns space in uploadBuffer used for loading weights
  void LoadWeights(float* pfilter, float* pBias, DxContext *pContext);
  void Eval(int N, DXAlloc output, DXAlloc input, DXAlloc input2,
            DXAlloc scratch, DXAlloc scratc2,
            ID3D12GraphicsCommandList5* pCL) override;

 private:
  const int c_input_;
  const int filter_size_;
  const bool use_relu_;
  const bool use_bias_;
  const bool skip_add_;

  DXAlloc biases_;
  DXAlloc weights_;
  DXAlloc transformed_weights_; // After winograd transform.

  ShaderWrapper* shader_wrapper_;
  GemmMetaCommand* meta_command_;
};

class FCLayer : public BaseLayer {
 public:
  FCLayer(bool fp16, DxContext* pContext, BaseLayer* ip,
          int C, int H, int W, bool bias, bool relu, bool tanh);
  ~FCLayer();

  // returns space in uploadBuffer used for loading weights
  void LoadWeights(float* cpuWeight, float* cpuBias, DxContext *pContext);
  void Eval(int N, DXAlloc output, DXAlloc input, DXAlloc input2,
            DXAlloc scratch, DXAlloc scratc2,
            ID3D12GraphicsCommandList5* pCL) override;

 private:
  const bool use_bias_;

  // Only one of the below 2 activation functions should be enabled.
  const bool use_relu_;
  const bool use_tanh_;

  DXAlloc biases_;
  DXAlloc weights_;
  ShaderWrapper* shader_wrapper_;
  GemmMetaCommand* meta_command_;
};

}  // namespace dx_backend
}  // namespace lczero