/**
 * @file gltf_worker.js
 * @brief Dedicated Web Worker for asynchronous glTF/GLB JSON parsing, AABB telemetry calculation,
 *        and off-main-thread base64 data-URI buffer decoding.
 */
'use strict';

function extractJsonPayload(arrayBuffer) {
  if (!arrayBuffer || arrayBuffer.byteLength < 4) {
    throw new Error('Buffer is empty or too short to be a valid 3D model.');
  }

  const dv = new DataView(arrayBuffer);
  const magic = dv.getUint32(0, true);
  if (magic === 0x46546C67) {
    if (arrayBuffer.byteLength < 20) {
      throw new Error(`Truncated GLB container: Header is only ${arrayBuffer.byteLength} bytes.`);
    }
    const chunkLength = dv.getUint32(12, true);
    const chunkType = dv.getUint32(16, true);
    if (chunkType !== 0x4E4F534A) {
      throw new Error(`Invalid GLB: First chunk type 0x${chunkType.toString(16)} is not JSON.`);
    }
    if (20 + chunkLength > arrayBuffer.byteLength) {
      throw new Error(`Truncated GLB JSON chunk (${chunkLength} bytes).`);
    }
    const jsonBytes = new Uint8Array(arrayBuffer, 20, chunkLength);
    return JSON.parse(new TextDecoder('utf-8', { fatal: false }).decode(jsonBytes));
  }

  return JSON.parse(new TextDecoder('utf-8', { fatal: false }).decode(new Uint8Array(arrayBuffer)));
}

function computeTelemetry(gltf) {
  const minB = [Infinity, Infinity, Infinity];
  const maxB = [-Infinity, -Infinity, -Infinity];
  let totalPrims = 0;
  let totalVerts = 0;

  if (gltf && Array.isArray(gltf.meshes) && Array.isArray(gltf.accessors)) {
    for (let m = 0; m < gltf.meshes.length; ++m) {
      const mesh = gltf.meshes[m];
      if (!mesh || !Array.isArray(mesh.primitives)) continue;
      totalPrims += mesh.primitives.length;
      for (let p = 0; p < mesh.primitives.length; ++p) {
        const prim = mesh.primitives[p];
        if (!prim || !prim.attributes || prim.attributes.POSITION === undefined) continue;
        const acc = gltf.accessors[prim.attributes.POSITION];
        if (!acc) continue;
        totalVerts += acc.count || 0;
        if (acc.min && acc.max) {
          for (let i = 0; i < 3; ++i) {
            if (acc.min[i] < minB[i]) minB[i] = acc.min[i];
            if (acc.max[i] > maxB[i]) maxB[i] = acc.max[i];
          }
        }
      }
    }
  }

  return {
    valid: minB[0] <= maxB[0],
    minB,
    maxB,
    totalPrims,
    totalVerts
  };
}

function decodeEmbeddedBuffers(gltf, transferList) {
  const embeddedBuffers = [];
  if (!gltf || !Array.isArray(gltf.buffers)) return embeddedBuffers;

  for (let i = 0; i < gltf.buffers.length; ++i) {
    const buf = gltf.buffers[i];
    if (!buf || typeof buf.uri !== 'string' || !buf.uri.startsWith('data:')) continue;
    const commaIdx = buf.uri.indexOf(',');
    if (commaIdx === -1) continue;
    const base64Str = buf.uri.slice(commaIdx + 1);
    const binStr = atob(base64Str);
    const bytes = new Uint8Array(binStr.length);
    for (let b = 0; b < binStr.length; ++b) {
      bytes[b] = binStr.charCodeAt(b);
    }
    embeddedBuffers[i] = bytes.buffer;
    transferList.push(bytes.buffer);
  }
  return embeddedBuffers;
}

self.onmessage = function onWorkerMessage(evt) {
  const msg = evt.data;
  if (!msg || typeof msg.id !== 'number') return;

  try {
    let gltf;
    if (msg.cmd === 'parseJsonChunkOnly') {
      const jsonStr = new TextDecoder('utf-8', { fatal: false }).decode(new Uint8Array(msg.buffer));
      gltf = JSON.parse(jsonStr);
    } else {
      gltf = extractJsonPayload(msg.buffer);
    }

    const telemetry = computeTelemetry(gltf);
    const transferList = [];
    const embeddedBuffers = decodeEmbeddedBuffers(gltf, transferList);

    self.postMessage(
      { id: msg.id, ok: true, gltf, telemetry, embeddedBuffers },
      transferList
    );
  } catch (err) {
    self.postMessage({
      id: msg.id,
      ok: false,
      error: err && err.message ? err.message : String(err)
    });
  }
};
