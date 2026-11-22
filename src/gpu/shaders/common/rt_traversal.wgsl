#pragma once

struct RtVertex {
    posU : vec4<f32>,
    normV : vec4<f32>,
};

struct RtBvhNode {
    aabbMin : vec3<f32>,
    leftFirst : u32,
    aabbMax : vec3<f32>,
    triCount : u32,
};

struct RtInstance {
    model : mat4x4<f32>,
    invModel : mat4x4<f32>,
    aabbMin : vec3<f32>,
    blasRootNode : u32,
    aabbMax : vec3<f32>,
    materialIdx : u32,
    vertexOffset : u32,
    triOffset : u32,
    flags : u32,
    nodeIndex : u32,
};

struct RtMaterial {
    baseColorFactor : vec4<f32>,
    emissiveFactor : vec4<f32>,
    pbrParams : vec4<f32>,   // x=metallic, y=roughness, z=alphaCutoff, w=ior
    texIndices : vec4<i32>,  // x=baseColor, y=mr, z=normal, w=emissive
    flagsAlpha : vec4<u32>,  // x=flags, y=alphaMode, z=doubleSided, w=transmissive
};

struct RtHit {
    hit : bool,
    t : f32,
    instIdx : u32,
    triIdx : u32,
    materialIdx : u32,
    worldPos : vec3<f32>,
    worldNormal : vec3<f32>,
    uv : vec2<f32>,
};

@group(1) @binding(0) var<storage, read> rtTlasNodes : array<RtBvhNode>;
@group(1) @binding(1) var<storage, read> rtInstances : array<RtInstance>;
@group(1) @binding(2) var<storage, read> rtBlasNodes : array<RtBvhNode>;
@group(1) @binding(3) var<storage, read> rtIndices : array<u32>;
@group(1) @binding(4) var<storage, read> rtVertices : array<RtVertex>;
@group(1) @binding(5) var<storage, read> rtMaterials : array<RtMaterial>;

fn rtSafeInvDir(d : vec3<f32>) -> vec3<f32> {
    let sx = select(-1e8, 1e8, d.x >= 0.0);
    let sy = select(-1e8, 1e8, d.y >= 0.0);
    let sz = select(-1e8, 1e8, d.z >= 0.0);
    return vec3<f32>(
        select(sx, 1.0 / d.x, abs(d.x) > 1e-7),
        select(sy, 1.0 / d.y, abs(d.y) > 1e-7),
        select(sz, 1.0 / d.z, abs(d.z) > 1e-7)
    );
}

fn rtIntersectAabb(ro : vec3<f32>, invD : vec3<f32>, bmin : vec3<f32>, bmax : vec3<f32>, tMax : f32) -> f32 {
    let eps = vec3<f32>(1e-5);
    let t0 = ((bmin - eps) - ro) * invD;
    let t1 = ((bmax + eps) - ro) * invD;
    let tSmall = min(t0, t1);
    let tBig = max(t0, t1);
    let tmin = max(0.0, max(tSmall.x, max(tSmall.y, tSmall.z)));
    let tmax = min(tMax, min(tBig.x, min(tBig.y, tBig.z)));
    return select(1e30, tmin, tmax >= tmin);
}

fn rtTraceClosest(ro : vec3<f32>, rd : vec3<f32>, tMin : f32, tMax : f32) -> RtHit {
    var best : RtHit;
    best.hit = false;
    best.t = tMax;
    best.instIdx = 0u;
    best.triIdx = 0u;
    best.materialIdx = 0u;
    best.worldPos = vec3<f32>(0.0);
    best.worldNormal = vec3<f32>(0.0, 1.0, 0.0);
    best.uv = vec2<f32>(0.0);

    var bestU = 0.0;
    var bestV = 0.0;

    let tlasCount = arrayLength(&rtTlasNodes);
    if (tlasCount == 0u) {
        return best;
    }

    let worldInvD = rtSafeInvDir(rd);
    var tlasStack : array<u32, 32>;
    var tlasSp : u32 = 0u;
    tlasStack[0] = 0u;
    tlasSp = 1u;

    while (tlasSp > 0u) {
        tlasSp = tlasSp - 1u;
        let nodeIdx = tlasStack[tlasSp];
        let node = rtTlasNodes[nodeIdx];
        if (rtIntersectAabb(ro, worldInvD, node.aabbMin, node.aabbMax, best.t) >= best.t) {
            continue;
        }
        if (node.triCount > 0u) {
            for (var i : u32 = 0u; i < node.triCount; i = i + 1u) {
                let instIdx = node.leftFirst + i;
                let inst = rtInstances[instIdx];
                if (rtIntersectAabb(ro, worldInvD, inst.aabbMin, inst.aabbMax, best.t) >= best.t) {
                    continue;
                }

                let localRo = (inst.invModel * vec4<f32>(ro, 1.0)).xyz;
                let localRd = (inst.invModel * vec4<f32>(rd, 0.0)).xyz;
                let localInvD = rtSafeInvDir(localRd);

                var blasStack : array<u32, 32>;
                var blasSp : u32 = 0u;
                blasStack[0] = 0u;
                blasSp = 1u;

                while (blasSp > 0u) {
                    blasSp = blasSp - 1u;
                    let localNodeIdx = blasStack[blasSp];
                    let bNode = rtBlasNodes[inst.blasRootNode + localNodeIdx];
                    if (rtIntersectAabb(localRo, localInvD, bNode.aabbMin, bNode.aabbMax, best.t) >= best.t) {
                        continue;
                    }
                    if (bNode.triCount > 0u) {
                        for (var t : u32 = 0u; t < bNode.triCount; t = t + 1u) {
                            let localTri = bNode.leftFirst + t;
                            let gTri = inst.triOffset + localTri;
                            let i0 = inst.vertexOffset + rtIndices[gTri * 3u + 0u];
                            let i1 = inst.vertexOffset + rtIndices[gTri * 3u + 1u];
                            let i2 = inst.vertexOffset + rtIndices[gTri * 3u + 2u];
                            let p0 = rtVertices[i0].posU.xyz;
                            let p1 = rtVertices[i1].posU.xyz;
                            let p2 = rtVertices[i2].posU.xyz;

                            let e1 = p1 - p0;
                            let e2 = p2 - p0;
                            let pvec = cross(localRd, e2);
                            let det = dot(e1, pvec);
                            if (abs(det) < 1e-8) {
                                continue;
                            }
                            let invDet = 1.0 / det;
                            let tvec = localRo - p0;
                            let u = dot(tvec, pvec) * invDet;
                            if (u < 0.0 || u > 1.0) {
                                continue;
                            }
                            let qvec = cross(tvec, e1);
                            let v = dot(localRd, qvec) * invDet;
                            if (v < 0.0 || u + v > 1.0) {
                                continue;
                            }
                            let hitT = dot(e2, qvec) * invDet;
                            if (hitT > tMin && hitT < best.t) {
                                best.hit = true;
                                best.t = hitT;
                                best.instIdx = instIdx;
                                best.triIdx = gTri;
                                best.materialIdx = inst.materialIdx;
                                bestU = u;
                                bestV = v;
                            }
                        }
                    } else {
                        let leftLocal = localNodeIdx + 1u;
                        let rightLocal = bNode.leftFirst;
                        let lNode = rtBlasNodes[inst.blasRootNode + leftLocal];
                        let rNode = rtBlasNodes[inst.blasRootNode + rightLocal];
                        let dL = rtIntersectAabb(localRo, localInvD, lNode.aabbMin, lNode.aabbMax, best.t);
                        let dR = rtIntersectAabb(localRo, localInvD, rNode.aabbMin, rNode.aabbMax, best.t);
                        if (dL < dR) {
                            if (dR < best.t && blasSp < 32u) {
                                blasStack[blasSp] = rightLocal;
                                blasSp = blasSp + 1u;
                            }
                            if (dL < best.t && blasSp < 32u) {
                                blasStack[blasSp] = leftLocal;
                                blasSp = blasSp + 1u;
                            }
                        } else {
                            if (dL < best.t && blasSp < 32u) {
                                blasStack[blasSp] = leftLocal;
                                blasSp = blasSp + 1u;
                            }
                            if (dR < best.t && blasSp < 32u) {
                                blasStack[blasSp] = rightLocal;
                                blasSp = blasSp + 1u;
                            }
                        }
                    }
                }
            }
        } else {
            let leftIdx = nodeIdx + 1u;
            let rightIdx = node.leftFirst;
            let lNode = rtTlasNodes[leftIdx];
            let rNode = rtTlasNodes[rightIdx];
            let dL = rtIntersectAabb(ro, worldInvD, lNode.aabbMin, lNode.aabbMax, best.t);
            let dR = rtIntersectAabb(ro, worldInvD, rNode.aabbMin, rNode.aabbMax, best.t);
            if (dL < dR) {
                if (dR < best.t && tlasSp < 32u) {
                    tlasStack[tlasSp] = rightIdx;
                    tlasSp = tlasSp + 1u;
                }
                if (dL < best.t && tlasSp < 32u) {
                    tlasStack[tlasSp] = leftIdx;
                    tlasSp = tlasSp + 1u;
                }
            } else {
                if (dL < best.t && tlasSp < 32u) {
                    tlasStack[tlasSp] = leftIdx;
                    tlasSp = tlasSp + 1u;
                }
                if (dR < best.t && tlasSp < 32u) {
                    tlasStack[tlasSp] = rightIdx;
                    tlasSp = tlasSp + 1u;
                }
            }
        }
    }

    if (best.hit) {
        let inst = rtInstances[best.instIdx];
        let i0 = inst.vertexOffset + rtIndices[best.triIdx * 3u + 0u];
        let i1 = inst.vertexOffset + rtIndices[best.triIdx * 3u + 1u];
        let i2 = inst.vertexOffset + rtIndices[best.triIdx * 3u + 2u];
        let v0 = rtVertices[i0];
        let v1 = rtVertices[i1];
        let v2 = rtVertices[i2];
        let w = 1.0 - bestU - bestV;
        let localN = v0.normV.xyz * w + v1.normV.xyz * bestU + v2.normV.xyz * bestV;
        let normalMat = transpose(mat3x3<f32>(
            inst.invModel[0].xyz,
            inst.invModel[1].xyz,
            inst.invModel[2].xyz
        ));
        var worldN = normalize(normalMat * localN);
        if (dot(worldN, rd) > 0.0) {
            worldN = -worldN;
        }
        best.worldNormal = worldN;
        best.worldPos = ro + rd * best.t;
        best.uv = vec2<f32>(
            v0.posU.w * w + v1.posU.w * bestU + v2.posU.w * bestV,
            v0.normV.w * w + v1.normV.w * bestU + v2.normV.w * bestV
        );
    }

    return best;
}

fn rtTraceAnyHit(ro : vec3<f32>, rd : vec3<f32>, tMin : f32, tMax : f32) -> bool {
    let tlasCount = arrayLength(&rtTlasNodes);
    if (tlasCount == 0u) {
        return false;
    }

    let worldInvD = rtSafeInvDir(rd);
    var tlasStack : array<u32, 32>;
    var tlasSp : u32 = 1u;
    tlasStack[0] = 0u;

    while (tlasSp > 0u) {
        tlasSp = tlasSp - 1u;
        let nodeIdx = tlasStack[tlasSp];
        let node = rtTlasNodes[nodeIdx];
        if (rtIntersectAabb(ro, worldInvD, node.aabbMin, node.aabbMax, tMax) >= tMax) {
            continue;
        }
        if (node.triCount > 0u) {
            for (var i : u32 = 0u; i < node.triCount; i = i + 1u) {
                let inst = rtInstances[node.leftFirst + i];
                if ((inst.flags & 2u) != 0u) {
                    continue; // Transmissive glass does not block opaque shadow rays
                }
                if (rtIntersectAabb(ro, worldInvD, inst.aabbMin, inst.aabbMax, tMax) >= tMax) {
                    continue;
                }

                let localRo = (inst.invModel * vec4<f32>(ro, 1.0)).xyz;
                let localRd = (inst.invModel * vec4<f32>(rd, 0.0)).xyz;
                let localInvD = rtSafeInvDir(localRd);

                var blasStack : array<u32, 32>;
                var blasSp : u32 = 1u;
                blasStack[0] = 0u;

                while (blasSp > 0u) {
                    blasSp = blasSp - 1u;
                    let localNodeIdx = blasStack[blasSp];
                    let bNode = rtBlasNodes[inst.blasRootNode + localNodeIdx];
                    if (rtIntersectAabb(localRo, localInvD, bNode.aabbMin, bNode.aabbMax, tMax) >= tMax) {
                        continue;
                    }
                    if (bNode.triCount > 0u) {
                        for (var t : u32 = 0u; t < bNode.triCount; t = t + 1u) {
                            let gTri = inst.triOffset + bNode.leftFirst + t;
                            let i0 = inst.vertexOffset + rtIndices[gTri * 3u + 0u];
                            let i1 = inst.vertexOffset + rtIndices[gTri * 3u + 1u];
                            let i2 = inst.vertexOffset + rtIndices[gTri * 3u + 2u];
                            let p0 = rtVertices[i0].posU.xyz;
                            let p1 = rtVertices[i1].posU.xyz;
                            let p2 = rtVertices[i2].posU.xyz;

                            let e1 = p1 - p0;
                            let e2 = p2 - p0;
                            let pvec = cross(localRd, e2);
                            let det = dot(e1, pvec);
                            if (abs(det) < 1e-8) {
                                continue;
                            }
                            let invDet = 1.0 / det;
                            let tvec = localRo - p0;
                            let u = dot(tvec, pvec) * invDet;
                            if (u < 0.0 || u > 1.0) {
                                continue;
                            }
                            let qvec = cross(tvec, e1);
                            let v = dot(localRd, qvec) * invDet;
                            if (v < 0.0 || u + v > 1.0) {
                                continue;
                            }
                            let hitT = dot(e2, qvec) * invDet;
                            if (hitT > tMin && hitT < tMax) {
                                return true;
                            }
                        }
                    } else {
                        if (blasSp < 31u) {
                            blasStack[blasSp] = bNode.leftFirst;
                            blasStack[blasSp + 1u] = localNodeIdx + 1u;
                            blasSp = blasSp + 2u;
                        }
                    }
                }
            }
        } else {
            if (tlasSp < 30u) {
                tlasStack[tlasSp] = node.leftFirst;
                tlasStack[tlasSp + 1u] = nodeIdx + 1u;
                tlasSp = tlasSp + 2u;
            }
        }
    }
    return false;
}

fn rtPcgHash(seed : u32) -> u32 {
    let state = seed * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

fn rtRand01(state : ptr<function, u32>) -> f32 {
    *state = rtPcgHash(*state);
    return f32(*state) * (1.0 / 4294967296.0);
}

fn rtBuildOrthonormalBasis(n : vec3<f32>) -> mat3x3<f32> {
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(n.z) < 0.999);
    let t = normalize(cross(up, n));
    let b = cross(n, t);
    return mat3x3<f32>(t, b, n);
}

fn rtSampleCosineHemisphere(u1 : f32, u2 : f32, n : vec3<f32>) -> vec3<f32> {
    let r = sqrt(clamp(u1, 0.0, 1.0));
    let phi = 6.28318530718 * u2;
    let x = r * cos(phi);
    let y = r * sin(phi);
    let z = sqrt(max(0.0, 1.0 - u1));
    return normalize(rtBuildOrthonormalBasis(n) * vec3<f32>(x, y, z));
}

fn rtWorldPosFromDepth(uv : vec2<f32>, ndcDepth : f32, invViewProj : mat4x4<f32>) -> vec3<f32> {
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcDepth, 1.0);
    let wp = invViewProj * ndc;
    let safeW = select(wp.w, 1e-6, abs(wp.w) < 1e-6);
    return wp.xyz / safeW;
}
