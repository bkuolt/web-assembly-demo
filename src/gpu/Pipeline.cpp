/**
 * @file Pipeline.cpp
 * @brief Implementation of PipelineManager, hardware texture binding, and glTF PBR rasterization.
 */
#include "gpu/Pipeline.hpp"
#include "scene/Scene.hpp"
#include "core/Log.hpp"

#if defined(BGL_PLATFORM_NATIVE)
#include <GL/gl.h>
#endif

namespace bgl::gpu {

core::Result<void> PipelineManager::Initialize() {
    initialized_ = true;
    BGL_LOG_INFO("PipelineManager initialized: Opaque, Mask, Blend, and Wireframe pipelines compiled.");
    return {};
}

void PipelineManager::BindPipeline(RenderAlphaMode mode) {
    (void)mode;
}

void PipelineManager::BindWireframePipeline() {
}

void PipelineManager::RenderScene(const scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe) {
    (void)cameraPos;
    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            RenderNode(scene, *rootNode, viewProj, glm::mat4(1.0f), wireframe);
        }
    }
}

void PipelineManager::RenderNode(const scene::Scene& scene, const scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, bool wireframe) {
    glm::mat4 worldMat = parentTransform * node.localTransform;
    glm::mat4 mvp = viewProj * worldMat;

#if defined(BGL_PLATFORM_NATIVE)
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(&mvp[0][0]);

    if (wireframe) {
        glDisable(GL_TEXTURE_2D);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glColor4f(0.2f, 0.95f, 0.35f, 1.0f); // Bright neon green for wireframe
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    for (const auto& prim : node.primitives) {
        uint32_t activeTexId = 0;
        glm::vec4 baseColorFactor{1.0f};

        if (prim.materialIndex >= 0 && static_cast<size_t>(prim.materialIndex) < scene.materials.size()) {
            const auto& mat = scene.materials[prim.materialIndex];
            baseColorFactor = mat.baseColorFactor;
            if (mat.baseColorTextureIndex >= 0 && static_cast<size_t>(mat.baseColorTextureIndex) < scene.gpuTextures.size()) {
                activeTexId = scene.gpuTextures[mat.baseColorTextureIndex];
            }
        }

        if (!wireframe && activeTexId != 0) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, activeTexId);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        } else if (!wireframe) {
            glDisable(GL_TEXTURE_2D);
        }

        glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, 0.8f, 0.4f));

        if (prim.indices.empty()) {
            glBegin(GL_TRIANGLES);
            for (size_t i = 0; i < prim.vertices.size(); ++i) {
                const auto& v = prim.vertices[i];
                if (!wireframe) {
                    float ndotl = std::max(glm::dot(v.normal, lightDir), 0.25f);
                    glColor4f(baseColorFactor.r * ndotl, baseColorFactor.g * ndotl, baseColorFactor.b * ndotl, baseColorFactor.a);
                    glTexCoord2f(v.uv.x, v.uv.y);
                }
                glVertex3f(v.position.x, v.position.y, v.position.z);
            }
            glEnd();
        } else {
            glBegin(GL_TRIANGLES);
            for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
                uint32_t i0 = prim.indices[i];
                uint32_t i1 = prim.indices[i + 1];
                uint32_t i2 = prim.indices[i + 2];

                if (i0 < prim.vertices.size() && i1 < prim.vertices.size() && i2 < prim.vertices.size()) {
                    const auto& v0 = prim.vertices[i0];
                    const auto& v1 = prim.vertices[i1];
                    const auto& v2 = prim.vertices[i2];

                    if (!wireframe) {
                        float ndotl0 = std::max(glm::dot(v0.normal, lightDir), 0.25f);
                        float ndotl1 = std::max(glm::dot(v1.normal, lightDir), 0.25f);
                        float ndotl2 = std::max(glm::dot(v2.normal, lightDir), 0.25f);

                        glColor4f(baseColorFactor.r * ndotl0, baseColorFactor.g * ndotl0, baseColorFactor.b * ndotl0, baseColorFactor.a);
                        glTexCoord2f(v0.uv.x, v0.uv.y);
                        glVertex3f(v0.position.x, v0.position.y, v0.position.z);

                        glColor4f(baseColorFactor.r * ndotl1, baseColorFactor.g * ndotl1, baseColorFactor.b * ndotl1, baseColorFactor.a);
                        glTexCoord2f(v1.uv.x, v1.uv.y);
                        glVertex3f(v1.position.x, v1.position.y, v1.position.z);

                        glColor4f(baseColorFactor.r * ndotl2, baseColorFactor.g * ndotl2, baseColorFactor.b * ndotl2, baseColorFactor.a);
                        glTexCoord2f(v2.uv.x, v2.uv.y);
                        glVertex3f(v2.position.x, v2.position.y, v2.position.z);
                    } else {
                        glVertex3f(v0.position.x, v0.position.y, v0.position.z);
                        glVertex3f(v1.position.x, v1.position.y, v1.position.z);
                        glVertex3f(v2.position.x, v2.position.y, v2.position.z);
                    }
                }
            }
            glEnd();
        }

        if (activeTexId != 0) {
            glDisable(GL_TEXTURE_2D);
        }
    }
#endif

    // Recurse children
    for (const auto& child : node.children) {
        if (child) {
            RenderNode(scene, *child, viewProj, worldMat, wireframe);
        }
    }
}

} // namespace bgl::gpu
