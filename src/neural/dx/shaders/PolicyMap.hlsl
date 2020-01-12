#include "shader_shared.h"

// ------------------- Policy Map Shader -----------------------------//
RWBuffer<float> input : register(u8);

// Output is always fp32.
RWStructuredBuffer<float> output  : register(u1);

// Weights are always int32.
RWStructuredBuffer<int> indices : register(u2);

cbuffer PolicyMapConsts : register(b0) {
  uint N;
  uint inputSize;
  uint usedSize;
  uint outputSize;
};

[numthreads(kPolicyMapBlockSize, 1, 1)] 
void PolicyMapShader
(
  uint3 globalThreadIdx : SV_DispatchThreadID
) 
{
  int tid = globalThreadIdx.x;
  int n = tid / usedSize;
  int i = tid % usedSize;

  if (n >= N) return;

  int j = indices[i];

  if (j >= 0) {
    output[n * outputSize + j] = input[n * inputSize + i];
  }
}