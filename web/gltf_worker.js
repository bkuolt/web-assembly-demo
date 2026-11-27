/**
 * @file gltf_worker.js
 * @brief Dedicated Web Worker for asynchronous glTF/GLB JSON parsing, AABB telemetry calculation,
 *        and off-main-thread base64 data-URI buffer decoding.
 */
'use strict';

const MAX_MODEL_BYTES = 128 * 1024 * 1024;
const MAX_EMBEDDED_BUFFERS = 64;

function extractJsonPayload(arrayBuffer) {
  if (!arrayBuffer || arrayBuffer.byteLength < 4 || arrayBuffer.byteLength > MAX_MODEL_BYTES) {
    throw new Error('Buffer is empty, too short, or exceeds maximum allowed size.');
  }

  const dv = new DataView(arrayBuffer);
  const magic = dv.getUint32(0, true);
  if (magic === 0x46546C67) {
    if (arrayBuffer.byteLength < 20) {
      throw new Error(`Truncated GLB container: Header is only ${arrayBuffer.byteLength} bytes.`);
    }
    const version = dv.getUint32(4, true);
    if (version !== 2) {
      throw new Error(`Unsupported GLB version: ${version}. Expected GLB v2.`);
    }
    const totalLength = dv.getUint32(8, true);
    if (totalLength < 20 || totalLength > arrayBuffer.byteLength) {
      throw new Error(`Truncated or invalid GLB length (${totalLength} vs ${arrayBuffer.byteLength} bytes).`);
    }
    const chunkLength = dv.getUint32(12, true);
    const chunkType = dv.getUint32(16, true);
    if (chunkType !== 0x4E4F534A) {
      throw new Error(`Invalid GLB: First chunk type 0x${chunkType.toString(16)} is not JSON.`);
    }
    if (chunkLength > totalLength - 20) {
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
        if (!prim || !prim.attributes || !Number.isInteger(prim.attributes.POSITION)) continue;
        const posIdx = prim.attributes.POSITION;
        if (posIdx < 0 || posIdx >= gltf.accessors.length) continue;
        const acc = gltf.accessors[posIdx];
        if (!acc || !Number.isFinite(acc.count) || acc.count < 0) continue;
        totalVerts += acc.count;
        if (Array.isArray(acc.min) && Array.isArray(acc.max) && acc.min.length >= 3 && acc.max.length >= 3) {
          for (let i = 0; i < 3; ++i) {
            if (Number.isFinite(acc.min[i]) && acc.min[i] < minB[i]) minB[i] = acc.min[i];
            if (Number.isFinite(acc.max[i]) && acc.max[i] > maxB[i]) maxB[i] = acc.max[i];
          }
        }
      }
    }
  }

  return {
    valid: Number.isFinite(minB[0]) && Number.isFinite(maxB[0]) && minB[0] <= maxB[0],
    minB,
    maxB,
    totalPrims,
    totalVerts
  };
}

function decodeEmbeddedBuffers(gltf, transferList) {
  const embeddedBuffers = [];
  if (!gltf || !Array.isArray(gltf.buffers)) return embeddedBuffers;

  const count = Math.min(gltf.buffers.length, MAX_EMBEDDED_BUFFERS);
  let totalDecodedBytes = 0;
  for (let i = 0; i < count; ++i) {
    const buf = gltf.buffers[i];
    if (!buf || typeof buf.uri !== 'string' || !/^data:/i.test(buf.uri)) continue;
    const commaIdx = buf.uri.indexOf(',');
    if (commaIdx === -1) continue;
    const header = buf.uri.slice(5, commaIdx).toLowerCase();
    const payload = buf.uri.slice(commaIdx + 1);
    try {
      let bytes;
      if (header.includes(';base64')) {
        const remainingBudget = MAX_MODEL_BYTES - totalDecodedBytes;
        if (payload.length > Math.ceil(remainingBudget * 4 / 3) + 4) {
          throw new Error('Encoded base64 embedded buffer exceeds size limit');
        }
        const binStr = atob(payload);
        if (binStr.length > remainingBudget) {
          throw new Error('Decoded embedded buffer exceeds size limit');
        }
        bytes = new Uint8Array(binStr.length);
        for (let b = 0; b < binStr.length; ++b) {
          bytes[b] = binStr.charCodeAt(b);
        }
      } else {
        const text = decodeURIComponent(payload);
        bytes = new TextEncoder().encode(text);
        if (bytes.byteLength > MAX_MODEL_BYTES || totalDecodedBytes + bytes.byteLength > MAX_MODEL_BYTES) {
          throw new Error('Decoded embedded buffer exceeds size limit');
        }
      }
      totalDecodedBytes += bytes.byteLength;
      embeddedBuffers[i] = bytes.buffer;
      transferList.push(bytes.buffer);
    } catch (_e) {
      // Skip corrupted embedded buffer URI cleanly
    }
  }
  return embeddedBuffers;
}

self.onmessage = function onWorkerMessage(evt) {
  const msg = evt.data;
  if (!msg || typeof msg.id !== 'number') return;

  try {
    if (!msg.buffer || msg.buffer.byteLength > MAX_MODEL_BYTES) {
      throw new Error('Buffer exceeds maximum allowed size.');
    }
    let gltf;
    if (msg.cmd === 'parseJsonChunkOnly') {
      const jsonStr = new TextDecoder('utf-8', { fatal: false }).decode(new Uint8Array(msg.buffer));
      gltf = JSON.parse(jsonStr);
    } else {
      gltf = extractJsonPayload(msg.buffer);
    }

    if (!gltf || typeof gltf !== 'object' || Array.isArray(gltf)) {
      throw new Error('Parsed glTF payload is not a JSON object.');
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
