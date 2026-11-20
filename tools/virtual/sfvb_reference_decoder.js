#!/usr/bin/env node
// SigFlow SFVB v1 reference decoder (planning book §4.4).
// Node.js standard library only: DataView + BigInt for 64-bit tick/seq,
// zero Number precision loss. Emits the same canonical frame JSON as the
// C++ BinaryFrameCodec decoder so cross-language parity is byte-comparable.
'use strict';
const fs=require('fs');
const RT_INFO=1,RT_DICTIONARY=2,RT_ACKS=3,RT_PAD_CHANGES=4,RT_COMPONENTS=5,RT_TRACE_CURSOR=6,RT_TRACE_EVENTS=7,RT_EVENTS=8,RT_DIAGNOSTICS=9;
const HEADER=64;
function SfvbError(message){const e=new Error(message);e.sfvb=true;return e;}
class Reader{
    constructor(buffer,offset=0){this.buffer=buffer;this.view=new DataView(buffer.buffer,buffer.byteOffset,buffer.byteLength);this.at=offset;this.end=buffer.length;}
    need(n){if(this.end-this.at<n)throw SfvbError('E_SFVB_TRUNCATED');}
    u8(){this.need(1);return this.view.getUint8(this.at++);}
    u16(){this.need(2);const v=this.view.getUint16(this.at,true);this.at+=2;return v;}
    u32(){this.need(4);const v=this.view.getUint32(this.at,true);this.at+=4;return v;}
    u64(){this.need(8);const v=this.view.getBigUint64(this.at,true);this.at+=8;return v;}
    str(){const n=this.u16();this.need(n);const s=this.buffer.toString('utf8',this.at,this.at+n);this.at+=n;return s;}
    blob(){const n=this.u32();this.need(n);const s=this.buffer.toString('utf8',this.at,this.at+n);this.at+=n;return s;}
    done(){return this.at===this.end;}
}
function ackStatus(code){const map={1:'applied',2:'pending',3:'superseded',4:'blocked',5:'rejected',6:'observed',7:'cancelled'};if(!map[code])throw SfvbError('E_SFVB_STATUS');return map[code];}
function kindName(kind){if(kind===1)return 'snapshot';if(kind===2)return 'delta';if(kind===3)return 'trace';if(kind===4)return 'display';throw SfvbError('E_SFVB_KIND');}
function kindOf(name){if(name==='snapshot')return 1;if(name==='delta')return 2;if(name==='trace')return 3;if(name==='display')return 4;throw SfvbError('E_SFVB_KIND');}
function decodeFrame(bytes){
    if(bytes.length<HEADER)throw SfvbError('E_SFVB_HEADER');
    if(!(bytes[0]===0x53&&bytes[1]===0x46&&bytes[2]===0x56&&bytes[3]===0x42))throw SfvbError('E_SFVB_MAGIC');
    const r=new Reader(bytes,4);
    const version=r.u16(),kind=r.u16(),flags=r.u32(),headerBytes=r.u32();
    const payloadBytes=r.u32(),runHandle=r.u32(),generation=r.u32();
    const seq=r.u64(),baseSeq=r.u64(),tick=r.u64();
    const delta=r.u32(),recordCount=r.u32(),reserved=r.u32();
    if(version!==1)throw SfvbError('E_SFVB_VERSION');
    if(headerBytes!==HEADER||reserved!==0)throw SfvbError('E_SFVB_HEADER');
    if(payloadBytes!==bytes.length-HEADER||payloadBytes>64*1024*1024)throw SfvbError('E_SFVB_PAYLOAD');
    const frame={kind,flags,run_handle:runHandle,generation,seq,base_seq:baseSeq,tick,delta,
        schema:'',run_id:'',state_encoding:'',state:'',reason:'',error:'',tick_ps:0n,
        dictionary:[],acks:[],pad_changes:[],components:[],trace:{revision:0n,committed_through_tick:0n,committed_event_seq:0n,cursor:''},
        trace_events:[],events:[],diagnostics:[]};
    r.at=HEADER;
    let records=0;
    const next=()=>r.done()?0:r.u16();
    if(next()!==RT_INFO)throw SfvbError('E_SFVB_RECORD_ORDER');
    {const len=r.u32(),end=r.at+len;frame.schema=r.str();frame.run_id=r.str();frame.state_encoding=r.str();
        frame.state=r.str();frame.reason=r.str();frame.error=r.str();frame.tick_ps=r.u64();if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');}
    records++;
    if(frame.schema!=='sigflow.virtual-frame.v1')throw SfvbError('E_SFVB_SCHEMA');
    let type=next();
    if(type===RT_DICTIONARY){
        if(frame.kind!==1)throw SfvbError('E_SFVB_DELTA_DICTIONARY');
        const len=r.u32(),end=r.at+len,count=r.u32();
        for(let i=0;i<count;i++){const id=r.u32(),kindByte=r.u8(),name=r.str();frame.dictionary.push([id,kindByte,name]);}
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();
    }
    if(type===RT_ACKS){const len=r.u32(),end=r.at+len,count=r.u32();
        for(let i=0;i<count;i++){const commandId=r.str(),code=r.u8(),appliedTick=r.u64(),appliedDelta=r.u32(),appliedOrder=r.u64();
            frame.acks.push({command_id:commandId,status:ackStatus(code),applied_tick:appliedTick,applied_delta:appliedDelta,applied_order:appliedOrder});}
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type===RT_PAD_CHANGES){const len=r.u32(),end=r.at+len,count=r.u32();
        for(let i=0;i<count;i++){const id=r.u32(),stateCode=r.u8(),logic=r.u8(),flag=r.u8();
            frame.pad_changes.push({numeric_id:id,target:'',state_code:stateCode,logic:String.fromCharCode(logic),valid:!!(flag&1),contention:!!(flag&2)});}
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type===RT_COMPONENTS){const len=r.u32(),end=r.at+len,count=r.u32();
        for(let i=0;i<count;i++){const id=r.u32(),blob=r.blob();frame.components.push({numeric_id:id,component:'',state:JSON.parse(blob)});}
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type===RT_TRACE_CURSOR){const len=r.u32(),end=r.at+len;frame.trace.revision=r.u64();frame.trace.committed_through_tick=r.u64();
        frame.trace.committed_event_seq=r.u64();frame.trace.cursor=r.str();
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type===RT_TRACE_EVENTS){const len=r.u32(),end=r.at+len,count=r.u32();
        for(let i=0;i<count;i++){const id=r.u32(),tickv=r.u64(),dv=r.u32(),ev=r.u64(),value=r.u8(),stateCode=r.u8(),flag=r.u8();
            frame.trace_events.push({numeric_id:id,tick:tickv,event_seq:ev,delta:dv,value:String.fromCharCode(value),
                state_code:stateCode,valid:!!(flag&1),contention:!!(flag&2),has_state_code:!!(flag&4)});}
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type===RT_EVENTS){const len=r.u32(),end=r.at+len,count=r.u32();
        for(let i=0;i<count;i++){const name=r.str(),blob=r.blob();frame.events.push({type:name,detail:JSON.parse(blob)});}
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type===RT_DIAGNOSTICS){const len=r.u32(),end=r.at+len;frame.diagnostics=JSON.parse(r.blob());
        if(r.at!==end)throw SfvbError('E_SFVB_RECORD_SIZE');records++;type=next();}
    if(type!==0||!r.done())throw SfvbError('E_SFVB_RECORD_ORDER');
    if(records!==recordCount)throw SfvbError('E_SFVB_RECORD_COUNT');
    if(frame.kind===1&&frame.base_seq!==0n)throw SfvbError('E_SFVB_SNAPSHOT_BASE');
    if(frame.dictionary.length){
        const names=new Map();
        for(const [id,kindByte,name] of frame.dictionary)names.set(id,name);
        for(const pad of frame.pad_changes){if(!names.has(pad.numeric_id))throw SfvbError('E_SFVB_NUMERIC_ID');pad.target=names.get(pad.numeric_id);}
        for(const comp of frame.components){if(!names.has(comp.numeric_id))throw SfvbError('E_SFVB_NUMERIC_ID');comp.component=names.get(comp.numeric_id);}
        for(const trace of frame.trace_events)if(!names.has(trace.numeric_id))throw SfvbError('E_SFVB_NUMERIC_ID');
    }
    return frame;
}
class StreamDecoder{
    constructor(){this.hasBaseline=false;this.lastSeq=0n;this.runHandle=0;this.generation=0;this.dictionary=new Map();}
    feed(bytes){
        try{
            const frame=decodeFrame(bytes);
            if(!this.hasBaseline){
                if(frame.kind!==1)return {status:'NeedFullSnapshot',frame,error:'E_SFVB_BASELINE_MISSING'};
                if(frame.base_seq!==0n||frame.dictionary.length===0)throw SfvbError('E_SFVB_SNAPSHOT_BASE');
                this.runHandle=frame.run_handle;this.generation=frame.generation;this.dictionary=new Map();
                for(const [id,kindByte,name] of frame.dictionary)this.dictionary.set(id,name);
                this.hasBaseline=true;this.lastSeq=frame.seq;
                this.resolve(frame);
                return {status:'Ok',frame,error:''};
            }
            if(frame.run_handle!==this.runHandle||frame.generation!==this.generation)return {status:'StaleGeneration',frame,error:'E_SFVB_GENERATION_STALE'};
            if(frame.seq!==this.lastSeq+1n)return {status:'NeedFullSnapshot',frame,error:'E_SFVB_SEQ_GAP'};
            if(frame.kind===1){
                if(frame.base_seq!==0n||frame.dictionary.length===0)throw SfvbError('E_SFVB_SNAPSHOT_BASE');
                this.dictionary=new Map();
                for(const [id,kindByte,name] of frame.dictionary)this.dictionary.set(id,name);
            }else if(frame.base_seq!==this.lastSeq)return {status:'NeedFullSnapshot',frame,error:'E_SFVB_BASELINE_LOST'};
            this.resolve(frame);
            this.lastSeq=frame.seq;
            return {status:'Ok',frame,error:''};
        }catch(error){return {status:'FormatError',frame:null,error:error.sfvb?error.message:'E_SFVB_INTERNAL'};}
    }
    resolve(frame){
        for(const pad of frame.pad_changes){if(!this.dictionary.has(pad.numeric_id))throw SfvbError('E_SFVB_NUMERIC_ID');pad.target=this.dictionary.get(pad.numeric_id);}
        for(const comp of frame.components){if(!this.dictionary.has(comp.numeric_id))throw SfvbError('E_SFVB_NUMERIC_ID');comp.component=this.dictionary.get(comp.numeric_id);}
        for(const trace of frame.trace_events)if(!this.dictionary.has(trace.numeric_id))throw SfvbError('E_SFVB_NUMERIC_ID');
    }
}
function u64(value){return value.toString();}
function frameJson(frame){
    const dictionary=[];
    for(const [id,kindByte,name] of frame.dictionary)
        dictionary.push({kind:kindByte===0?'signal':kindByte===1?'node':'component',name,numeric_id:u64(BigInt(id))});
    const out={
        acks:frame.acks.map(a=>({applied_delta:a.applied_delta,applied_order:u64(a.applied_order),applied_tick:u64(a.applied_tick),command_id:a.command_id,status:a.status})),
        base_seq:u64(frame.base_seq),committed_delta:frame.delta,committed_tick:u64(frame.tick),
        components:frame.components.map(c=>({id:c.component,numeric_id:u64(BigInt(c.numeric_id)),state:c.state})),
        diagnostics:frame.diagnostics,error:frame.error,
        events:frame.events.map(e=>({detail:e.detail,type:e.type})),
        generation:u64(BigInt(frame.generation)),kind:kindName(frame.kind),run_handle:u64(BigInt(frame.run_handle)),
        run_id:frame.run_id,schema:frame.schema,seq:u64(frame.seq),state:frame.state,state_encoding:frame.state_encoding,
        tick_ps:u64(frame.tick_ps),
        trace_events:frame.trace_events.map(t=>({contention:t.contention,delta:t.delta,event_seq:u64(t.event_seq),
            has_state_code:t.has_state_code,numeric_id:u64(BigInt(t.numeric_id)),state_code:t.has_state_code?t.state_code:null,
            tick:u64(t.tick),valid:t.valid,value:t.value})),
        trace:{committed_event_seq:u64(frame.trace.committed_event_seq),committed_through_tick:u64(frame.trace.committed_through_tick),
            cursor:frame.trace.cursor,revision:u64(frame.trace.revision)},
        pad_changes:frame.pad_changes.map(p=>({contention:p.contention,logic:p.logic,numeric_id:u64(BigInt(p.numeric_id)),
            state_code:p.state_code,target:p.target,valid:p.valid})),
        reason:frame.reason
    };
    if(dictionary.length)out.dictionary=dictionary;
    return out;
}
function sorted(value){
    if(Array.isArray(value))return value.map(sorted);
    if(value&&typeof value==='object'){
        const out={};for(const key of Object.keys(value).sort())out[key]=sorted(value[key]);return out;
    }
    return value;
}
function main(){
    const args=process.argv.slice(2);
    if(args.length<2){process.stderr.write('usage: sfvb_reference_decoder.js <stream.sfvb> <out.json>\n');process.exit(2);}
    const bytes=fs.readFileSync(args[0]);
    const decoder=new StreamDecoder();
    const frames=[];let at=0,decoded=0;
    while(at+HEADER<=bytes.length){
        const view=new DataView(bytes.buffer,bytes.byteOffset+at,bytes.length-at);
        const payload=view.getUint32(16,true);
        const slice=bytes.subarray(at,at+HEADER+payload);
        const result=decoder.feed(slice);
        if(result.status!=='Ok'){
            fs.writeFileSync(args[1],JSON.stringify({status:'rejected',error:result.error,decoded_frames:frames.length},null,2)+'\n');
            process.stderr.write(result.error+'\n');process.exit(1);
        }
        frames.push(frameJson(result.frame));decoded++;at+=HEADER+payload;
    }
    if(at!==bytes.length)throw SfvbError('E_SFVB_TRAILING_BYTES');
    const out={schema:'sigflow.sfvb-parity.v1',count:frames.length,frames};
    fs.writeFileSync(args[1],JSON.stringify(sorted(out),null,2)+'\n');
    process.stdout.write(JSON.stringify({status:'passed',frames:frames.length})+'\n');
}
if(require.main===module)main();
module.exports={decodeFrame,StreamDecoder,frameJson,HEADER};
