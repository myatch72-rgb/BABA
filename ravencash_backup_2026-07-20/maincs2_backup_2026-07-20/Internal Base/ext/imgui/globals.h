#pragma once

/*icons:
A - target (person)
B - target variant 2
C - eye
D - half filled( or half empty :) ) circle
E - circle
F - flag
G - heart
H - home
I - linux
J - magic stick
K - 'play'
L - exit
M - exit variant 2
N - wifi
O - half filled shield
P - log out
Q - loading
R - water drop
S - umbrella
T - caution
U - settings
*/

#define cheat_icon_symbol "G"
#define cheat_name "raven"
#define cheat_domain ".cc"

extern bool particles;

struct ID3D11ShaderResourceView;

/*textures*/
extern ID3D11ShaderResourceView* logo;
extern ID3D11ShaderResourceView* logo_png;
extern ID3D11ShaderResourceView* logotwo;
extern ID3D11ShaderResourceView* foto_user;

/*user info*/
extern char discord_username[64];
extern char expiry_date[64];

/*floats*/
extern float accent_colour[4];

/*ints*/
extern int old_tab;
static int tab = 0;

/*chars*/
static char username[64];
static char password[64];

/*options*/
extern float content_animation;
extern float dpi_scale;

static bool update_on_f5 = true;
static bool remember_me = false;

static int game = 0;
extern const char* games_list[4];
