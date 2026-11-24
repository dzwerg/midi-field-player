// OP-1 Field open-field: MIDI Song Player 1.0 (SMF notes, multi-track and tempo map).
// See README for the limits of the Level-5 sequencer interface.
#include "build_r3_card.h"
#include "midi_core.h"

typedef unsigned int u32;
typedef unsigned char u8;
enum { FOLDER=0, ASK=0, FILES=64, NAME_BYTES=40, INPUT_BYTES=36, FRAME_BYTES=40 };
enum { STOPPED=0, PLAYING=1, PAUSED=2, FINISHED=3 };

extern "C" const Op1fCard op1f_card_midiplayer OP1F_NAMED("op1f_card_midiplayer");

static u8 data[MIDI_FILE_LIMIT];
static MidiEvent song[MIDI_MAX_EVENTS];
static MidiInfo info;
static char all_names[FILES * NAME_BYTES];
static char names[FILES][NAME_BYTES];
static char current[NAME_BYTES];
static char text[64];
static u32 file_count=0,index=0;
static volatile u32 mode=STOPPED, seq_enabled=0, loaded=0;
static volatile u32 stop_request=0, click_request=0;
static volatile int turn_total=0, transpose_total=0, speed_total=0;
static volatile int loop_turn_total=0;

static volatile u32 seen_click=0, seen_stop=0;
static int seen_turn=0, seen_transpose=0, seen_speed=0;
static int seen_loop=0;
static u32 cursor=0, elapsed_us=0, error=0;
// GUI-only precomputed note lengths; playback events remain unchanged.
static u32 roll_end_us[MIDI_MAX_EVENTS];
static MidiU16 roll_prev[MIDI_MAX_EVENTS];
static MidiU16 roll_top[16][128];
static u32 title_frames=0;
static u32 roll_low=48;
static u8 active[128];
static u8 held_pitch[16][128];
static u8 held_count[16][128];
static volatile u32 help_held=0;
static int transpose=0;
static volatile u32 speed_percent=100;
static volatile u32 loop_enabled=0;
static u32 started=0, ready=0, old_disk=0, old_page=0;
static u32 pending_refresh=1;

static void str_copy(char *to,const char *from,u32 room) {
 if(!room)return;
 u32 n=0;while(n+1<room && from[n]){to[n]=from[n];++n;}
 to[n]=0;
}
static int str_equal(const char *a,const char *b) {
 while(*a && *b){if(*a++!=*b++)return 0;}
 return *a==*b;
}
static int midi_name(const char *s) {
 u32 n=0;while(s[n] && n<NAME_BYTES)++n;
 if(n<5 || n>=NAME_BYTES || s[n-4]!='.')return 0;
 char a=s[n-3],b=s[n-2],c=s[n-1];
 if(a>='A'&&a<='Z')a+=32;
 if(b>='A'&&b<='Z')b+=32;
 if(c>='A'&&c<='Z')c+=32;
 return a=='m' && b=='i' && c=='d';
}
static u32 divide_u32(u32 value,u32 denominator,u32 *remainder) {
 u32 result=0,rem=0;
 for(int bit=31;bit>=0;--bit){
  rem=(rem<<1)|((value>>bit)&1u);
  if(rem>=denominator){rem-=denominator;result|=1u<<bit;}
 }
 if(remainder)*remainder=rem;
 return result;
}
static u32 put_number(char* out,u32 room,u32 value) {
 if(room<2)return 0;
 char digits[11];u32 n=0;
 do {
      u32 rem=0;
      u32 q=divide_u32(value,10,&rem);
      digits[n++]=(char)('0'+rem);value=q;
 }while(value && n<sizeof digits);
 u32 k=0;while(n && k+1<room)out[k++]=digits[--n];out[k]=0;return k;
}
static int clamped(int value,int lo,int hi){return value<lo?lo:value>hi?hi:value;}

static void scan() {
 // Do not read while a song is playing: the frame owns disk access.
 char previous[NAME_BYTES];str_copy(previous,current,sizeof previous);
 int got=op1f_list(&op1f_card_midiplayer,FOLDER,all_names,sizeof all_names);
 if(got<0){error=100u+(u32)(-got);file_count=0;return;}
 file_count=0;u32 at=0;
 for(int i=0;i<got && i<FILES;++i){
  if(at>=sizeof all_names)break;
  u32 n=0;while(at+n<sizeof all_names && n<NAME_BYTES && all_names[at+n])++n;
  if(n>=NAME_BYTES || at+n>=sizeof all_names)break;
  if(midi_name(&all_names[at]))str_copy(names[file_count++],&all_names[at],NAME_BYTES);
  at+=n+1;
 }
 index=0;
 for(u32 i=0;i<file_count;++i)if(str_equal(names[i],previous)){index=i;break;}
 error=file_count?0u:20u;
}
// Called only from frame after parsing, never from the sequencer tick.
// Each note-on is linked to its matching note-off (per channel and pitch).
static void prepare_roll() {
 for(u32 ch=0;ch<16;++ch)
  for(u32 n=0;n<128;++n)roll_top[ch][n]=0;
 for(u32 i=0;i<info.count;++i){
  roll_prev[i]=0;
  roll_end_us[i]=song[i].due_us;
  const MidiEvent* e=&song[i];
  if(e->kind==MIDI_NOTE_ON){
   roll_prev[i]=roll_top[e->channel][e->note];
   roll_top[e->channel][e->note]=(MidiU16)(i+1u);
  }else if(e->kind==MIDI_NOTE_OFF){
   MidiU16 top=roll_top[e->channel][e->note];
   if(top){
    u32 on=(u32)top-1u;
    roll_end_us[on]=e->due_us;
    roll_top[e->channel][e->note]=roll_prev[on];
   }
  }
 }
 // Unclosed notes are drawn as short bars, never infinitely long.
 for(u32 ch=0;ch<16;++ch)
  for(u32 n=0;n<128;++n){
   MidiU16 top=roll_top[ch][n];
   while(top){
    u32 on=(u32)top-1u;
    u32 start=song[on].due_us;
    roll_end_us[on]=(start<=0xffffffffu-200000u)?start+200000u:start;
    top=roll_prev[on];
   }
  }
}

static void load_song() {
 // Calls to op1f_read and midi_parse only happen from frame, when not playing.
 if(mode==PLAYING || mode==PAUSED)return;
 loaded=0;cursor=0;elapsed_us=0;
 if(!file_count){error=20;return;}
 str_copy(current,names[index],sizeof current);title_frames=0;
 int n=op1f_read(&op1f_card_midiplayer,FOLDER,current,data,sizeof data);
 if(n<0){error=30u+(u32)(-n);return;}
 MidiInfo result;
 if(!midi_parse(data,(u32)n,song,MIDI_MAX_EVENTS,&result)){
  error=40u+result.error;return;
 }
 info=result;error=0;loaded=1;title_frames=0;prepare_roll();
}
static void stop_active() {
 for(u32 i=0;i<128;++i){
  if(active[i]){
   active[i]=0;
   op1f_note_off(&op1f_card_midiplayer,i,0);
  }
 }
 for(u32 ch=0;ch<16;++ch)
  for(u32 n=0;n<128;++n){
   held_pitch[ch][n]=0;
   held_count[ch][n]=0;
  }
}
static u32 change_events() {
 u32 click=click_request,stop=stop_request;
 if(stop!=seen_stop){
  seen_stop=stop;seen_click=click;
  stop_active();mode=STOPPED;cursor=0;elapsed_us=0;return 1;
 }
 if(click!=seen_click){
  seen_click=click;
  if(mode==PLAYING){stop_active();mode=PAUSED;return 1;}
  if(!loaded)return 1;
  if(mode==STOPPED || mode==FINISHED){cursor=0;elapsed_us=0;stop_active();}
  mode=PLAYING;return 1;
 }
 return 0;
}
extern "C" void midiplayer_run(unsigned host,unsigned how) OP1F_NAMED("midiplayer_run");
extern "C" void midiplayer_run(unsigned host,unsigned how) {
 if(host!=0)return;
 u32 was=op1f_mask();
 seq_enabled=(how==(unsigned)OP1F_RUN_ON)?1u:0u;
 if(!seq_enabled){mode=STOPPED;cursor=0;elapsed_us=0;
  for(u32 i=0;i<128;++i)active[i]=0;
  for(u32 ch=0;ch<16;++ch)
   for(u32 n=0;n<128;++n){
    held_pitch[ch][n]=0;
    held_count[ch][n]=0;
   }
 }else{mode=STOPPED;cursor=0;elapsed_us=0;}
 seen_click=click_request;seen_stop=stop_request;
 op1f_unmask(was);
 // The open-field base ends the pack's sounding notes after RUN_STOP.
}
extern "C" void midiplayer_tick(const Op1fTick* now) OP1F_NAMED("midiplayer_tick");
extern "C" void midiplayer_tick(const Op1fTick* now) {
 if(now->bytes<32 || now->host!=0 || !seq_enabled)return;
 if(change_events()){
  if(mode!=PLAYING)return;
  // Starting or resuming: don't advance time until the next pulse.

 } else if(mode!=PLAYING)return;
 else {
  u32 delta=divide_u32(now->us*speed_percent,100u,0);
  if(now->us>=1000000u || delta>0xffffffffu-elapsed_us){
   stop_active();mode=FINISHED;return;
  }
  elapsed_us+=delta;
 }

 // A bounded dispatch limit prevents a pathological MIDI file blocking a tick.
 for(u32 n=0;n<64 && cursor<info.count;++n) {
  const MidiEvent* e=&song[cursor];
  if(e->due_us>elapsed_us)break;
  ++cursor;
  if(e->kind==MIDI_TEMPO)continue;
  if(e->note>127 || e->channel>15)continue;

  u32 ch=e->channel;
  u32 original=e->note;
  u32 p=0;

  if(e->kind==MIDI_NOTE_ON){
   int pitch=(int)original+transpose;
   if(pitch<0 || pitch>127)continue;

   p=(u32)pitch;

   if(held_count[ch][original]){
    // Keep the first note's pitch until its off.
    p=held_pitch[ch][original]-1u;
   }else{
    held_pitch[ch][original]=(u8)(p+1u);
   }

   if(held_count[ch][original]<255)
    ++held_count[ch][original];

   if(!active[p]){
    if(op1f_note_on(&op1f_card_midiplayer,
                    p,e->velocity,0))
     active[p]=1;
   }else if(active[p]<255){
    ++active[p];
   }
  }else{
   if(!held_count[ch][original])continue;

   p=held_pitch[ch][original]-1u;
   --held_count[ch][original];

   if(!held_count[ch][original])
    held_pitch[ch][original]=0;

   if(active[p] && --active[p]==0)
    op1f_note_off(&op1f_card_midiplayer,p,0);
  }
 }
 if(cursor>=info.count){
   stop_active();
   if(loop_enabled){
    cursor=0;
    elapsed_us=0;
   }else{
    mode=FINISHED;
   }
  }
}
extern "C" void midiplayer_shown(unsigned where,unsigned how) OP1F_NAMED("midiplayer_shown");
extern "C" void midiplayer_shown(unsigned where,unsigned how){(void)where;(void)how;}

extern "C" int midiplayer_input(const Op1fInput* e) OP1F_NAMED("midiplayer_input");
extern "C" int midiplayer_input(const Op1fInput* e){
 if(e->bytes<INPUT_BYTES || !e->mine)return 0;

 // HELP: press and hold.
 if(e->key==0u){
  if(e->what==(u32)OP1F_DOWN){
   help_held=1;
   return 1;
  }
  if(e->what==(u32)OP1F_UP){
   help_held=0;
   return 1;
  }
 }

 if(e->what==(u32)OP1F_TURN){
  if(e->key==0){turn_total+=e->turn>>10;return 1;}
  if(e->key==1){loop_turn_total+=e->turn>>10;return 1;}
  if(e->key==2){speed_total+=e->turn>>10;return 1;}
  if(e->key==3){transpose_total+=e->turn>>10;return 1;}
  return 0;
 }
 // Clicks 54 (blue) and 55 (second); the firmware generates a click on release.
 if(e->what==(u32)OP1F_DOWN){
  if(e->key==54){++click_request;return 1;}
  if(e->key==55){++stop_request;return 1;}
 }
 if(e->what==(u32)OP1F_UP && (e->key==54 || e->key==55))return 1;
 return 0;
}

static const char* error_name(u32 e){
 if(e==20)return "NO MIDI FILES";
 if(e>=30 && e<40)return e==32?"FILE TOO LARGE":"FILE READ ERROR";
 if(e==41 || e==42)return "BAD MIDI HEADER";
 if(e==43)return "MIDI FORMAT 2";
 if(e==44)return "SMPTE DIVISION";
 if(e==45)return "TOO MANY TRACKS";
 if(e==46)return "TRUNCATED MIDI";
 if(e==47)return "BAD MIDI EVENT";
 if(e==48)return "TOO MANY EVENTS";
 if(e==49)return "MIDI TOO LONG";
 if(e==50)return "NO NOTES";
 if(e>=100)return "FOLDER READ ERROR";
 return "MIDI FILE ERROR";
}
static void label(void*canvas,int x,int y,const char*txt,unsigned color,int scale){
 op1f_paint_text(canvas,x,y,txt,color,scale);
}
extern "C" void midiplayer_frame(const Op1fFrame* now) OP1F_NAMED("midiplayer_frame");
extern "C" void midiplayer_frame(const Op1fFrame* now){
 if(now->bytes<FRAME_BYTES)return;
 if(!ready){if(op1f_page_stand_in(&op1f_card_midiplayer,ASK,0))ready=1;}
 const u32 page=(now->page==&op1f_card_midiplayer)?1u:0u;
 if(!started){started=1;pending_refresh=1;}
 if(old_disk && !now->disk)pending_refresh=1;
 old_disk=now->disk?1u:0u;
 if(page && !old_page)pending_refresh=1;
 old_page=page;
 if(pending_refresh && !now->disk && mode!=PLAYING && mode!=PAUSED){
  pending_refresh=0;scan();load_song();
 }
 int change=turn_total-seen_turn;seen_turn=turn_total;
 int tr=transpose_total-seen_transpose;seen_transpose=transpose_total;
 int sp=speed_total-seen_speed;seen_speed=speed_total;
  int lp=loop_turn_total-seen_loop;
  seen_loop=loop_turn_total;
  if(lp<0)loop_enabled=0;
  if(lp>0)loop_enabled=1;
 if(sp)speed_percent=(u32)clamped((int)speed_percent+sp*5,50,200);
 if(tr)transpose=clamped(transpose+tr,-24,24);
 if(change && page && !now->disk && mode!=PLAYING && mode!=PAUSED && file_count){
  int next=clamped((int)index+change,0,(int)file_count-1);
  if(index!=(u32)next){index=(u32)next;load_song();}
 }


 if(!page || !now->canvas)return;
 void* c=now->canvas;
 const u32 BG=OP1F_COLOUR(20,27,35);
 const u32 PANEL=OP1F_COLOUR(29,39,50);
 const u32 GRID=OP1F_COLOUR(48,59,70);
 const u32 WHITE=OP1F_COLOUR(230,235,238);
 const u32 GREY=OP1F_COLOUR(145,157,169);
 const u32 BLUE=OP1F_COLOUR(105,174,235);
 const u32 ORANGE=OP1F_COLOUR(230,168,93);
 const u32 GREEN=OP1F_COLOUR(115,215,169);
 const u32 YELLOW=OP1F_COLOUR(241,205,106);
 const u32 RED=OP1F_COLOUR(232,126,136);
 op1f_paint_box(c,0,0,480,240,BG);

 if(help_held){
  label(c,16,12,"MIDI PLAYER - HELP",WHITE,2);
  op1f_paint_box(c,16,39,448,1,PANEL);
  label(c,16,52,"BLUE",BLUE,2);
  label(c,16,75,"TURN: SELECT MIDI FILE",WHITE,1);
  label(c,16,91,"PRESS: PLAY / PAUSE",WHITE,1);
  label(c,248,52,"ORANGE",ORANGE,2);
  label(c,248,75,"LEFT: ONCE",WHITE,1);
  label(c,248,91,"RIGHT: LOOP",WHITE,1);
  label(c,16,119,"WHITE",WHITE,2);
  label(c,16,142,"TURN: SPEED",WHITE,1);
  label(c,16,158,"PRESS: STOP / REWIND",WHITE,1);
  label(c,248,119,"RED",RED,2);
  label(c,248,142,"TURN: TRANSPOSE",WHITE,1);
  label(c,248,158,"LIVE PITCH: +/-24",WHITE,1);
  op1f_paint_box(c,16,192,448,1,PANEL);
  label(c,16,205,"RELEASE HELP TO RETURN",GREY,1);
  label(c,16,221,"PLAYBACK CONTINUES",GREEN,1);
  return;
 }

 // Fixed-size, fixed-width title window: 22 chars at scale 2.
 // Long filenames scroll in a seamless circular marquee.
 const char* title=file_count?current:"NO MIDI FILES";
 u32 name_len=0;
 while(title[name_len] && name_len<NAME_BYTES)++name_len;
 char title_line[24];
 const u32 visible=22;
 u32 offset=0;
 if(name_len>visible){
  // Four spaces provide the gap between repeated filename copies.
  u32 frame_step=title_frames; offset=divide_u32(frame_step,name_len+4u,&offset);
  if(title_frames>=0xfffffff0u)title_frames=0;
  ++title_frames;
 }
 for(u32 i=0;i<visible;++i){
  u32 at=offset+i;
  if(name_len>visible){
   u32 rem=0; divide_u32(at,name_len+4u,&rem); at=rem;
   title_line[i]=(at<name_len)?title[at]:' ';
  }else title_line[i]=(i<name_len)?title[i]:' ';
 }
 title_line[visible]=0;
 label(c,16,8,title_line,file_count?WHITE:ORANGE,2);

 // One transport status icon only.
 if(mode==PLAYING){
  for(int k=0;k<11;++k)
   op1f_paint_box(c,341+k,12+k/2,1,13-k,GREEN);
 }else if(mode==PAUSED){
  op1f_paint_box(c,341,14,4,13,YELLOW);
  op1f_paint_box(c,349,14,4,13,YELLOW);
 }else{
  op1f_paint_box(c,341,14,13,13,RED);
 }

 // Time (elapsed / total) takes the former redundant icon area.
 u32 sec=divide_u32(elapsed_us,1000000u,0);
 u32 total=loaded?divide_u32(info.duration_us,1000000u,0):0;
 char time_text[32];
 char num[12];u32 t=0;
 u32 n=put_number(num,sizeof num,sec);
 for(u32 i=0;i<n && t+1<sizeof time_text;++i)time_text[t++]=num[i];
 if(t+3<sizeof time_text){time_text[t++]=' ';time_text[t++]='/';time_text[t++]=' ';}
 n=put_number(num,sizeof num,total);
 for(u32 i=0;i<n && t+1<sizeof time_text;++i)time_text[t++]=num[i];
 time_text[t]=0;
 label(c,366,17,time_text,WHITE,1);

 // Small file number and actual track/note statistics.
 put_number(text,sizeof text,index+1);
 label(c,16,36,text,ORANGE,1);
 label(c,34,36,"/",GREY,1);
 put_number(text,sizeof text,file_count);
 label(c,44,36,text,WHITE,1);
 label(c,86,36,"TRACKS",GREY,1);
 put_number(text,sizeof text,loaded?info.tracks:0);
 label(c,131,36,text,WHITE,1);
 label(c,185,36,"NOTES",GREY,1);
 put_number(text,sizeof text,loaded?info.notes:0);
 label(c,224,36,text,WHITE,1);

 // Real piano roll: 36 semitone rows, x=49..463, y=53..196.
 // Horizontal scale: 4 pixels per 100ms, playhead at x=173.
 const int kLeft=49,kTop=53,kBottom=197,kRight=464;
 const int kRows=36,kRowH=4,kHeadX=173;
 op1f_paint_box(c,16,kTop,448,kBottom-kTop,PANEL);
 const u32 unit_us=100000u;
 u32 now_unit=divide_u32(elapsed_us,unit_us,0);

 // Choose a visible 3-octave range based on upcoming/ongoing notes.
 // We move the window only when notes fall outside it.
 if(loaded && info.count){
  u32 first=cursor>512u?cursor-512u:0u;
  u32 end=cursor+768u;
  if(end>info.count)end=info.count;
  int low=127,high=0,seen=0;
  for(u32 i=first;i<end;++i){
   const MidiEvent* e=&song[i];
   if(e->kind!=MIDI_NOTE_ON)continue;
   u32 ev_unit=divide_u32(e->due_us,unit_us,0);
   u32 tail_unit=divide_u32(roll_end_us[i],unit_us,0);
   if(tail_unit+38u<now_unit || ev_unit>now_unit+73u)continue;
   int pitch=(int)e->note+transpose;
   if(pitch<0||pitch>127)continue;
   if(pitch<low)low=pitch;
   if(pitch>high)high=pitch;
   seen=1;
  }
  if(seen){
   int target=(low+high)/2-18;
   if(target<0)target=0;
   if(target>92)target=92;
   int previous=(int)roll_low;
   if(low<previous+2 || high>previous+33)
    roll_low=(u32)target;
  }
 }

 // Keyboard: alternating natural/accidental notes, aligned to grid.
 for(int row=0;row<kRows;++row){
  int pitch=(int)roll_low+kRows-1-row;
  int y=kTop+row*kRowH;
  u32 rem=0; divide_u32((u32)pitch,12u,&rem); int semitone=(int)rem;
  int black=(semitone==1||semitone==3||semitone==6||
             semitone==8||semitone==10);
  op1f_paint_box(c,16,y,32,kRowH-1,black?BG:WHITE);
  if(black)op1f_paint_box(c,16,y,19,kRowH-1,GRID);
  op1f_paint_box(c,kLeft,y,kRight-kLeft,1,GRID);
 }
 op1f_paint_box(c,48,kTop,1,kBottom-kTop,GREY);
 for(int x=kHeadX-120;x<kRight;x+=40)
  if(x>=kLeft)op1f_paint_box(c,x,kTop,1,kBottom-kTop,GRID);

 if(loaded && info.count){
  // Time-window scan, bounded to keep GUI frame work predictable.
  // No changes to sequencer tick or MIDI dispatch.
  u32 first=cursor>1024u?cursor-1024u:0u;
  u32 end=cursor+1024u;
  if(end>info.count)end=info.count;
  for(u32 i=first;i<end;++i){
   const MidiEvent* e=&song[i];
   if(e->kind!=MIDI_NOTE_ON)continue;
   u32 from=divide_u32(e->due_us,unit_us,0);
   u32 until=divide_u32(roll_end_us[i],unit_us,0);
   if(until<from+1u)until=from+1u;
   if(until+32u<now_unit || from>now_unit+73u)continue;
   int pitch=(int)e->note+transpose;
   if(pitch<(int)roll_low||pitch>=(int)roll_low+kRows)continue;
   int x=kHeadX+((int)from-(int)now_unit)*4;
   int right=kHeadX+((int)until-(int)now_unit)*4;
   if(x<kLeft)x=kLeft;
   if(right>kRight)right=kRight;
   if(right<=x)continue;
   int y=kTop+(kRows-1-(pitch-(int)roll_low))*kRowH;
   op1f_paint_box(c,x,y+1,right-x,kRowH-1,
                  (e->channel&1u)?BLUE:ORANGE);
  }
 }
 // Playhead is always visible above the note bars.
 op1f_paint_box(c,kHeadX,kTop,2,kBottom-kTop,GREEN);
 if(error){
  op1f_paint_box(c,49,95,410,44,BG);
  label(c,59,107,error_name(error),ORANGE,2);
 }

 // Compact colored labels only; same font size throughout.
 label(c,16,212,"PLAY/PAUSE",BLUE,1);
 label(c,130,212,loop_enabled?"LOOP":"ONCE",ORANGE,1);
 label(c,236,212,"SPEED",WHITE,1);
 put_number(text,sizeof text,speed_percent);
 label(c,275,212,text,WHITE,1);
 label(c,303,212,"%",WHITE,1);
 label(c,349,212,"PITCH",RED,1);
 label(c,390,212,transpose>=0?"+":"-",RED,1);
 put_number(text,sizeof text,(u32)(transpose>=0?transpose:-transpose));
 label(c,399,212,text,RED,1);
 if(!seq_enabled)label(c,16,229,"SEQ OFF",ORANGE,1);
}
