#include "shader_shared.h"

// ------------------- Add Vectors kernel -----------------------------//
//
// C = act(A + B)
// A and B can have different lengths, mod size is used to pick the required
// element.
// fp16 version processes 2 elements at a time.

RWStructuredBuffer<uint> A : register(u0);
RWStructuredBuffer<uint> B : register(u1);
RWStructuredBuffer<uint> C : register(u2);

cbuffer AddVectorConsts : register(b0) {
  // sizes are /2 for fp16
  uint a_size;
  uint b_size;
  uint c_size;
  uint relu;
  uint act_tanh;
  uint fp16;
};

float2 extractElements(uint packedVal) {
  return float2(f16tof32(packedVal & 0xFFFF),
                f16tof32((packedVal >> 16) & 0xFFFF));
}

[numthreads(kAddVectorsBlockSize, 1, 1)] 
void add_vectors_shader
(
  uint3 globalThreadIdx : SV_DispatchThreadID
) 
{
  int index = globalThreadIdx.x;
  if (index >= c_size) return;
  uint a = A[index % a_size];
  uint b = B[index % b_size];
  uint opVal;
  if (fp16) {
    float2 f2a = extractElements(a);
    float2 f2b = extractElements(b);
    float2 f2c = f2a + f2b;
    if (relu) {
      if (f2c.x < 0) f2c.x = 0;
      if (f2c.y < 0) f2c.y = 0;
    }
    if (act_tanh) {
      f2c = tanh(f2c);
    }
    uint2 opu = f32tof16(f2c);
    opVal = opu.x | (opu.y << 16);
  } else {
    float c = asfloat(a) + asfloat(b);
    if (relu && c < 0) c = 0;
    if (act_tanh) c = tanh(c);
    opVal = asuint(c);
  }
  C[index] = opVal;
}