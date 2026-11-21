#!/usr/bin/env node
// Cross-end same-source SFVB v1 workbench projector (Phase 5).
// Compatible with both Node.js (CommonJS) and Browser (<script>, window.SigFlowSfvb).
// Zero third-party dependencies; uses DataView + BigInt for exact 64-bit ticks/seqs.
(function (root, factory) {
    if (typeof module === 'object' && module.exports) {
        const ref = require('./sfvb_reference_decoder.js');
        const fs = require('fs');
        const crypto = require('crypto');
        module.exports = factory(ref, fs, crypto);
    } else {
        root.SigFlowSfvb = factory(null, null, null);
    }
}(typeof globalThis !== 'undefined' ? globalThis : this, function (refDecoder, fs, crypto) {
    'use strict';

    const HEADER = 64;
    const RT_INFO = 1, RT_DICTIONARY = 2, RT_ACKS = 3, RT_PAD_CHANGES = 4, RT_COMPONENTS = 5;
    const RT_TRACE_CURSOR = 6, RT_TRACE_EVENTS = 7, RT_EVENTS = 8, RT_DIAGNOSTICS = 9;

    const STATE_NAMES = new Map([
        [0x00, 'UND'], [0x02, 'FLT'], [0x04, 'WUD'], [0x05, 'WLO'], [0x07, 'WHI'],
        [0x0c, 'SUD'], [0x0d, 'SLO'], [0x0f, 'SHI'], [0x15, 'ILO'], [0x17, 'IHI'],
        [0x1d, 'PLO'], [0x1f, 'PHI']
    ]);

    function stateName(code) {
        const c = Number(code) & 0xff;
        if (c & 0x20) return 'CON';
        return STATE_NAMES.get(c) || 'INVALID';
    }

    function classifyNode(node) {
        if (!node) return 'unknown';
        const code = Number(node.state_code || 0);
        if (node.contention || (code & 0x20)) return 'contention';
        if (code === 2) return 'floating';
        if (!node.valid) return 'unknown';
        return node.logic === '0' ? 'low' : 'high';
    }

    function cloneJson(value) {
        return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
    }

    function sfvbError(message) {
        const err = new Error(message);
        err.sfvb = true;
        return err;
    }

    const utf8Decoder = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8', { fatal: false }) : null;
    const utf8Encoder = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;

    function decodeUtf8(bytes, start, end) {
        if (utf8Decoder) return utf8Decoder.decode(bytes.subarray(start, end));
        return Buffer.from(bytes.buffer, bytes.byteOffset + start, end - start).toString('utf8');
    }

    function encodeUtf8(str) {
        if (utf8Encoder) return utf8Encoder.encode(str);
        return Uint8Array.from(Buffer.from(str, 'utf8'));
    }

    class BrowserReader {
        constructor(bytes, offset = 0) {
            this.bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
            this.at = offset;
            this.end = this.bytes.length;
        }
        need(n) { if (this.end - this.at < n) throw sfvbError('E_SFVB_TRUNCATED'); }
        u8() { this.need(1); return this.bytes[this.at++]; }
        u16() { this.need(2); const v = this.view.getUint16(this.at, true); this.at += 2; return v; }
        u32() { this.need(4); const v = this.view.getUint32(this.at, true); this.at += 4; return v; }
        u64() { this.need(8); const v = this.view.getBigUint64(this.at, true); this.at += 8; return v; }
        str() { const n = this.u16(); this.need(n); const s = decodeUtf8(this.bytes, this.at, this.at + n); this.at += n; return s; }
        blob() { const n = this.u32(); this.need(n); const s = decodeUtf8(this.bytes, this.at, this.at + n); this.at += n; return s; }
        done() { return this.at === this.end; }
    }

    function ackStatusName(code) {
        const map = { 1: 'applied', 2: 'pending', 3: 'superseded', 4: 'blocked', 5: 'rejected', 6: 'observed', 7: 'cancelled' };
        if (!map[code]) throw sfvbError('E_SFVB_STATUS');
        return map[code];
    }

    function ackStatusCode(name) {
        const map = { applied: 1, pending: 2, superseded: 3, blocked: 4, rejected: 5, observed: 6, cancelled: 7 };
        if (!map[name]) throw sfvbError('E_SFVB_STATUS');
        return map[name];
    }

    function kindName(kind) {
        if (kind === 1) return 'snapshot';
        if (kind === 2) return 'delta';
        if (kind === 3) return 'trace';
        if (kind === 4) return 'display';
        throw sfvbError('E_SFVB_KIND');
    }

    function decodeFrameBrowser(rawBytes) {
        const bytes = rawBytes instanceof Uint8Array ? rawBytes : new Uint8Array(rawBytes);
        if (bytes.length < HEADER) throw sfvbError('E_SFVB_HEADER');
        if (!(bytes[0] === 0x53 && bytes[1] === 0x46 && bytes[2] === 0x56 && bytes[3] === 0x42)) throw sfvbError('E_SFVB_MAGIC');
        const r = new BrowserReader(bytes, 4);
        const version = r.u16(), kind = r.u16(), flags = r.u32(), headerBytes = r.u32();
        const payloadBytes = r.u32(), runHandle = r.u32(), generation = r.u32();
        const seq = r.u64(), baseSeq = r.u64(), tick = r.u64();
        const delta = r.u32(), recordCount = r.u32(), reserved = r.u32();
        if (version !== 1) throw sfvbError('E_SFVB_VERSION');
        if (headerBytes !== HEADER || reserved !== 0) throw sfvbError('E_SFVB_HEADER');
        if (payloadBytes !== bytes.length - HEADER || payloadBytes > 64 * 1024 * 1024) throw sfvbError('E_SFVB_PAYLOAD');
        const frame = {
            kind, flags, run_handle: runHandle, generation, seq, base_seq: baseSeq, tick, delta,
            schema: '', run_id: '', state_encoding: '', state: '', reason: '', error: '', tick_ps: 0n,
            dictionary: [], acks: [], pad_changes: [], components: [],
            trace: { revision: 0n, committed_through_tick: 0n, committed_event_seq: 0n, cursor: '' },
            trace_events: [], events: [], diagnostics: []
        };
        r.at = HEADER;
        let records = 0;
        const next = () => r.done() ? 0 : r.u16();
        if (next() !== RT_INFO) throw sfvbError('E_SFVB_RECORD_ORDER');
        {
            const len = r.u32(), end = r.at + len;
            frame.schema = r.str(); frame.run_id = r.str(); frame.state_encoding = r.str();
            frame.state = r.str(); frame.reason = r.str(); frame.error = r.str(); frame.tick_ps = r.u64();
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
        }
        records++;
        if (frame.schema !== 'sigflow.virtual-frame.v1') throw sfvbError('E_SFVB_SCHEMA');
        let type = next();
        if (type === RT_DICTIONARY) {
            if (frame.kind !== 1) throw sfvbError('E_SFVB_DELTA_DICTIONARY');
            const len = r.u32(), end = r.at + len, count = r.u32();
            for (let i = 0; i < count; i++) { const id = r.u32(), kb = r.u8(), name = r.str(); frame.dictionary.push([id, kb, name]); }
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_ACKS) {
            const len = r.u32(), end = r.at + len, count = r.u32();
            for (let i = 0; i < count; i++) {
                const commandId = r.str(), code = r.u8(), appliedTick = r.u64(), appliedDelta = r.u32(), appliedOrder = r.u64();
                frame.acks.push({ command_id: commandId, status: ackStatusName(code), applied_tick: appliedTick, applied_delta: appliedDelta, applied_order: appliedOrder });
            }
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_PAD_CHANGES) {
            const len = r.u32(), end = r.at + len, count = r.u32();
            for (let i = 0; i < count; i++) {
                const id = r.u32(), stateCode = r.u8(), logic = r.u8(), flag = r.u8();
                frame.pad_changes.push({ numeric_id: id, target: '', state_code: stateCode, logic: String.fromCharCode(logic), valid: !!(flag & 1), contention: !!(flag & 2) });
            }
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_COMPONENTS) {
            const len = r.u32(), end = r.at + len, count = r.u32();
            for (let i = 0; i < count; i++) { const id = r.u32(), blob = r.blob(); frame.components.push({ numeric_id: id, component: '', state: JSON.parse(blob) }); }
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_TRACE_CURSOR) {
            const len = r.u32(), end = r.at + len;
            frame.trace.revision = r.u64(); frame.trace.committed_through_tick = r.u64();
            frame.trace.committed_event_seq = r.u64(); frame.trace.cursor = r.str();
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_TRACE_EVENTS) {
            const len = r.u32(), end = r.at + len, count = r.u32();
            for (let i = 0; i < count; i++) {
                const id = r.u32(), tickv = r.u64(), dv = r.u32(), ev = r.u64(), value = r.u8(), stateCode = r.u8(), flag = r.u8();
                frame.trace_events.push({ numeric_id: id, tick: tickv, event_seq: ev, delta: dv, value: String.fromCharCode(value), state_code: stateCode, valid: !!(flag & 1), contention: !!(flag & 2), has_state_code: !!(flag & 4) });
            }
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_EVENTS) {
            const len = r.u32(), end = r.at + len, count = r.u32();
            for (let i = 0; i < count; i++) { const name = r.str(), blob = r.blob(); frame.events.push({ type: name, detail: JSON.parse(blob) }); }
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type === RT_DIAGNOSTICS) {
            const len = r.u32(), end = r.at + len;
            frame.diagnostics = JSON.parse(r.blob());
            if (r.at !== end) throw sfvbError('E_SFVB_RECORD_SIZE');
            records++; type = next();
        }
        if (type !== 0 || !r.done()) throw sfvbError('E_SFVB_RECORD_ORDER');
        if (records !== recordCount) throw sfvbError('E_SFVB_RECORD_COUNT');
        if (frame.kind === 1 && frame.base_seq !== 0n) throw sfvbError('E_SFVB_SNAPSHOT_BASE');
        if (frame.dictionary.length) {
            const names = new Map();
            for (const [id, , name] of frame.dictionary) names.set(id, name);
            for (const pad of frame.pad_changes) { if (!names.has(pad.numeric_id)) throw sfvbError('E_SFVB_NUMERIC_ID'); pad.target = names.get(pad.numeric_id); }
            for (const comp of frame.components) { if (!names.has(comp.numeric_id)) throw sfvbError('E_SFVB_NUMERIC_ID'); comp.component = names.get(comp.numeric_id); }
            for (const tr of frame.trace_events) if (!names.has(tr.numeric_id)) throw sfvbError('E_SFVB_NUMERIC_ID');
        }
        return frame;
    }

    const decodeFrame = refDecoder
        ? (raw) => refDecoder.decodeFrame(typeof Buffer !== 'undefined' && !Buffer.isBuffer(raw) ? Buffer.from(raw.buffer, raw.byteOffset, raw.byteLength) : raw)
        : decodeFrameBrowser;

    function encodeFrame(f) {
        const chunks = [];
        const pushBytes = (arr) => chunks.push(arr instanceof Uint8Array ? arr : new Uint8Array(arr));
        const pushU8 = (v) => pushBytes([v & 0xff]);
        const pushU16 = (v) => { const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, Number(v), true); pushBytes(b); };
        const pushU32 = (v) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, Number(v), true); pushBytes(b); };
        const pushU64 = (v) => { const b = new Uint8Array(8); new DataView(b.buffer).setBigUint64(0, BigInt(v), true); pushBytes(b); };
        const pushStr = (s) => { const enc = encodeUtf8(String(s || '')); pushU16(enc.length); pushBytes(enc); };
        const pushBlob = (s) => { const enc = encodeUtf8(String(s || '')); pushU32(enc.length); pushBytes(enc); };
        const concat = (arrs) => {
            let total = 0; for (const a of arrs) total += a.length;
            const out = new Uint8Array(total); let at = 0;
            for (const a of arrs) { out.set(a, at); at += a.length; }
            return out;
        };

        const payloadChunks = [];
        let records = 0;
        const addRecord = (type, fn) => {
            const saved = chunks.length;
            fn();
            const body = concat(chunks.splice(saved));
            const recHead = new Uint8Array(6);
            const dv = new DataView(recHead.buffer);
            dv.setUint16(0, type, true);
            dv.setUint32(2, body.length, true);
            payloadChunks.push(recHead, body);
            records++;
        };

        addRecord(RT_INFO, () => {
            pushStr(f.schema || 'sigflow.virtual-frame.v1');
            pushStr(f.run_id || '');
            pushStr(f.state_encoding || 'sigflow.state13.v1');
            pushStr(f.state || 'Paused');
            pushStr(f.reason || '');
            pushStr(f.error || '');
            pushU64(f.tick_ps !== undefined ? f.tick_ps : 1n);
        });
        if (f.dictionary && f.dictionary.length) {
            addRecord(RT_DICTIONARY, () => {
                pushU32(f.dictionary.length);
                for (const [id, kb, name] of f.dictionary) { pushU32(id); pushU8(kb); pushStr(name); }
            });
        }
        addRecord(RT_ACKS, () => {
            const acks = f.acks || [];
            pushU32(acks.length);
            for (const a of acks) {
                pushStr(a.command_id); pushU8(ackStatusCode(a.status));
                pushU64(a.applied_tick); pushU32(a.applied_delta || 0); pushU64(a.applied_order || 0n);
            }
        });
        addRecord(RT_PAD_CHANGES, () => {
            const pads = f.pad_changes || [];
            pushU32(pads.length);
            for (const p of pads) {
                pushU32(p.numeric_id); pushU8(p.state_code);
                pushU8((p.logic || 'x').charCodeAt(0));
                pushU8((p.valid ? 1 : 0) | (p.contention ? 2 : 0));
            }
        });
        addRecord(RT_COMPONENTS, () => {
            const comps = f.components || [];
            pushU32(comps.length);
            for (const c of comps) { pushU32(c.numeric_id); pushBlob(JSON.stringify(c.state)); }
        });
        addRecord(RT_TRACE_CURSOR, () => {
            const tr = f.trace || { revision: 0n, committed_through_tick: 0n, committed_event_seq: 0n, cursor: '' };
            pushU64(tr.revision || 0n); pushU64(tr.committed_through_tick || 0n);
            pushU64(tr.committed_event_seq || 0n); pushStr(tr.cursor || '');
        });
        addRecord(RT_TRACE_EVENTS, () => {
            const tes = f.trace_events || [];
            pushU32(tes.length);
            for (const t of tes) {
                pushU32(t.numeric_id); pushU64(t.tick); pushU32(t.delta || 0); pushU64(t.event_seq);
                pushU8((t.value || 'x').charCodeAt(0)); pushU8(t.state_code || 0);
                pushU8((t.valid ? 1 : 0) | (t.contention ? 2 : 0) | (t.has_state_code ? 4 : 0));
            }
        });
        addRecord(RT_EVENTS, () => {
            const evs = f.events || [];
            pushU32(evs.length);
            for (const e of evs) { pushStr(e.type); pushBlob(JSON.stringify(e.detail)); }
        });
        addRecord(RT_DIAGNOSTICS, () => {
            pushBlob(JSON.stringify(f.diagnostics || []));
        });

        const payload = concat(payloadChunks);
        const header = new Uint8Array(HEADER);
        header[0] = 0x53; header[1] = 0x46; header[2] = 0x56; header[3] = 0x42;
        const hv = new DataView(header.buffer);
        hv.setUint16(4, 1, true);
        hv.setUint16(6, Number(f.kind), true);
        hv.setUint32(8, Number(f.flags || 0), true);
        hv.setUint32(12, HEADER, true);
        hv.setUint32(16, payload.length, true);
        hv.setUint32(20, Number(f.run_handle || 0), true);
        hv.setUint32(24, Number(f.generation || 0), true);
        hv.setBigUint64(28, BigInt(f.seq), true);
        hv.setBigUint64(36, BigInt(f.base_seq || 0n), true);
        hv.setBigUint64(44, BigInt(f.tick || 0n), true);
        hv.setUint32(52, Number(f.delta || 0), true);
        hv.setUint32(56, records, true);
        hv.setUint32(60, 0, true);
        const out = concat([header, payload]);
        return typeof Buffer !== 'undefined' ? Buffer.from(out.buffer, out.byteOffset, out.byteLength) : out;
    }

    function splitStreamFrames(rawStream) {
        const bytes = rawStream instanceof Uint8Array ? rawStream : new Uint8Array(rawStream);
        const slices = [];
        let at = 0;
        while (at + HEADER <= bytes.length) {
            const view = new DataView(bytes.buffer, bytes.byteOffset + at, bytes.length - at);
            const payload = view.getUint32(16, true);
            const total = HEADER + payload;
            if (at + total > bytes.length) throw sfvbError('E_SFVB_TRUNCATED');
            slices.push(bytes.subarray(at, at + total));
            at += total;
        }
        if (at !== bytes.length) throw sfvbError('E_SFVB_TRAILING_BYTES');
        return slices;
    }

    function decodeOledBitmap(compState) {
        const width = Number(compState.width || 64);
        const height = Number(compState.height || 32);
        const inverted = Boolean(compState.inverted);
        const rawBytes = new Uint8Array((width * height) / 8);
        const pixels = [];
        for (let y = 0; y < height; y++) pixels.push(new Array(width).fill(inverted ? 1 : 0));
        const pages = Array.isArray(compState.pages) ? compState.pages : [];
        let litCount = 0;
        for (let p = 0; p < pages.length && p * 8 < height; p++) {
            const pageHex = String(pages[p].bytes || '');
            for (let col = 0; col < width && (2 * col + 1) < pageHex.length; col++) {
                const byteVal = parseInt(pageHex.slice(2 * col, 2 * col + 2), 16) & 0xff;
                rawBytes[p * width + col] = byteVal;
                for (let bit = 0; bit < 8 && (p * 8 + bit) < height; bit++) {
                    const rawLit = ((byteVal >> bit) & 1) !== 0;
                    if (rawLit) litCount++;
                    pixels[p * 8 + bit][col] = (inverted ? !rawLit : rawLit) ? 1 : 0;
                }
            }
        }
        return { width, height, inverted, pixels, rawBytes, rawLitPixels: litCount };
    }

    const LED_PADS = [10, 11, 13, 14, 15, 16];
    const KEY_PADS = [3, 4];

    function buildWorkbenchView(snap) {
        const nodesObj = {};
        const sortedNodeNames = Object.keys(snap.nodes).sort();
        for (const name of sortedNodeNames) {
            const n = snap.nodes[name];
            nodesObj[name] = {
                state_code: Number(n.state_code),
                state_name: stateName(n.state_code),
                logic: String(n.logic),
                valid: Boolean(n.valid),
                contention: Boolean(n.contention),
                visual: classifyNode(n)
            };
        }
        const hasNodes = sortedNodeNames.length > 0;
        const leds = [];
        for (let i = 0; i < 6; i++) {
            const pad = LED_PADS[i];
            const ledKey = 'led' + i;
            const ledNodeName = ledKey + '.K';
            const comp = snap.components[ledKey] || null;
            const dutyPpm = comp && comp.integration ? Number(comp.integration.duty_ppm || 0) : 0;
            if (!hasNodes || snap.nodes[ledNodeName]) {
                const node = snap.nodes[ledNodeName] || { state_code: 0, logic: 'x', valid: false, contention: false };
                const code = Number(node.state_code || 0);
                const visual = classifyNode(node);
                const lit = Boolean(node.valid && !node.contention && node.logic === '0');
                leds.push({
                    index: i, id: ledKey, pad, mounted: true, multiplexed: false,
                    label: 'LED' + i + ' / P' + pad,
                    lit, duty_ppm: dutyPpm,
                    state_code: code, state_name: stateName(code),
                    logic: String(node.logic || 'x'), valid: Boolean(node.valid), contention: Boolean(node.contention),
                    visual
                });
            } else {
                const padNodeName = 'pad.' + pad;
                const padNode = snap.nodes[padNodeName] || null;
                if (padNode) {
                    const code = Number(padNode.state_code || 0);
                    leds.push({
                        index: i, id: ledKey, pad, mounted: false, multiplexed: true,
                        label: 'P' + pad + ' → 外接',
                        lit: false, duty_ppm: 0,
                        state_code: code, state_name: stateName(code),
                        logic: String(padNode.logic || 'x'), valid: Boolean(padNode.valid), contention: Boolean(padNode.contention),
                        visual: classifyNode(padNode)
                    });
                } else {
                    leds.push({
                        index: i, id: ledKey, pad, mounted: false, multiplexed: false,
                        label: 'P' + pad + ' / 未挂载',
                        lit: false, duty_ppm: 0,
                        state_code: null, state_name: '未挂载',
                        logic: 'z', valid: false, contention: false,
                        visual: 'unmounted'
                    });
                }
            }
        }
        const keys = [];
        for (const pad of KEY_PADS) {
            const keyNodeName = 'key_p' + pad + '.contact';
            if (!hasNodes || snap.nodes[keyNodeName]) {
                const node = snap.nodes[keyNodeName] || { state_code: 7, logic: '1', valid: true, contention: false };
                const code = Number(node.state_code || 0);
                const visual = classifyNode(node);
                keys.push({
                    pad, id: 'key_p' + pad, mounted: true, label: 'KEY P' + pad,
                    pressed: visual === 'low', state_code: code, state_name: stateName(code),
                    visual, status_text: visual === 'contention' ? 'CON' : visual === 'floating' ? 'FLT' : (visual === 'low' ? '按下' : '松开')
                });
            } else {
                keys.push({
                    pad, id: 'key_p' + pad, mounted: false, label: 'KEY P' + pad,
                    pressed: false, state_code: null, state_name: '未挂载',
                    visual: 'unmounted', status_text: '未挂载'
                });
            }
        }
        const seven_seg = {}, dips = {}, uarts = {}, keypads = {}, oleds = {}, i2cs = {};
        for (const id of Object.keys(snap.components).sort()) {
            const c = snap.components[id];
            if (!c || typeof c !== 'object') continue;
            const model = c.model || '';
            if (model === 'sigflow.seven-segment-mux@1') seven_seg[id] = cloneJson(c);
            else if (model === 'sigflow.dip-switch@1') dips[id] = cloneJson(c);
            else if (model === 'sigflow.matrix-keypad@1') keypads[id] = cloneJson(c);
            else if (model === 'sigflow.spi-oled@1') {
                const bmp = decodeOledBitmap(c);
                oleds[id] = Object.assign(cloneJson(c), {
                    pixels: bmp.pixels,
                    raw_lit_pixels: bmp.rawLitPixels
                });
            } else if (model === 'sigflow.i2c-sensor@1') {
                const addrHex = '0x' + Number(c.address_7bit || 0).toString(16).padStart(2, '0');
                i2cs[id] = Object.assign(cloneJson(c), { address_hex: addrHex });
            } else if (!model && ('received' in c || 'transmitted' in c)) {
                uarts[id] = cloneJson(c);
            }
        }
        return {
            schema: 'sigflow.workbench-view.v1',
            run_id: snap.run_id,
            run_handle: snap.run_handle,
            generation: snap.generation,
            seq: snap.seq,
            base_seq: snap.base_seq,
            tick: snap.tick,
            tick_ps: snap.tick_ps,
            delta: snap.delta,
            kind: snap.kind,
            state: snap.state,
            reason: snap.reason,
            error: snap.error,
            state_encoding: snap.state_encoding,
            leds, keys, seven_seg, dips, uarts, keypads, oleds, i2cs,
            nodes: nodesObj,
            trace: cloneJsonWithBigInt(snap.trace),
            acks: snap.acks.map(a => Object.assign({}, a)),
            frame_acks: snap.frame_acks.map(a => Object.assign({}, a)),
            events: cloneJson(snap.events),
            diagnostics: cloneJson(snap.diagnostics),
            stream_health: Object.assign({}, snap.stream_health)
        };
    }

    function cloneJsonWithBigInt(value) {
        if (typeof value === 'bigint') return value;
        if (Array.isArray(value)) return value.map(cloneJsonWithBigInt);
        if (value && typeof value === 'object') {
            const out = {};
            for (const k of Object.keys(value)) out[k] = cloneJsonWithBigInt(value[k]);
            return out;
        }
        return value;
    }

    function serializeValue(value) {
        if (typeof value === 'bigint') return value.toString();
        if (Array.isArray(value)) return value.map(serializeValue);
        if (value && typeof value === 'object') {
            const out = {};
            for (const k of Object.keys(value).sort()) out[k] = serializeValue(value[k]);
            return out;
        }
        return value;
    }

    function serializeView(view) {
        return serializeValue(view);
    }

    class SfvbClientWorkbench {
        constructor() {
            this.reset();
        }

        reset() {
            this.hasBaseline = false;
            this.needFullSnapshot = false;
            this.runHandle = 0;
            this.generation = 0;
            this.lastSeq = 0n;
            this.currentRunId = '';
            this.dictionary = new Map();
            this.signalNames = new Map();
            this.nodes = {};
            this.components = {};
            this.traceCursor = { revision: 0n, committed_through_tick: 0n, committed_event_seq: 0n, cursor: '' };
            this.traceEdges = [];
            this.seenTraceSeqs = new Set();
            this.ackLedger = new Map();
            this.appliedAcks = [];
            this.resyncCount = 0;
            this.staleDiscarded = 0;
            this.deduplicatedAcks = 0;
            this.snapshots = new Map();
            this.seqs = [];
        }

        feedBytes(bytes) {
            let frame;
            try {
                frame = decodeFrame(bytes);
            } catch (err) {
                return { status: 'FormatError', frame: null, error: err && err.sfvb ? err.message : 'E_SFVB_INTERNAL' };
            }
            return this.feedFrame(frame);
        }

        feedStream(rawStream) {
            const slices = splitStreamFrames(rawStream);
            const results = [];
            for (const slice of slices) {
                const res = this.feedBytes(slice);
                results.push(res);
                if (res.status !== 'Ok') return { status: res.status, error: res.error, results };
            }
            return { status: 'Ok', error: '', results, count: results.length };
        }

        feedFrame(rawFrame) {
            const frame = cloneJsonWithBigInt(rawFrame);
            if (!this.hasBaseline) {
                if (frame.kind !== 1) {
                    this.needFullSnapshot = true;
                    return { status: 'NeedFullSnapshot', frame, error: 'E_SFVB_BASELINE_MISSING', needFullSnapshot: true };
                }
                if (frame.base_seq !== 0n || !frame.dictionary || frame.dictionary.length === 0) {
                    return { status: 'FormatError', frame: null, error: 'E_SFVB_SNAPSHOT_BASE' };
                }
                this.runHandle = frame.run_handle;
                this.generation = frame.generation;
                this.hasBaseline = true;
                this.needFullSnapshot = false;
                return this._commitSnapshot(frame, false);
            }

            if (frame.run_handle !== this.runHandle || frame.generation !== this.generation) {
                this.staleDiscarded++;
                return { status: 'StaleGeneration', frame, error: 'E_SFVB_GENERATION_STALE', discarded: true };
            }

            if (this.needFullSnapshot) {
                if (frame.kind !== 1) {
                    return { status: 'NeedFullSnapshot', frame, error: 'E_SFVB_BASELINE_LOST', needFullSnapshot: true };
                }
                if (frame.base_seq !== 0n || !frame.dictionary || frame.dictionary.length === 0) {
                    return { status: 'FormatError', frame: null, error: 'E_SFVB_SNAPSHOT_BASE' };
                }
                if (frame.seq <= this.lastSeq) {
                    this.staleDiscarded++;
                    return { status: 'StaleGeneration', frame, error: 'E_SFVB_SEQ_STALE', discarded: true };
                }
                this.needFullSnapshot = false;
                this.resyncCount++;
                return this._commitSnapshot(frame, true);
            }

            if (frame.seq !== this.lastSeq + 1n) {
                this.needFullSnapshot = true;
                return { status: 'NeedFullSnapshot', frame, error: 'E_SFVB_SEQ_GAP', needFullSnapshot: true };
            }

            if (frame.kind === 1) {
                if (frame.base_seq !== 0n || !frame.dictionary || frame.dictionary.length === 0) {
                    return { status: 'FormatError', frame: null, error: 'E_SFVB_SNAPSHOT_BASE' };
                }
                return this._commitSnapshot(frame, false);
            }

            if (frame.base_seq !== this.lastSeq) {
                this.needFullSnapshot = true;
                return { status: 'NeedFullSnapshot', frame, error: 'E_SFVB_BASELINE_LOST', needFullSnapshot: true };
            }

            return this._commitDelta(frame);
        }

        _commitSnapshot(frame, recovered) {
            const sameRun = this.currentRunId === frame.run_id && this.currentRunId !== '';
            this.currentRunId = frame.run_id;
            this.dictionary = new Map();
            this.signalNames = new Map();
            for (const [id, kb, name] of frame.dictionary) {
                this.dictionary.set(id, { kindByte: kb, name });
                if (kb === 0) this.signalNames.set(id, name);
            }
            this.nodes = {};
            this.components = {};
            if (!sameRun) {
                this.traceEdges = [];
                this.seenTraceSeqs = new Set();
                this.ackLedger = new Map();
                this.appliedAcks = [];
            }
            this._resolveAndMerge(frame);
            this.lastSeq = frame.seq;
            this._storeMaterialized(frame);
            return { status: 'Ok', frame, error: '', recovered };
        }

        _commitDelta(frame) {
            this._resolveAndMerge(frame);
            this.lastSeq = frame.seq;
            this._storeMaterialized(frame);
            return { status: 'Ok', frame, error: '', recovered: false };
        }

        _resolveAndMerge(frame) {
            for (const pad of (frame.pad_changes || [])) {
                const entry = this.dictionary.get(pad.numeric_id);
                if (!entry) throw sfvbError('E_SFVB_NUMERIC_ID');
                pad.target = entry.name;
                this.nodes[entry.name] = {
                    state_code: pad.state_code,
                    logic: pad.logic,
                    valid: Boolean(pad.valid),
                    contention: Boolean(pad.contention)
                };
            }
            for (const comp of (frame.components || [])) {
                const entry = this.dictionary.get(comp.numeric_id);
                if (!entry) throw sfvbError('E_SFVB_NUMERIC_ID');
                comp.component = entry.name;
                this.components[entry.name] = cloneJson(comp.state);
            }
            if (frame.trace) {
                this.traceCursor = {
                    revision: frame.trace.revision,
                    committed_through_tick: frame.trace.committed_through_tick,
                    committed_event_seq: frame.trace.committed_event_seq,
                    cursor: frame.trace.cursor
                };
            }
            for (const te of (frame.trace_events || [])) {
                const entry = this.dictionary.get(te.numeric_id);
                if (!entry) throw sfvbError('E_SFVB_NUMERIC_ID');
                if (!this.signalNames.has(te.numeric_id)) this.signalNames.set(te.numeric_id, entry.name);
                if (this.seenTraceSeqs.has(te.event_seq)) continue;
                this.seenTraceSeqs.add(te.event_seq);
                this.traceEdges.push({
                    signal_id: te.numeric_id,
                    signal_name: entry.name,
                    tick: te.tick,
                    delta: te.delta,
                    event_seq: te.event_seq,
                    value: te.value,
                    state_code: te.has_state_code ? te.state_code : null,
                    state_name: te.has_state_code ? stateName(te.state_code) : null,
                    valid: Boolean(te.valid),
                    contention: Boolean(te.contention),
                    has_state_code: Boolean(te.has_state_code)
                });
            }
            for (const ack of (frame.acks || [])) {
                const prev = this.ackLedger.get(ack.command_id);
                if (prev && prev.status === ack.status && prev.applied_tick === ack.applied_tick &&
                    prev.applied_delta === ack.applied_delta && prev.applied_order === ack.applied_order) {
                    this.deduplicatedAcks++;
                    continue;
                }
                const copy = Object.assign({}, ack);
                this.ackLedger.set(ack.command_id, copy);
                if (prev) {
                    const idx = this.appliedAcks.findIndex(x => x.command_id === ack.command_id);
                    if (idx >= 0) this.appliedAcks[idx] = copy;
                    else this.appliedAcks.push(copy);
                } else {
                    this.appliedAcks.push(copy);
                }
            }
        }

        _storeMaterialized(frame) {
            const key = frame.seq.toString();
            if (!this.snapshots.has(key)) this.seqs.push(frame.seq);
            const signals = [];
            for (const [id, name] of Array.from(this.signalNames.entries()).sort((a, b) => a[0] - b[0])) {
                signals.push({ id, name });
            }
            const snap = {
                run_id: frame.run_id,
                run_handle: BigInt(frame.run_handle),
                generation: BigInt(frame.generation),
                seq: frame.seq,
                base_seq: frame.base_seq,
                tick: frame.tick,
                tick_ps: frame.tick_ps,
                delta: frame.delta,
                kind: kindName(frame.kind),
                state: frame.state,
                reason: frame.reason,
                error: frame.error,
                state_encoding: frame.state_encoding,
                nodes: cloneJson(this.nodes),
                components: cloneJson(this.components),
                trace: {
                    revision: this.traceCursor.revision,
                    committed_through_tick: this.traceCursor.committed_through_tick,
                    committed_event_seq: this.traceCursor.committed_event_seq,
                    cursor: this.traceCursor.cursor,
                    signals,
                    edges: this.traceEdges.map(e => Object.assign({}, e))
                },
                acks: this.appliedAcks.map(a => Object.assign({}, a)),
                frame_acks: (frame.acks || []).map(a => Object.assign({}, a)),
                events: cloneJson(frame.events || []),
                diagnostics: cloneJson(frame.diagnostics || []),
                stream_health: {
                    has_baseline: this.hasBaseline,
                    need_full_snapshot: this.needFullSnapshot,
                    resync_count: this.resyncCount,
                    stale_discarded: this.staleDiscarded,
                    deduplicated_acks: this.deduplicatedAcks,
                    total_frames: this.seqs.length
                }
            };
            this.snapshots.set(key, snap);
        }

        frameCount() { return this.seqs.length; }

        seqAt(index) {
            if (index < 0 || index >= this.seqs.length) return null;
            return this.seqs[index];
        }

        projectWorkbenchView(seq) {
            if (this.seqs.length === 0) return null;
            const targetSeq = (seq === undefined || seq === null) ? this.seqs[this.seqs.length - 1] : BigInt(seq);
            const snap = this.snapshots.get(targetSeq.toString());
            if (!snap) return null;
            return buildWorkbenchView(snap);
        }
    }

    function runSelfTest(i2cBurstPath, blindMixedPath, playerHtmlPath, outPath) {
        const assertions = [];
        const assert = (cond, msg) => {
            if (!cond) throw new Error('SELFTEST_FAIL: ' + msg);
            assertions.push(msg);
        };

        // 1. Real stream resilience: seq gap -> NeedFullSnapshot -> Delta rejected -> Recovery Snapshot -> Delta resumes.
        const burstBytes = fs.readFileSync(i2cBurstPath);
        const burstSlices = splitStreamFrames(burstBytes);
        assert(burstSlices.length >= 4, 'i2c burst stream has at least 4 SFVB frames');
        const resyncWb = new SfvbClientWorkbench();
        assert(resyncWb.feedBytes(burstSlices[1]).error === 'E_SFVB_BASELINE_MISSING', 'delta before baseline returns E_SFVB_BASELINE_MISSING');
        assert(resyncWb.feedBytes(burstSlices[0]).status === 'Ok', 'snapshot 1 accepted');
        const gapRes = resyncWb.feedBytes(burstSlices[2]);
        assert(gapRes.status === 'NeedFullSnapshot' && gapRes.error === 'E_SFVB_SEQ_GAP', 'sequence gap triggers E_SFVB_SEQ_GAP + NeedFullSnapshot');
        const deltaWhileLost = resyncWb.feedBytes(burstSlices[2]);
        assert(deltaWhileLost.status === 'NeedFullSnapshot' && deltaWhileLost.error === 'E_SFVB_BASELINE_LOST', 'delta while baseline lost stays blocked until full snapshot');
        const recoveryFrame = decodeFrame(burstSlices[2]);
        const f0 = decodeFrame(burstSlices[0]);
        // Construct an authoritative recovery full snapshot at seq 3 (matching the real trajectory state after frame 2)
        const preWb = new SfvbClientWorkbench();
        preWb.feedBytes(burstSlices[0]); preWb.feedBytes(burstSlices[1]); preWb.feedBytes(burstSlices[2]);
        const preView = preWb.projectWorkbenchView();
        const recDict = Array.from(preWb.dictionary.entries()).map(([id, info]) => [id, info.kindByte, info.name]);
        const nameToId = new Map(recDict.map(([id, , name]) => [name, id]));
        const recPads = Object.keys(preWb.nodes).map(n => Object.assign({ numeric_id: nameToId.get(n) }, preWb.nodes[n]));
        const recComps = Object.keys(preWb.components).map(c => ({ numeric_id: nameToId.get(c), state: preWb.components[c] }));
        const recBytes = encodeFrame({
            kind: 1, run_handle: f0.run_handle, generation: f0.generation,
            seq: recoveryFrame.seq, base_seq: 0n, tick: recoveryFrame.tick, delta: recoveryFrame.delta,
            schema: 'sigflow.virtual-frame.v1', run_id: recoveryFrame.run_id, state_encoding: recoveryFrame.state_encoding,
            state: recoveryFrame.state, reason: recoveryFrame.reason, error: recoveryFrame.error, tick_ps: 1n,
            dictionary: recDict, acks: recoveryFrame.acks, pad_changes: recPads, components: recComps,
            trace: recoveryFrame.trace, trace_events: [], events: [], diagnostics: []
        });
        const recRes = resyncWb.feedBytes(recBytes);
        assert(recRes.status === 'Ok' && recRes.recovered === true && resyncWb.resyncCount === 1, 'recovery snapshot re-establishes baseline and increments resyncCount');
        if (burstSlices.length > 3) {
            const afterRec = resyncWb.feedBytes(burstSlices[3]);
            assert(afterRec.status === 'Ok', 'delta stream continues seamlessly after recovery snapshot');
        }

        // 2. Tampered base_seq -> E_SFVB_BASELINE_LOST; stale generation -> E_SFVB_GENERATION_STALE discarded.
        const baseTamperWb = new SfvbClientWorkbench();
        baseTamperWb.feedBytes(burstSlices[0]);
        const d1 = decodeFrame(burstSlices[1]);
        d1.base_seq = d1.base_seq + 1n;
        const baseTamperRes = baseTamperWb.feedBytes(encodeFrame(d1));
        assert(baseTamperRes.status === 'NeedFullSnapshot' && baseTamperRes.error === 'E_SFVB_BASELINE_LOST', 'base_seq mismatch returns E_SFVB_BASELINE_LOST');

        const staleWb = new SfvbClientWorkbench();
        staleWb.feedBytes(burstSlices[0]);
        const staleF = decodeFrame(burstSlices[1]);
        staleF.generation = staleF.generation + 1;
        const staleRes = staleWb.feedBytes(encodeFrame(staleF));
        assert(staleRes.status === 'StaleGeneration' && staleRes.error === 'E_SFVB_GENERATION_STALE' && staleWb.staleDiscarded === 1, 'stale generation is discarded cleanly');
        assert(staleWb.feedBytes(burstSlices[1]).status === 'Ok', 'valid frame after stale discard still succeeds');

        // 3. Reconnect idempotent command_id ACK deduplication.
        const dedupWb = new SfvbClientWorkbench();
        dedupWb.feedBytes(burstSlices[0]);
        const f1Orig = decodeFrame(burstSlices[1]);
        const acked = { command_id: 'cmd-idempotent-1', status: 'applied', applied_tick: f1Orig.tick, applied_delta: 0, applied_order: 1n };
        const f1Ack = Object.assign({}, f1Orig, { acks: [acked], trace_events: [] });
        dedupWb.feedBytes(encodeFrame(f1Ack));
        const f2Dup = Object.assign({}, decodeFrame(burstSlices[2]), { acks: [acked], trace_events: [] });
        dedupWb.feedBytes(encodeFrame(f2Dup));
        const dedupView = dedupWb.projectWorkbenchView();
        assert(dedupWb.deduplicatedAcks === 1 && dedupView.acks.filter(a => a.command_id === 'cmd-idempotent-1').length === 1, 'duplicate command_id ACK across reconnect is idempotently deduplicated');

        // 4. 64-bit BigInt precision beyond Number.MAX_SAFE_INTEGER (2^53 - 1).
        const bigTick = 18446744073709551613n;
        const bigSeq = 9007199254740999n;
        const bigFrameBytes = encodeFrame({
            kind: 1, run_handle: 99, generation: 2, seq: bigSeq, base_seq: 0n, tick: bigTick, delta: 4,
            schema: 'sigflow.virtual-frame.v1', run_id: 'bigint-precision-run', state_encoding: 'sigflow.state13.v1',
            state: 'Paused', reason: 'StepComplete', error: '', tick_ps: 1n,
            dictionary: [[1, 1, 'pad.10'], [2, 2, 'led0']],
            acks: [{ command_id: 'cmd-big', status: 'applied', applied_tick: bigTick, applied_delta: 4, applied_order: bigSeq }],
            pad_changes: [{ numeric_id: 1, state_code: 13, logic: '0', valid: true, contention: false }],
            components: [{ numeric_id: 2, state: { lit: true, integration: { duty_ppm: 500000 } } }],
            trace: { revision: bigSeq, committed_through_tick: bigTick, committed_event_seq: bigSeq, cursor: 'chunk-big' },
            trace_events: [], events: [], diagnostics: []
        });
        const bigWb = new SfvbClientWorkbench();
        assert(bigWb.feedBytes(bigFrameBytes).status === 'Ok', '64-bit BigInt frame encodes and decodes');
        const bigSerialized = serializeView(bigWb.projectWorkbenchView(bigSeq));
        assert(bigSerialized.tick === '18446744073709551613' && bigSerialized.seq === '9007199254740999', 'BigInt preserves 64-bit tick and >2^53 seq losslessly');
        assert(bigSerialized.acks[0].applied_tick === '18446744073709551613' && bigSerialized.acks[0].applied_order === '9007199254740999', 'BigInt ACK timestamps/orders preserved');

        // 5. Blind stream projection leaks zero teacher secrets & player HTML script has zero hardcoded colors.
        const blindBytes = fs.readFileSync(blindMixedPath);
        const blindWb = new SfvbClientWorkbench();
        const blindRes = blindWb.feedStream(blindBytes);
        assert(blindRes.status === 'Ok', 'mixed teacher+blind stream feeds cleanly with mid-stream re-baseline');
        const blindViews = [];
        for (let i = 0; i < blindWb.frameCount(); i++) {
            const v = serializeView(blindWb.projectWorkbenchView(blindWb.seqAt(i)));
            if (String(v.run_id).startsWith('opaque-')) blindViews.push(v);
        }
        assert(blindViews.length >= 4, 'blind run contributes projected views in mixed stream');
        const blindBlob = JSON.stringify(blindViews);
        for (const secret of ['hidden-stuck', 'hidden-open', 'hidden-bounce', 'hidden-clock', 'hidden-bridge', 'secret-', 'rubric', '3100002', '9500123', 'fault_id']) {
            assert(!blindBlob.includes(secret), 'projected blind views leak zero ' + secret);
        }

        const html = fs.readFileSync(playerHtmlPath, 'utf8');
        const scriptMatches = Array.from(html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)).map(m => m[1]);
        const inlineScript = scriptMatches.join('\n');
        assert(inlineScript.length > 500, 'player HTML contains interactive script');
        assert(!/#[0-9a-fA-F]{3,8}\b/.test(inlineScript) && !/\brgba?\s*\(/i.test(inlineScript), 'player HTML script uses zero hardcoded hex or rgb() colors');
        for (const secret of ['hidden-', 'secret-', 'rubric', '9500123', '3100002']) {
            assert(!html.includes(secret), 'player HTML leaks zero ' + secret);
        }

        // 6. Verify OLED raw VRAM SHA-256 reconstruction capability if OLED present in stream
        for (let i = 0; i < resyncWb.frameCount(); i++) {
            const v = resyncWb.projectWorkbenchView(resyncWb.seqAt(i));
            for (const oled of Object.values(v.oleds)) {
                const bmp = decodeOledBitmap(oled);
                const digest = crypto.createHash('sha256').update(bmp.rawBytes).digest('hex');
                assert(digest === oled.frame_sha256, 'decoded OLED VRAM SHA-256 matches frame_sha256');
            }
        }

        const out = { status: 'passed', checks: assertions.length, assertions };
        fs.writeFileSync(outPath, JSON.stringify(out, null, 2) + '\n');
        process.stdout.write(JSON.stringify({ status: 'passed', checks: assertions.length }) + '\n');
    }

    if (typeof module === 'object' && module.exports && require.main === module) {
        const args = process.argv.slice(2);
        if (args[0] === '--self-test') {
            if (args.length < 5) {
                process.stderr.write('usage: sfvb_web_projector.js --self-test <p5-i2c.sfvb> <p12-mixed.sfvb> <player.html> <out.json>\n');
                process.exit(2);
            }
            runSelfTest(args[1], args[2], args[3], args[4]);
        } else {
            if (args.length < 2) {
                process.stderr.write('usage: sfvb_web_projector.js <stream.sfvb> <out.json>\n');
                process.exit(2);
            }
            const bytes = fs.readFileSync(args[0]);
            const wb = new SfvbClientWorkbench();
            const res = wb.feedStream(bytes);
            if (res.status !== 'Ok') {
                fs.writeFileSync(args[1], JSON.stringify({ status: 'rejected', error: res.error, frames: wb.frameCount() }, null, 2) + '\n');
                process.stderr.write(res.error + '\n');
                process.exit(1);
            }
            const views = [];
            for (let i = 0; i < wb.frameCount(); i++) {
                views.push(serializeView(wb.projectWorkbenchView(wb.seqAt(i))));
            }
            const out = {
                schema: 'sigflow.sfvb-workbench-stream.v1',
                status: 'passed',
                count: views.length,
                stream_health: views.length ? views[views.length - 1].stream_health : {},
                views
            };
            fs.writeFileSync(args[1], JSON.stringify(out, null, 2) + '\n');
            process.stdout.write(JSON.stringify({ status: 'passed', frames: views.length }) + '\n');
        }
    }

    return {
        HEADER,
        stateName,
        classifyNode,
        decodeFrame,
        encodeFrame,
        splitStreamFrames,
        decodeOledBitmap,
        serializeView,
        SfvbClientWorkbench
    };
}));
