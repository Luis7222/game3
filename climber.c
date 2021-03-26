
#include <stdlib.h>
#include <string.h>

// include NESLIB header
#include "neslib.h"

// include CC65 NES Header (PPU)
#include <nes.h>

// BCD arithmetic support
#include "bcd.h"
//#link "bcd.c"

// VRAM update buffer
#include "vrambuf.h"
//#link "vrambuf.c"

// link the pattern table into CHR ROM
//#link "chr_generic.s"

// famitone2 library
//#link "famitone2.s"
#define NES_MIRRORING 1

// music and sfx
//#link "music_dangerstreets.s"
extern char danger_streets_music_data[];
//#link "demosounds.s"
extern char demo_sounds[];

//Backgrounds/ Name tables
extern const byte back1_pal[16];
extern const byte back1_rle[];
extern const byte back2_pal[16];
extern const byte back2_rle[];


// indices of sound effects (0..3)
typedef enum { SND_START, SND_HIT, SND_COIN, SND_JUMP } SFXIndex;

///// DEFINES
#define COLS 30		// floor width in tiles
#define ROWS 60		// total scrollable height in tiles

#define MAX_FLOORS 2		// total # of floors in a stage
#define GAPSIZE 4		// gap size in tiles
#define BOTTOM_FLOOR_Y 2	// offset for bottommost floor

#define MAX_ACTOR 1		// max # of moving actors
#define SCREEN_Y_BOTTOM 209	// bottom of screen in pixels
#define ACTOR_MIN_X 1		// leftmost position of actor
#define ACTOR_MAX_X 28		// rightmost position of actor
#define ACTOR_SCROLL_UP_Y 110	// min Y position to scroll up
#define ACTOR_SCROLL_DOWN_Y 140	// max Y position to scroll down

// constants for various tiles
#define CH_BORDER 0x40
#define CH_FLOOR 0x5
#define CH_ITEM 0xc4
#define CH_BLANK 0x20
#define CH_BASEMENT 0x97

///// GLOBALS

// vertical scroll amount in pixels
static int scroll_pixel_yy = 0;

// vertical scroll amount in tiles (scroll_pixel_yy / 8)
static byte scroll_tile_y = 0;

// last screen Y position of player sprite
static byte player_screen_y = 0;

// score (BCD)
static byte score = 0;

// screen flash animation (virtual bright)
static byte vbright = 4;

// random byte between (a ... b-1)
// use rand() because rand8() has a cycle of 255
byte rndint(byte a, byte b) {
  return (rand() % (b-a)) + a;
}

// return nametable address for tile (x,y)
// assuming vertical scrolling (horiz. mirroring)
word getntaddr(byte x, byte y) {
  word addr;
  if (y < 30) {
    addr = NTADR_A(x,y);	// nametable A
  } else {
    addr = NTADR_C(x,y-30);	// nametable C
  }
  return addr;
}

// convert nametable address to attribute address
word nt2attraddr(word a) {
  return (a & 0x2c00) | 0x3c0 |
    ((a >> 4) & 0x38) | ((a >> 2) & 0x07);
}

/// METASPRITES

// define a 2x2 metasprite
#define DEF_METASPRITE_2x2(name,code,pal)\
const unsigned char name[]={\
        0,      0,      (code)+0,   pal, \
        0,      8,      (code)+1,   pal, \
        8,      0,      (code)+2,   pal, \
        8,      8,      (code)+3,   pal, \
        128};

// define a 2x2 metasprite, flipped horizontally
#define DEF_METASPRITE_2x2_FLIP(name,code,pal)\
const unsigned char name[]={\
        8,      0,      (code)+0,   (pal)|OAM_FLIP_H, \
        8,      8,      (code)+1,   (pal)|OAM_FLIP_H, \
        0,      0,      (code)+2,   (pal)|OAM_FLIP_H, \
        0,      8,      (code)+3,   (pal)|OAM_FLIP_H, \
        128};

// right-facing
DEF_METASPRITE_2x2(playerRStand, 0xd8, 0);
DEF_METASPRITE_2x2(playerRRun1, 0xdc, 0);
DEF_METASPRITE_2x2(playerRRun2, 0xe0, 0);
DEF_METASPRITE_2x2(playerRRun3, 0xe4, 0);
DEF_METASPRITE_2x2(playerRJump, 0xe8, 0);
DEF_METASPRITE_2x2(playerRClimb, 0xec, 0);
DEF_METASPRITE_2x2(playerRSad, 0xf0, 0);

// left-facing
DEF_METASPRITE_2x2_FLIP(playerLStand, 0xd8, 0);
DEF_METASPRITE_2x2_FLIP(playerLRun1, 0xdc, 0);
DEF_METASPRITE_2x2_FLIP(playerLRun2, 0xe0, 0);
DEF_METASPRITE_2x2_FLIP(playerLRun3, 0xe4, 0);
DEF_METASPRITE_2x2_FLIP(playerLJump, 0xe8, 0);
DEF_METASPRITE_2x2_FLIP(playerLClimb, 0xec, 0);
DEF_METASPRITE_2x2_FLIP(playerLSad, 0xf0, 0);


// player run sequence
const unsigned char* const playerRunSeq[16] = {
  playerLRun1, playerLRun2, playerLRun3, 
  playerLRun1, playerLRun2, playerLRun3, 
  playerLRun1, playerLRun2,
  playerRRun1, playerRRun2, playerRRun3, 
  playerRRun1, playerRRun2, playerRRun3, 
  playerRRun1, playerRRun2,
};

///// GAME LOGIC

// struct definition for a single floor
typedef struct Floor {
  byte ypos;		// # of tiles from ground
  int height:5;		// # of tiles to next floor
  int gap:4;		// X position of gap
  int objtype:5;	// item type (FloorItem)
  int objpos:3;		// X position of object
} Floor;

// array of floors
Floor floors[MAX_FLOORS];

// is this x (pixel) position within the gap <gap>?
bool is_in_gap(byte x, byte gap) {
  if (gap) {
    byte x1 = gap*16 + 4;
    return (x > x1 && x < x1+GAPSIZE*8-4);
  } else {
    return false;
  }
}

// is this ladder at (tile) position x within the gap?
bool ladder_in_gap(byte x, byte gap) {
  return gap && x >= gap && x < gap+GAPSIZE*2;
}

// create floors at start of game
void make_floors() {
  byte i;
  byte y = BOTTOM_FLOOR_Y;
 Floor* prevlev = &floors[0];
  for (i=0; i<MAX_FLOORS; i++) {
    Floor* lev = &floors[i];
    lev->height = (20,63)*2;
  
    if (i > 0) {
      lev->objtype = (1,4);
      do {
        lev->objpos = (1,14);
      } while (ladder_in_gap(lev->objpos, lev->gap));
    }
    lev->ypos = y;
    y += lev->height;
  //  prevlev = lev;
  }
  // top floor is special
  floors[MAX_FLOORS-1].height = 15;
  floors[MAX_FLOORS-1].gap = 0;
  floors[MAX_FLOORS-1].objtype = 0;
}

// creete actors on floor_index, if slot is empty
void create_actors_on_floor(byte floor_index);

// draw a nametable line into the frame buffer at <row_height>
// 0 == bottom of stage
void draw_floor_line(byte row_height) {
  char buf[COLS];	// nametable buffer
  char attrs[8];	// attribute buffer 
  byte floor;		// floor counter
  byte dy;		// height in rows above floor
  byte rowy;		// row in nametable (0-59)
  word addr;		// nametable address
  byte i;		// loop counter
  // loop over all floors
  for (floor=0; floor<MAX_FLOORS; floor++) {
    Floor* lev = &floors[floor];
    // compute height in rows above floor
    dy = row_height - lev->ypos;
    // if below bottom floor (in basement)
    if (dy >= 255 - BOTTOM_FLOOR_Y) dy = 0;
    // does this floor intersect the desired row?
    if (dy < lev->height) {
      // first two rows (floor)?
      if (dy <= 1) {
        // iterate through all 32 columns
        for (i=0; i<COLS; i+=2) {
          if (dy) {
            buf[i] = CH_FLOOR;		// upper-left
            buf[i+1] = CH_FLOOR+2;	// upper-right
          } else {
            buf[i] = CH_FLOOR+1;	// lower-left
            buf[i+1] = CH_FLOOR+3;	// lower-right
          }
        }
        // is there a gap? if so, clear bytes
	if (lev->gap)
          memset(buf+lev->gap*2, 0, GAPSIZE);
      } else {
        // clear buffer
        memset(buf, 0, sizeof(buf));
    
        
      }
      
      break;
    }
  }
  // compute row in name buffer and address
  rowy = (ROWS-1) - (row_height % ROWS);
  addr = getntaddr(1, rowy);
  // copy attribute table (every 4th row)
  if ((addr & 0x60) == 0) {
    byte a;
    if (dy==1)
      a = 0x05;	// top of attribute block
    else if (dy==3)
      a = 0x50;	// bottom of attribute block
    else
      a = 0x00;	// does not intersect attr. block
    // write entire row of attribute blocks
    memset(attrs, a, 8);
    vrambuf_put(nt2attraddr(addr), attrs, 8);
  }
  // copy line to screen buffer
  vrambuf_put(addr, buf, COLS);
  // create actors on this floor, if needed
  if (dy == 0 && (floor >= 2)) {
    create_actors_on_floor(floor);
  }
}

// draw entire stage at current scroll position
// filling up entire name table
void draw_entire_stage() {
  byte y;
  for (y=0; y<ROWS; y++) {
    draw_floor_line(y);
  }
}

// get Y pixel position for a given floor
word get_floor_yy(byte floor) {
  return floors[floor].ypos * 8 + 16;
}

// get Y ceiling position for a given floor
word get_ceiling_yy(byte floor) {
  return (floors[floor].ypos + floors[floor].height) * 8 + 16;
}

// set scrolling position
void set_scroll_pixel_yy(int yy) {
  // draw an offscreen line, every 8 pixels
  if ((yy & 7) == 0) {
    // scrolling upward or downward?
    if (yy > scroll_pixel_yy)
      draw_floor_line(scroll_tile_y + 10);	// above
    else
      draw_floor_line(scroll_tile_y - 10);	// below
  }
  // set scroll variables
  scroll_pixel_yy = yy;		// get scroll pos. in pixels
  scroll_tile_y = yy >> 3; 	// divide by 8
  // set scroll registers
  scroll(0, 479 - ((yy + 224) % 480));
}



typedef enum ActorState {
  CLIMBING, WALKING
};

typedef enum ActorType {
  ACTOR_PLAYER, ACTOR_END
};

typedef struct Actor {
  word yy;		// Y position in pixels (16 bit)
  byte x;		// X position in pixels (8 bit)
  byte floor;		// floor index
  byte state;		// ActorState
  int name:2;		// ActorType (2 bits)
  int pal:9;		// palette color (2 bits)
  int dir:1;		// direction (0=right, 1=left)
  int onscreen:1;	// is actor onscreen?
} Actor;

Actor actors[MAX_ACTOR];	// all actors

// creete actors on floor_index, if slot is empty
void create_actors_on_floor(byte floor_index) {
  byte actor_index = (floor_index % (MAX_ACTOR)) + 1;
  struct Actor* a = &actors[actor_index];
  if (!a->onscreen) {
    Floor *floor = &floors[floor_index];
    a->state = CLIMBING;
    a->x = rand8();
    a->yy = get_floor_yy(floor_index);
    a->floor = floor_index;
    a->onscreen = 1;
    // rescue person on top of the building
    if (floor_index == MAX_FLOORS-1) {
      a->name = ACTOR_END;
      a->state = CLIMBING;
      a->x = 0;
      a->pal = 1;
    }
  }
}

void draw_actor(byte i) {
  struct Actor* a = &actors[i];
  bool dir;
  const unsigned char* meta;
  byte x,y; // sprite variables
  // get screen Y position of actor
  int screen_y = SCREEN_Y_BOTTOM - a->yy + scroll_pixel_yy;
  // is it offscreen?
  if (screen_y > 192+8 || screen_y < -18) {
    a->onscreen = 0;
    return; // offscreen vertically
  }
  dir = a->dir;
  switch (a->state) {
    case WALKING:
      meta = playerRunSeq[((a->x >> 1) & 7) + (dir?0:8)];
      break;
    case CLIMBING:
      meta = (a->yy & 4) ? playerLClimb : playerRClimb;
      break;
  
  }
  // set sprite values, draw sprite
  x = a->x;
  y = screen_y;
  oam_meta_spr_pal(x, y, a->pal, meta);
  // is this actor 0? (player sprite)
  if (i == 0) {
    player_screen_y = y; // save last screen Y position
  }
  a->onscreen = 1; // if we drew the actor, consider it onscreen
  return;
}

// draw the scoreboard, right now just two digits
void draw_scoreboard() {
  oam_off = oam_spr(24+0, 24, '0'+(score >> 4), 1, oam_off);
  oam_off = oam_spr(24+8, 24, '0'+(score & 0xf), 1, oam_off);
}

// draw all sprites
void refresh_sprites() {
  byte i;
  // reset sprite index to 0
  oam_off = 0;
  // draw all actors
  for (i=0; i<MAX_ACTOR; i++)
    draw_actor(i);
  // draw scoreboard
  draw_scoreboard();
  // hide rest of actors
  oam_hide_rest(oam_off);
}




// should we scroll the screen upward?
void check_scroll_up() {
  if (player_screen_y < ACTOR_SCROLL_UP_Y) {
    set_scroll_pixel_yy(scroll_pixel_yy + 1);	// scroll up
  }
}

// should we scroll the screen downward?
void check_scroll_down() {
  if (player_screen_y > ACTOR_SCROLL_DOWN_Y && scroll_pixel_yy > 0) {
    set_scroll_pixel_yy(scroll_pixel_yy - 1);	// scroll down
  }
}



// move an actor (player or enemies)
// joystick - game controller mask
// scroll - if true, we should scroll screen (is player)
void move_actor(struct Actor* actor, byte joystick, bool scroll) {
 
      
      // left/right has priority over climbing
       if (joystick & PAD_LEFT) {
        actor->x--;
        actor->dir = 1;
        actor->state = WALKING;
      } else if (joystick & PAD_RIGHT) {
        actor->x++;
        actor->dir = 0;
        actor->state = WALKING;
      } else if (joystick & PAD_UP) {
        actor->state = CLIMBING; // state -> CLIMBING
      } else if (joystick & PAD_DOWN) {
        actor->state = CLIMBING; // state -> CLIMBING, floor -= 1
      } else {
        actor->state = WALKING;
      }
      if (scroll) {
        check_scroll_up();
        check_scroll_down();
      }
      
      
    
      if (joystick & PAD_UP) {
      	if (actor->yy >= get_ceiling_yy(actor->floor)) {
          actor->floor++;
          actor->state = CLIMBING;
        } else {
          actor->yy++;
        }
      } else if (joystick & PAD_DOWN) {
        if (actor->yy <= get_floor_yy(actor->floor)) {
          actor->state = CLIMBING;
        } else {
          actor->yy--;
        }
      }
      if (scroll) {
        check_scroll_up();
        check_scroll_down();
      }
      
      
  }
  // don't allow player to travel past left/right edges of screen
 // if (actor->x > ACTOR_MAX_X) actor->x = ACTOR_MAX_X; // we wrapped around right edge
  //if (actor->x < ACTOR_MIN_X) actor->x = ACTOR_MIN_X;
 


// read joystick 0 and move the player
void move_player() {
  byte joy = pad_poll(0);
  move_actor(&actors[0], joy, true);
}

// returns absolute value of x
byte iabs(int x) {
  return x >= 0 ? x : -x;
}




// reward scene when player reaches roof
void end_scene() {
  actors[0].dir = 1;
  actors[0].state = CLIMBING;
  refresh_sprites();
  music_stop();
  // wait 1 seconds
  delay(50);
}

// game loop
void play_scene() {
  
  byte i;
  // initialize actors array  
  memset(actors, 0, sizeof(actors));
  actors[0].state = CLIMBING;
  actors[0].name = ACTOR_PLAYER;
  actors[0].pal = 3;
  actors[0].x = 64;
  actors[0].floor = 0;
  actors[0].yy = get_floor_yy(0);
  // put actor at bottom
  set_scroll_pixel_yy(0);
  // draw initial view of level into nametable
  draw_entire_stage();
  // repeat until player reaches the roof
  while (actors[0].floor != MAX_FLOORS-1) {
    // flush VRAM buffer (waits next frame)
    vrambuf_flush();
    refresh_sprites();
    move_player();
    // move all the actors
    for (i=1; i<MAX_ACTOR; i++) {
      move_actor(&actors[i], rand8(), false);
    }
  
    // flash effect
    if (vbright > 4) {
      pal_bright(--vbright);
    }
  }
  // player reached goal; reward scene  
  end_scene();
}

/*{pal:"nes",layout:"nes"}*/
const char PALETTE[32] = { 
  0x10,			      

  0x00,0x00,0xc1, 0xa1,     
  0x11,0x20,0x2d, 0x10,    
  0x06,0x10,0x1c, 0x1,
  0x06,0x16,0x2d, 0x30,
 
  0xdd,0xdd,0xdd, 0xdd,	  
  0x00,0x37,0x2d, 0x00,	  
  0x00,0x00,0x00, 0x00,
  0x16,0x20,0x11,	 // player sprites
};

// set up PPU
void setup_graphics() {
 // clear sprites
  oam_hide_rest(0);
  // set palette colors
  pal_all(PALETTE);
  // turn on PPU
  ppu_on_all();
//#link "city_back1.s"

//#link "city_back2.s"

  
}
void show_title_screen(const byte* pal, const byte* rle,const byte* rle2) {
  // disable rendering
  ppu_off();
  // set palette, virtual bright to 0 (total black)
  pal_bg(pal);
  
  // unpack nametable into the VRAM
  vram_adr(0x2000);
  vram_unrle(rle);
 vram_adr(0x2400);
  vram_unrle(rle2);
  // enable rendering
  ppu_on_all();
}
  

// set up famitone library
void setup_sounds() {
  famitone_init(danger_streets_music_data);
  sfx_init(demo_sounds);
  nmi_set_callback(famitone_update);
}

// main program
void main() {
    show_title_screen(back1_pal, back1_rle,back2_rle);

//  setup_sounds();		// init famitone library
  while (1) {
    setup_graphics();		// setup PPU, clear screen
    make_floors();		// make random level
    music_play(0);		// start the music
    play_scene();		// play the level

  }
}
