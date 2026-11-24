#include "midi_core.h"

static MidiU32 be16(const MidiU8 *p) { return ((MidiU32)p[0]<<8)|p[1]; }
static MidiU32 be32(const MidiU8 *p) {
 return ((MidiU32)p[0]<<24)|((MidiU32)p[1]<<16)|((MidiU32)p[2]<<8)|p[3];
}
static int variable(const MidiU8 *data, MidiU32 end, MidiU32 *pos, MidiU32 *value) {
 MidiU32 v=0;
 for(unsigned i=0;i<4;++i) {
  if(*pos>=end) return 0;
  MidiU32 b=data[(*pos)++]; v=(v<<7)|(b&127u);
  if(!(b&128u)) { *value=v; return 1; }
 }
 return 0;
}
static int less(const MidiEvent &a,const MidiEvent &b) {
 if(a.tick!=b.tick) return a.tick<b.tick;
 return a.order<b.order;
}
static void swap_ev(MidiEvent *a,MidiEvent *b){MidiEvent t=*a;*a=*b;*b=t;}
static void sift(MidiEvent *v,MidiU32 root,MidiU32 n) {
 for(;;) {
  MidiU32 child=root*2u+1u;
  if(child>=n) return;
  if(child+1u<n && less(v[child],v[child+1])) ++child;
  if(!less(v[root],v[child])) return;
  swap_ev(&v[root],&v[child]); root=child;
 }
}
static void sort_events(MidiEvent *v,MidiU32 n) {
 for(MidiU32 i=n/2;i>0;) {--i;sift(v,i,n);}
 for(MidiU32 i=n;i>1;) {--i;swap_ev(&v[0],&v[i]);sift(v,0,i);}
}

// 32 x 32 -> 64 without C++ 64-bit runtime helpers.
static void multiply(MidiU32 a,MidiU32 b,MidiU32 *hi,MidiU32 *lo) {
 MidiU32 a0=a&65535u,a1=a>>16,b0=b&65535u,b1=b>>16;
 MidiU32 p0=a0*b0, p1=a1*b0, p2=a0*b1, p3=a1*b1;
 MidiU32 mid=(p0>>16)+(p1&65535u)+(p2&65535u);
 *lo=(p0&65535u)|(mid<<16);
 *hi=p3+(p1>>16)+(p2>>16)+(mid>>16);
}
// Divides a 64-bit numerator by 1..32767. High nonzero quotient => overflow.
static int div64(MidiU32 hi,MidiU32 lo,MidiU32 denominator,
                 MidiU32 *quotient,MidiU32 *rem) {
 MidiU32 qlo=0, r=0;
 int overflow=0;
 for(int i=63;i>=0;--i) {
  MidiU32 bit=i>=32 ? ((hi>>(i-32))&1u) : ((lo>>i)&1u);
  r=(r<<1)|bit;
  if(r>=denominator) {
   r-=denominator;
   if(i>=32) overflow=1;
   else qlo|=(1u<<i);
  }
 }
 *quotient=qlo; *rem=r;
 return !overflow;
}
static int elapsed(MidiU32 delta,MidiU32 tempo,MidiU32 division,
                   MidiU32 *carry,MidiU32 *micros) {
 MidiU32 hi,lo,q,rem;
 multiply(delta,tempo,&hi,&lo);
 MidiU32 previous=lo;
 lo+=*carry;
 if(lo<previous) ++hi;
 if(!div64(hi,lo,division,&q,&rem) || q>0xffffffffu-*micros)
  return 0;
 *micros+=q; *carry=rem;
 return 1;
}
static int fail(MidiInfo *info,MidiU32 error) { info->error=error;return 0; }
int midi_parse(const MidiU8 *data,MidiU32 bytes,MidiEvent *out,
               MidiU32 capacity,MidiInfo *info) {
 info->count=info->notes=info->tracks=info->division=info->duration_us=0;
 info->error=MIDI_OK;
 if(bytes<14) return fail(info,MIDI_SHORT_HEADER);
 if(be32(data)!=0x4d546864u) return fail(info,MIDI_BAD_HEADER);
 MidiU32 header=be32(data+4);
 if(header<6 || header>bytes-8u) return fail(info,MIDI_BAD_HEADER);
 MidiU32 format=be16(data+8), ntracks=be16(data+10), ppqn=be16(data+12);
 if(format>1 || (format==0 && ntracks!=1)) return fail(info,MIDI_BAD_FORMAT);
 if(ppqn==0 || (ppqn&0x8000u)) return fail(info,MIDI_BAD_DIVISION);
 if(ntracks==0 || ntracks>MIDI_MAX_TRACKS) return fail(info,MIDI_TRACKS);
 info->tracks=ntracks;info->division=ppqn;
 MidiU32 pos=8+header,count=0,notes=0;
 for(MidiU32 tr=0;tr<ntracks;++tr) {
  if(pos>bytes || bytes-pos<8) return fail(info,MIDI_TRUNCATED);
  if(be32(data+pos)!=0x4d54726bu) return fail(info,MIDI_BAD_HEADER);
  MidiU32 sz=be32(data+pos+4);pos+=8;
  if(sz>bytes-pos) return fail(info,MIDI_TRUNCATED);
  MidiU32 cur=pos,end=pos+sz,absolute=0,running=0;
  while(cur<end) {
   MidiU32 delta;
   if(!variable(data,end,&cur,&delta) || delta>0xffffffffu-absolute)
    return fail(info,MIDI_BAD_EVENT);
   absolute+=delta;
   if(cur>=end) return fail(info,MIDI_TRUNCATED);
   MidiU32 status=data[cur];
   if(status&128u){++cur;running=status<0xf0u?status:0;}
   else if(!running)return fail(info,MIDI_BAD_EVENT);
   else status=running;
   if(status==0xffu) {
    if(cur>=end)return fail(info,MIDI_TRUNCATED);
    MidiU32 kind=data[cur++],length=0;
    if(!variable(data,end,&cur,&length)||length>end-cur)
     return fail(info,MIDI_TRUNCATED);
    if(kind==0x51u && length==3) {
     if(count>=capacity || count>=65535u)return fail(info,MIDI_EVENTS_FULL);
     MidiU32 tempo=((MidiU32)data[cur]<<16)|((MidiU32)data[cur+1]<<8)|data[cur+2];
     if(!tempo)return fail(info,MIDI_BAD_EVENT);
     MidiEvent *e=&out[count];e->tick=absolute;e->due_us=0;
     e->extra=tempo;e->order=(MidiU16)count;e->note=e->velocity=e->channel=0;
     e->kind=MIDI_TEMPO;++count;
    }
    cur+=length;
    if(kind==0x2fu)break;
    continue;
   }
   if(status==0xf0u || status==0xf7u) {
    MidiU32 length=0;
    if(!variable(data,end,&cur,&length)||length>end-cur)
     return fail(info,MIDI_TRUNCATED);
    cur+=length;continue;
   }
   if(status>=0xf0u)return fail(info,MIDI_BAD_EVENT);
   MidiU32 kind=status&0xf0u,need=(kind==0xc0u||kind==0xd0u)?1u:2u;
   if(need>end-cur)return fail(info,MIDI_TRUNCATED);
   MidiU32 a=data[cur++],b=need==2?data[cur++]:0;
   if(a>127 || b>127)return fail(info,MIDI_BAD_EVENT);
   if(kind==0x80u || kind==0x90u) {
    if(count>=capacity || count>=65535u)return fail(info,MIDI_EVENTS_FULL);
    MidiEvent *e=&out[count];e->tick=absolute;e->due_us=0;e->extra=0;
    e->order=(MidiU16)count;e->note=(MidiU8)a;e->velocity=(MidiU8)b;
    e->channel=(MidiU8)(status&15u);
    e->kind=(MidiU8)((kind==0x90u && b!=0)?MIDI_NOTE_ON:MIDI_NOTE_OFF);
    ++count;++notes;
   }
  }
  pos=end;
 }
 if(!notes)return fail(info,MIDI_NO_NOTES);
 sort_events(out,count);
 MidiU32 tempo=500000,last=0,us=0,carry=0;
 for(MidiU32 i=0;i<count;++i) {
  MidiEvent *e=&out[i];
  if(!elapsed(e->tick-last,tempo,ppqn,&carry,&us))
   return fail(info,MIDI_TIME_OVERFLOW);
  last=e->tick;
  e->due_us=us;
  if(e->kind==MIDI_TEMPO)tempo=e->extra;
 }
 info->count=count;info->notes=notes;info->duration_us=us;
 return 1;
}
