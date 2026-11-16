/**
 * @file Shaders.hpp
 * @brief Modular WGSL Shader accessors backed by `#embed` `.wgsl` files and `ShaderManager` `#include` expansion.
 */
#ifndef BGL_GPU_SHADERS_HPP
#define BGL_GPU_SHADERS_HPP

#include "gpu/ShaderManager.hpp"
#include <string_view>

namespace bgl::gpu {

inline const std::string_view kBrdfLutComputeWGSL{ShaderManager::GetEmbeddedPreprocessed("brdf_lut.wgsl")};
inline const std::string_view kGgxPrefilterComputeWGSL{ShaderManager::GetEmbeddedPreprocessed("ggx_prefilter.wgsl")};
inline const std::string_view kTonemapShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("tonemap.wgsl")};
inline const std::string_view kShadowShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("shadow.wgsl")};
inline const std::string_view kPbrShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("pbr.wgsl")};
inline const std::string_view kAabbShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("aabb.wgsl")};
inline const std::string_view kWireframeShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("wireframe.wgsl")};
inline const std::string_view kMipGenShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("mipgen.wgsl")};
inline const std::string_view kSkyboxShaderWGSL{ShaderManager::GetEmbeddedPreprocessed("skybox.wgsl")};
inline const std::string_view kGpuCullComputeWGSL{ShaderManager::GetEmbeddedPreprocessed("gpu_cull.wgsl")};
inline const std::string_view kGpuRayPickComputeWGSL{ShaderManager::GetEmbeddedPreprocessed("gpu_ray_pick.wgsl")};

} // namespace bgl::gpu

#endif // BGL_GPU_SHADERS_HPP
