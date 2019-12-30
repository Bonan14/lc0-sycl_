del shaders.h

dxc /Tcs_6_2 /EExpandPlanes_shader_fp32 /Fh temp.txt ExpandPlanes.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /EExpandPlanes_shader_fp16 /Fh temp.txt ExpandPlanes.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /Einput_transform_shader_fp16 /DFP16_IO=1 /Fh temp.txt WinogradTransform.hlsl  -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /Eoutput_transform_shader_fp16 /DFP16_IO=1 /Fh temp.txt WinogradTransform.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /Einput_transform_shader_fp32 /Fh temp.txt WinogradTransform.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /Eoutput_transform_shader_fp32 /Fh temp.txt WinogradTransform.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt


dxc /Tcs_6_2 /Econv_1x1_shader_fp16 /DFP16_IO=1 /Fh temp.txt Conv1x1.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /Econv_1x1_shader_fp32 /Fh temp.txt Conv1x1.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt

dxc /Tcs_6_2 /Eadd_vectors_shader /Fh temp.txt AddVectors.hlsl -enable-16bit-types
type temp.txt >> shaders.h
del temp.txt