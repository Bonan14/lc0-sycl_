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
#include "layers_dx.h"
#include <cassert>
#include <cstring>
#include <vector>
#include "comdef.h"
#include "utils/exception.h"

#include "MetaCommand.h"
#include "network_dx.h"

namespace lczero {
namespace dx_backend {

// for testing
size_t totalScratchSpace = 0;

void copyFloatToHalf(dx_half* out, const float* in, int elements) {
  for (int i = 0; i < elements; i++) {
    out[i] = FP32toFP16(in[i]);
  }
}

static void getTensorDesc(TensorDesc* outDesc, int batchSize, int rows,
                          int cols, bool fp16 = true) {
  memset(outDesc, 0, sizeof(TensorDesc));
  outDesc->DimensionCount = 4;
  outDesc->DataType = fp16 ? 1 : 0;

  outDesc->Size[0] = batchSize;
  outDesc->Size[1] = 1;
  outDesc->Size[2] = rows;
  outDesc->Size[3] = cols;

  // row-major by default
  outDesc->Stride[3] = 1;
  outDesc->Stride[2] = cols;
  outDesc->Stride[1] = rows * cols;
  outDesc->Stride[0] = rows * cols;

  for (int i = 0; i < 4; i++) outDesc->StrideAlignment[i] = 1;

  outDesc->BaseAlignmentInBytes = 4096;  // arbitary
  outDesc->PhysicalSizeInElements = batchSize * rows * cols;
}

GemmMetaCommand::GemmMetaCommand(DxContext* pContext, int rows, int cols, int K,
                                 int gemm_batch, bool fp16, bool a_transpose,
                                 bool b_transpose) {
  memset(scratch_data_persistent_, 0, sizeof(scratch_data_persistent_));
  memset(scratch_data_temporary_, 0, sizeof(scratch_data_temporary_));
  memset(meta_commands_, 0, sizeof(meta_commands_));

  // Note: the way GEMM is used, the 'rows'/M - dimension is a function of
  // batch size. gemm_batch is different and unrelated (either 36 for Winograd,
  // or 1 for other FC layers)
  int num_meta_commands = 1;
  if (rows == 0) {
    // Create metacommands for each 'rows' that is multiple of 8.
    num_meta_commands = kMaxMetacommands;
    rows_known_ = false;
  } else {
    rows_known_ = true;
  }

  for (int i = 0; i < num_meta_commands; i++) {
    int num_rows = rows ? rows : (i + 1) * kMetacommandGranulity;

    GemmCreateDesc createDesc = {};
    getTensorDesc(&createDesc.DescOut, gemm_batch, num_rows, cols, fp16);
    getTensorDesc(&createDesc.DescA, gemm_batch, a_transpose ? K : num_rows,
                  a_transpose ? num_rows : K, fp16);
    getTensorDesc(&createDesc.DescB, gemm_batch, b_transpose ? cols : K,
                  b_transpose ? K : cols, fp16);
    createDesc.cMatrixNull = 1;
    createDesc.ActivationIsNull = 1;
    createDesc.Alpha = 1.0;
    createDesc.Beta = 0.0;
    createDesc.Precision = fp16 ? 1 : 0;  // 0 - fp32, 1 - fp16
    createDesc.TransA = a_transpose;
    createDesc.TransB = b_transpose;

    ID3D12MetaCommand* pMetacommand = nullptr;
    HRESULT hr = pContext->getDevice()->CreateMetaCommand(
        GemmGuid, 1, &createDesc, sizeof(createDesc),
        IID_PPV_ARGS(&pMetacommand));

    if (hr != S_OK) {
      throw Exception("Error creating gemm Metacommand\n");
    }

    meta_commands_[i] = pMetacommand;

    size_t persistent_size = pMetacommand->GetRequiredParameterResourceSize(
        D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION, 4);
    size_t temp_size = pMetacommand->GetRequiredParameterResourceSize(
        D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION, 5);

    if (persistent_size) {
#if 0
      totalScratchSpace += persistent_size;
      printf(
          "allocating %llu bytes for persistent metacommand storage, total: "
          "%llu\n",
          persistent_size, totalScratchSpace);
      pContext->CreateAlloc(persistent_size, D3D12_HEAP_TYPE_DEFAULT,
                            scratch_data_persistent_[i]);
#endif
    }

    if (temp_size) {
#if 0
      totalScratchSpace += temp_size;
      printf(
          "allocating %llu bytes for temp metacommand storage, total: "
          "%llu\n",
          temp_size, totalScratchSpace);
      pContext->CreateAlloc(temp_size, D3D12_HEAP_TYPE_DEFAULT,
                            scratch_data_temporary_[i]);
#endif
    }

    InitConvDesc initDesc = {};
    initDesc.PersistentResource = scratch_data_persistent_[i].descHandle;
    initDesc.TemporaryResource = scratch_data_temporary_[i].descHandle;

    pContext->getCommandList()->InitializeMetaCommand(
        meta_commands_[i], &initDesc, sizeof(initDesc));
  }
}

void GemmMetaCommand::PerformGemm(int rows, DXAlloc A, DXAlloc B,
                                  DXAlloc output,
                                  ID3D12GraphicsCommandList5* command_list) {
  int index = 0;
  if (!rows_known_) {
    index = DivUp(rows, 8) - 1;
  }

  ID3D12MetaCommand* meta_command = meta_commands_[index];
  DXAlloc& scratch_persistent = scratch_data_persistent_[index];
  DXAlloc& scratch_temporary = scratch_data_temporary_[index];

  GemmExecuteDesc exec_desc = {};
  exec_desc.AResource = A.descHandle;
  exec_desc.BResource = B.descHandle;
  exec_desc.OutputResource = output.descHandle;
  exec_desc.PersistentResource = scratch_persistent.descHandle;
  exec_desc.TemporaryResource = scratch_temporary.descHandle;

  command_list->ExecuteMetaCommand(meta_command, &exec_desc, sizeof(exec_desc));
}

GemmMetaCommand::~GemmMetaCommand() {
  for (int i = 0; i < kMaxMetacommands; i++) {
    if (scratch_data_temporary_[i].pResource)
      scratch_data_temporary_[i].pResource->Release();
    if (scratch_data_persistent_[i].pResource)
      scratch_data_persistent_[i].pResource->Release();
    if (meta_commands_[i]) meta_commands_[i]->Release();
  }
}

BaseLayer::BaseLayer(int c, int h, int w, BaseLayer* ip, DxContext* pContext,
                     bool fp16)
    : input_(ip), C(c), H(h), W(w), dx_context_(pContext), fp16_(fp16) {}

ConvLayer::ConvLayer(bool fp16, GemmMetaCommand* pMetaCommand,
                     DxContext* pContext, BaseLayer* ip, int C, int H, int W,
                     int filter, int Cin, bool relu, bool bias, bool skipAdd)
    : BaseLayer(C, H, W, ip, pContext, fp16),
      meta_command_(pMetaCommand),
      c_input_(Cin),
      filter_size_(filter),
      use_relu_(relu),
      use_bias_(bias),
      skip_add_(skipAdd),
      weights_(),
      transformed_weights_(),
      biases_() {
  size_t element_size = fp16 ? sizeof(dx_half) : sizeof(float);
  size_t weight_size = element_size * C * Cin * filter * filter;
  size_t blas_size = element_size * C;

  pContext->CreateAlloc(weight_size, D3D12_HEAP_TYPE_DEFAULT, weights_);

  if (filter == 3) {
    // 6x6 transformed filter size, for 3x3 convolution
    pContext->CreateAlloc(weight_size * 4, D3D12_HEAP_TYPE_DEFAULT,
                          transformed_weights_);
  }

  if (use_bias_) {
    pContext->CreateAlloc(blas_size, D3D12_HEAP_TYPE_DEFAULT, biases_);
  }
  shader_wrapper_ = pContext->getShaderWrapper();
}

template <int M, int N, int K, typename T>
void matrixMulCPU(T* c, T* a, T* b) {
  for (int i = 0; i < M; ++i)
    for (int j = 0; j < N; ++j) {
      float S = 0;
      for (int k = 0; k < K; ++k)
        S += (float)(a[i * K + k]) * (float)(b[k * N + j]);
      c[i * N + j] = (T)S;
    }
}

template <typename T>
void filterTransform4x4(T* transformedFilter, T* filter) {
  // transform applied to filter (of size 3x3)
  T G[6 * 3] = {1.0 / 4,  0,         0,        -1.0 / 6, -1.0 / 6, -1.0 / 6,
                -1.0 / 6, 1.0 / 6,   -1.0 / 6, 1.0 / 24, 1.0 / 12, 1.0 / 6,
                1.0 / 24, -1.0 / 12, 1.0 / 6,  0,        0,        1};

  T Gt[3 * 6] = {1.0 / 4, -1.0 / 6, -1.0 / 6, 1.0 / 24, 1.0 / 24,  0,
                 0,       -1.0 / 6, 1.0 / 6,  1.0 / 12, -1.0 / 12, 0,
                 0,       -1.0 / 6, -1.0 / 6, 1.0 / 6,  1.0 / 6,   1};

  T tempFilter[6 * 3];
  matrixMulCPU<6, 3, 3, T>(tempFilter, G, filter);
  matrixMulCPU<6, 6, 3, T>(transformedFilter, tempFilter, Gt);
}

#define FILTER_IDX_NCHW(k, c, h, w) ((k)*C * S * R + (c)*S * R + (h)*R + w)

// Transform filter for winograd.
// (e.g: for K C H W - 256x256x3x3, filter output is 6x6x256x256 - H W K C)
void transformFilterTensor_Winograd4x4(int K, int C, float* transformedFilter,
                                       const float* weight) {
  constexpr int S = 3;
  constexpr int R = 3;

  for (int k = 0; k < K; k++) {
    for (int c = 0; c < C; c++) {
      // 1. read single filter from memory
      float filterTile[3][3];
      for (int s = 0; s < S; s++)
        for (int r = 0; r < R; r++) {
          filterTile[s][r] = weight[FILTER_IDX_NCHW(k, c, s, r)];
        }

      // 2. transform it
      float transformedFilterTile[6][6];
      filterTransform4x4(&(transformedFilterTile[0][0]), &(filterTile[0][0]));

      // 3. write it back to memory (in HWCK layout)
      for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) {
          transformedFilter[i * 6 * C * K + j * C * K + c * K + k] =
              transformedFilterTile[i][j];
        }
    }
  }
}

void ConvLayer::LoadWeights(float* pfilter, float* pBias, DxContext* pContext) {
  int num_weights = c_input_ * C * filter_size_ * filter_size_;
  size_t element_size = fp16_ ? sizeof(dx_half) : sizeof(float);
  size_t weight_size = element_size * num_weights;
  size_t bias_size = element_size * C;

  std::vector<dx_half> temp(num_weights);
  if (fp16_) {
    copyFloatToHalf(temp.data(), pfilter, num_weights);
    pContext->scheduleUpload(weights_, temp.data(), weight_size);
  } else {
    pContext->scheduleUpload(weights_, pfilter, weight_size);
  }

  if (pBias) {
    if (fp16_) {
      copyFloatToHalf(temp.data(), pBias, C);
      pContext->scheduleUpload(biases_, temp.data(), bias_size);
    } else {
      pContext->scheduleUpload(biases_, pBias, bias_size);
    }
  }

  if (filter_size_ == 3) {
    std::vector<float> temp_transformed(num_weights * 4);
    transformFilterTensor_Winograd4x4(C, c_input_, temp_transformed.data(),
                                      pfilter);
    if (fp16_) {
      std::vector<dx_half> temp_transformed_half(num_weights * 4);
      copyFloatToHalf(temp_transformed_half.data(), temp_transformed.data(),
                      num_weights * 4);
      pContext->scheduleUpload(transformed_weights_,
                               temp_transformed_half.data(), weight_size * 4);
    } else {
      pContext->scheduleUpload(transformed_weights_, temp_transformed.data(),
                               weight_size * 4);
    }
  }
}

bool firstTime = false;

void ConvLayer::Eval(int N, DXAlloc output, DXAlloc input, DXAlloc input2,
                     DXAlloc scratch, DXAlloc scratch2,
                     ID3D12GraphicsCommandList5* command_list) {
  if (filter_size_ == 3) {
    // Need to pad up the input to gemm too (i.e, the transformed Input tensor)!
    // It's in HWNC layout, and 'N'/GemmN needs to be padded up (HW = 6x6)
    // to make it simple, just pad up N to multiple of 2 here (so that gemmN is
    // multiple of 8).
    // TODO: figure out why padding up by 4 is needed (instead of 2!)
    N = ((N + 3) / 4) * 4;

    if (firstTime) {
      // Ankan - upload garbage to see if it matters?
      dx_half testData[112 * 64];
      memset(testData, 0x0, sizeof(testData));
      for (int i = 0; i < 112 * 64; i++) testData[i] = FP32toFP16(0.9f);
      DXAlloc testAlloc = input;
      testAlloc.offset += sizeof(testData);
      testAlloc.gpuVA += sizeof(testData);
      dx_context_->scheduleUpload(testAlloc, testData, sizeof(testData));
    }

    // 1. Input transform (input->scratch2)
    shader_wrapper_->inputTransform(command_list, scratch, input, N, c_input_,
                                    fp16_);

    command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(nullptr));

    if (firstTime) {
      printf("\nThe input");
      dx_context_->dumpTensor(input, 2 * 112 * 8 * 8, fp16_);

      printf("\nAfter input transform");
      dx_context_->dumpTensor(scratch, 2 * 112 * 12 * 12, fp16_);

      // printf("\nAfter gemm transform");
      // dx_context_->dumpTensor(scratch2, 2 * 256 * 12*12, fp16_);

      firstTime = false;
    }

    // 2. Gemm (scratch2 -> scratch)
    meta_command_->PerformGemm(N * 4, scratch, transformed_weights_, scratch2,
                               command_list);

    command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(nullptr));


    // 3. Output transform (scratch -> output)
    // TODO: handle SE!
    shader_wrapper_->outputTransform(
        command_list, output, scratch2, input2, biases_, scratch, scratch,
        scratch, scratch, N, C, use_relu_, use_bias_, skip_add_, false, fp16_);

  } else if (filter_size_ == 1) {
    shader_wrapper_->conv1x1(command_list, output, input, weights_, biases_, N,
                             c_input_, C, use_relu_, use_bias_, fp16_);
  } else {
    throw Exception("Unsupported filter shape for convolution! ");
  }
}

ConvLayer::~ConvLayer() {
  if (weights_.pResource) weights_.pResource->Release();
  if (biases_.pResource) biases_.pResource->Release();
  if (transformed_weights_.pResource) transformed_weights_.pResource->Release();
}

FCLayer::FCLayer(bool fp16, DxContext* pContext, BaseLayer* ip, int C, int H,
                 int W, bool bias, bool relu, bool tanh)
    : BaseLayer(C, H, W, ip, pContext, fp16),
      use_bias_(bias),
      use_relu_(relu),
      use_tanh_(tanh) {
  size_t element_size = fp16_ ? sizeof(dx_half) : sizeof(float);
  size_t weight_size =
      element_size * C * H * W * ip->GetC() * ip->GetH() * ip->GetW();
  size_t blas_size = element_size * C * H * W;

  pContext->CreateAlloc(weight_size, D3D12_HEAP_TYPE_DEFAULT, weights_);
  if (use_bias_)
    pContext->CreateAlloc(blas_size, D3D12_HEAP_TYPE_DEFAULT, biases_);

  shader_wrapper_ = pContext->getShaderWrapper();

  // Create metacommand object
  int rows = 0;  // batch size
  int cols = C * H * W;
  int K = ip->GetC() * ip->GetH() * ip->GetW();  // cols of input matrix
  // We do Out = A * weight.
  // The weight matrix need to be transpsoed before it can be multiplied.
  meta_command_ =
      new GemmMetaCommand(pContext, rows, cols, K, 1, fp16, false, true);
}

void FCLayer::LoadWeights(float* cpuWeight, float* cpuBias,
                          DxContext* pContext) {
  size_t num_weights =
      C * H * W * input_->GetC() * input_->GetH() * input_->GetW();

  size_t element_size = fp16_ ? sizeof(dx_half) : sizeof(float);
  size_t weight_size = element_size * num_weights;
  size_t num_biases = C * H * W;
  size_t bias_size = element_size * num_biases;

  std::vector<dx_half> temp(num_weights);
  if (fp16_) {
    copyFloatToHalf(temp.data(), cpuWeight, num_weights);
    pContext->scheduleUpload(weights_, temp.data(), weight_size);
  } else {
    pContext->scheduleUpload(weights_, cpuWeight, weight_size);
  }

  if (cpuBias) {
    if (fp16_) {
      copyFloatToHalf(temp.data(), cpuBias, C);
      pContext->scheduleUpload(biases_, temp.data(), bias_size);
    } else {
      pContext->scheduleUpload(biases_, cpuBias, bias_size);
    }
  }
}

void FCLayer::Eval(int N, DXAlloc output, DXAlloc input, DXAlloc input2,
                   DXAlloc scratch, DXAlloc scratch2,
                   ID3D12GraphicsCommandList5* command_list) {
  int num_outputs = C * H * W;
  int num_inputs = input_->GetC() * input_->GetH() * input_->GetW();

  meta_command_->PerformGemm(N, input, weights_, output, command_list);

  if (use_bias_ || use_relu_ || use_tanh_) {
    command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(nullptr));
    shader_wrapper_->addVectors(command_list, output, output, biases_,
                                N * num_outputs, N * num_outputs, num_outputs, 
                                use_relu_, use_tanh_, fp16_);
  }
}

FCLayer::~FCLayer() {
  if (weights_.pResource) weights_.pResource->Release();
  if (biases_.pResource) biases_.pResource->Release();
}

void DxError(HRESULT status, const char* file, const int& line) {
  if (FAILED(status)) {
    assert(0);
    char message[128];
    sprintf(message, "Dx error: %s (%s:%d) ", "generic dx error", file, line);
    throw Exception(message);
  }
}

}  // namespace dx_backend
}  // namespace lczero