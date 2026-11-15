/**
 * @file ShaderManager.hpp
 * @brief Modular WGSL shader loader, `#include`/`#pragma once` preprocessor, `#embed` fallback, and hot-reload tracker.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bgl::gpu {

/**
 * @struct PreprocessedShader
 * @brief Result of expanding WGSL `#include` directives together with all resolved source dependencies.
 */
struct PreprocessedShader {
    std::string code{};
    std::vector<std::string> dependencyVirtualPaths{};
};

/**
 * @class ShaderManager
 * @brief Manages modular `.wgsl` shaders with C++23 `#embed` bundles and optional filesystem hot-reload on native.
 */
class ShaderManager {
public:
    ShaderManager();

    /**
     * @brief Returns the process-wide default ShaderManager instance.
     */
    [[nodiscard]] static ShaderManager& Instance();

    /**
     * @brief Returns an embedded raw (unpreprocessed) `.wgsl` file view if registered, or `std::nullopt`.
     */
    [[nodiscard]] static std::string_view GetEmbeddedRaw(std::string_view virtualPath) noexcept;

    /**
     * @brief Returns a cached view of a strictly embedded, `#include`-expanded shader.
     * @details Thread-safe construct-on-first-use storage; zero filesystem I/O.
     */
    [[nodiscard]] static std::string_view GetEmbeddedPreprocessed(std::string_view virtualPath);

    /**
     * @brief Sets the base filesystem directory for shader overrides and hot-reload detection.
     * @param dir Directory containing `pbr.wgsl`, `common/uniforms.wgsl`, etc. Empty disables disk overrides.
     */
    void SetShaderDirectory(std::filesystem::path dir);

    /**
     * @brief Returns the active filesystem shader directory (empty when running purely from embedded shaders).
     */
    [[nodiscard]] std::filesystem::path GetShaderDirectory() const;

    /**
     * @brief Resolves and preprocesses a shader from disk (if available) or `#embed` fallback.
     *        Does not modify hot-reload tracking state.
     */
    [[nodiscard]] core::Result<PreprocessedShader> Preprocess(std::string_view virtualPath) const;

    /**
     * @brief Loads and preprocesses an entry shader, recording all included file timestamps for hot-reload.
     */
    [[nodiscard]] core::Result<std::string> LoadShader(std::string_view virtualPath);

    /**
     * @brief Loads, preprocesses, and compiles a WGSL shader module cleanly on `device`.
     */
    [[nodiscard]] core::Result<ShaderModuleHandle> LoadAndCompile(WGPUDevice device, std::string_view virtualPath,
                                                                  std::string_view label, bool validateScope = false);

    /**
     * @brief Compiles a WGSL source string into a `WGPUShaderModule` with optional validation error scope.
     */
    [[nodiscard]] static core::Result<ShaderModuleHandle>
    CompileWgslModule(WGPUDevice device, std::string_view code, std::string_view label, bool validateScope = false);

    /**
     * @brief Checks tracked entry shaders and their `#include` dependencies for modified on-disk timestamps.
     * @return List of entry virtual paths (e.g. `"pbr.wgsl"`, `"shadow.wgsl"`) whose source or includes changed.
     */
    [[nodiscard]] std::vector<std::string> PollModifiedShaders();

    /**
     * @brief Resolves a `#include "target"` path relative to `includingVirtualPath` using lexical normalization.
     */
    [[nodiscard]] static std::string ResolveIncludePath(std::string_view includingVirtualPath,
                                                        std::string_view includeTarget);

    using SourceLookupFn = std::function<core::Result<std::string>(std::string_view resolvedVirtualPath)>;

    /**
     * @brief Pure `#include` / `#pragma once` preprocessor operating on an arbitrary source lookup callback.
     */
    [[nodiscard]] static core::Result<PreprocessedShader>
    PreprocessWithLookup(std::string_view entryVirtualPath, const SourceLookupFn& lookup, size_t maxDepth = 16);

private:
    struct TrackedDep {
        std::filesystem::path diskPath{};
        bool existedOnDisk{false};
        std::filesystem::file_time_type lastWriteTime{};
    };

    [[nodiscard]] std::filesystem::path DetectDefaultShaderDirectory() const;
    [[nodiscard]] core::Result<std::string> ReadSourceFile(std::string_view virtualPath) const;

    mutable std::mutex mutex_{};
    std::filesystem::path shaderDir_{};
    std::unordered_map<std::string, std::vector<TrackedDep>> trackedEntries_{};
};

} // namespace bgl::gpu
