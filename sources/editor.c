#include <stdbool.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "SDL.h"
#include "SDL_ttf.h"

#include "colorforth.h"

SDL_Color red         = {.r=234, .g=8,   .b=8};
SDL_Color cyan        = {.r=0,   .g=216, .b=249};
SDL_Color green       = {.r=9,   .g=201, .b=16};
SDL_Color dark_green  = {.r=36,  .g=122, .b=39};
SDL_Color yellow      = {.r=255, .g=255, .b=0};
SDL_Color dark_yellow = {.r=212, .g=209, .b=66};
SDL_Color magenta     = {.r=210, .g=20,  .b=197};
SDL_Color white       = {.r=255, .g=255, .b=255};
SDL_Color black       = {.r=0,   .g=0,   .b=0};

SDL_Renderer *renderer;
TTF_Font     *font;
SDL_Surface  *surface;
SDL_Texture  *texture;

cell_t      *blocks;
bool         is_first_definition;
bool         is_command = false;
bool         is_dirty_hack = false;
unsigned int word_index;
int          nb_block = 0;

// Globally defined for display_word() and screen_clear()
int x = 0, y = 0;

#define MASK 			0xffffffffL
#define INTERPRET_NUMBER_TAG 	8
#define INTERPRET_WORD_TAG 	0x00000001
#define SPACE_BETWEEN_WORDS	7
#define WORD_MAX_LENGTH 	20

static void
cursor_display(int x, int y)
{
	SDL_Rect r = {.x = x, .y = y, .w = 10, .h = 12};
	SDL_SetRenderDrawColor(renderer, 0, 12, 125, 255);
	SDL_RenderFillRect(renderer, &r);
	SDL_RenderPresent(renderer);
}

static void
display_text(char *text, SDL_Color color, int x, int y)
{
	int texW = 0;
	int texH = 0;

	surface = TTF_RenderText_Solid(font, text, color);
	texture = SDL_CreateTextureFromSurface(renderer, surface);

	SDL_QueryTexture(texture, NULL, NULL, &texW, &texH);
	SDL_Rect location = {x, y, texW, texH};

	SDL_RenderCopy(renderer, texture, NULL, &location);
	SDL_RenderPresent(renderer);
}

static void
display_word(cell_t word)
{
	uint8_t word_color = word & 0x0000000f;
	//bool is_hex = false;
	static SDL_Color color;
	char unpacked[WORD_MAX_LENGTH]; // Let's forsee very large
	int w, h; // text width and height

	switch(word_color)
	{
		case 0:
			snprintf(unpacked, WORD_MAX_LENGTH, "%s", unpack(word));
			TTF_SizeText(font, unpacked, &w, &h);
			x -= w; // Go back to hide a space
			break;

		case 1:
			snprintf(unpacked, WORD_MAX_LENGTH, "%s", unpack(word));
			color = yellow;
			break;

		case 2:
			snprintf(unpacked, WORD_MAX_LENGTH, "%d", word >> 5);
			color = dark_yellow;
			break;

		case 3:
			snprintf(unpacked, WORD_MAX_LENGTH, "%s", unpack(word));
			TTF_SizeText(font, unpacked, &w, &h);
			if (is_first_definition)
				is_first_definition = false;
			else
				y += h;
			x = 0;
			color = red;
			break;

		case 4:
			snprintf(unpacked, WORD_MAX_LENGTH, "%s", unpack(word));
			color = green;
			break;

		case 5:
			/*if (word & 0x10)
				is_hex = true;*/
			snprintf(unpacked, WORD_MAX_LENGTH, "%d", word >> 5);
			color = dark_green;
			break;

		case 6:
			snprintf(unpacked, WORD_MAX_LENGTH, "%d", word >> 5);
			color = green;
			break;

		case 7:
			snprintf(unpacked, WORD_MAX_LENGTH, "%d", word >> 5);
			color = cyan;
			break;

		case 8:
			snprintf(unpacked, WORD_MAX_LENGTH, "%d", word >> 5);
			color = yellow;
			break;

		case 9:
		case 0xa:
		case 0xb:
			snprintf(unpacked, WORD_MAX_LENGTH, "%s", unpack(word));
			color = white;
			break;

		case 0xc:
			snprintf(unpacked, WORD_MAX_LENGTH, "%s", unpack(word));
			color = magenta;
			break;

		case 0xf:
			snprintf(unpacked, WORD_MAX_LENGTH, "%d", word >> 5);
			color = white;
			break;

		default:
			SDL_Log("Error: wrong color code!");
			break;
	}

	display_text(unpacked, color, x, y);

	TTF_SizeText(font, unpacked, &w, &h);

	x += w + SPACE_BETWEEN_WORDS;
}


static void
screen_clear(void)
{
	x = 0;
	y = 0;
	SDL_SetRenderDrawColor(renderer, 50, 70, 122, 147);
	SDL_RenderClear(renderer);
}

static void
command_prompt_display(void)
{
	display_text("> ", yellow, 0, 550);
	cursor_display(15, 562);
}

static void
status_bar_update_block_number(cell_t n)
{
	char block_info[30];

	snprintf(block_info, 30, "Block: %d", n);
	display_text(block_info, yellow, 680, 560);
}

static void
display_stack()
{
	char *stack_content = dot_s();
	display_text(stack_content, yellow, 0, 570);
	free(stack_content);
}

static void
display_block(cell_t n)
{
	unsigned long start = n * 256;     // Start executing block from here...
	unsigned long limit = (n+1) * 256; // to this point.

	screen_clear();

	is_first_definition = true;

	for (word_index = start; word_index < limit; word_index++)
		display_word(blocks[word_index]);

	command_prompt_display();
	status_bar_update_block_number(n);
	display_stack();
}

bool
is_number(const char *ptr)
{
	while (*ptr++)
	{
		// Allowed ones are 0 to 9 and NULL
		if (!((*ptr >= 0x31 && *ptr <= 0x39) || *ptr == 0x00))
			return false;
	}

	return true;
}

static int
do_cmd(const char *word)
{
	cell_t packed;

	if (is_number(word))
	{
		packed = ((atoi(word) << 5) & MASK) + INTERPRET_NUMBER_TAG;
	}
	else
	{
		packed = (pack(word) & 0xfffffff0) | INTERPRET_WORD_TAG;

		if (!lookup_word(packed, FORTH_DICTIONARY))
			return -1;
	}

	if ((cell_t)(packed & 0xfffffff0) == pack("dup"))
		is_dirty_hack = true;
	else
		is_dirty_hack = false;

	do_word(packed);

	return 0;
}


int
main(void)
{
	char word[WORD_MAX_LENGTH];
	bool done = SDL_FALSE;
	char *str;
	int status;
	int fd;
	struct stat sbuf;


	SDL_Event event;

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		SDL_Log("Unable to initialize SDL: %s", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	TTF_Init();

	SDL_Window *window = SDL_CreateWindow("Iridescence colorForth",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		800, 600, 0);
	renderer = SDL_CreateRenderer(window, -1, 0);
	font = TTF_OpenFont("GohuFont-Bold.ttf", 25);
	SDL_Color color = yellow;

	memset(word, 0, WORD_MAX_LENGTH);

	if ((fd = open("blocks/blocks.cf", O_RDONLY)) == -1)
	{
		perror("open");
		exit(EXIT_FAILURE);
	}

	if (stat("blocks/blocks.cf", &sbuf) == -1)
	{
		perror("stat");
		exit(EXIT_FAILURE);
	}

	blocks = mmap(0, sbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);

	if (blocks == MAP_FAILED)
	{
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	colorforth_initialize();

	display_block(0);

	while (!done)
	{
		SDL_WaitEvent(&event);

		switch (event.type)
		{
			case SDL_QUIT:
				done = SDL_TRUE;
				break;

			case SDL_KEYDOWN:
				switch (event.key.keysym.sym)
				{
					case SDLK_F1:
						color = red;
						break;

					case SDLK_F2:
						color = cyan;
						break;

					case SDLK_F3:
						color = green;
						break;

					case SDLK_F4:
						color = dark_green;
						break;

					case SDLK_F5:
						color = yellow;
						break;

					case SDLK_F6:
						color = dark_yellow;
						break;

					case SDLK_F7:
						color = magenta;
						break;

					case SDLK_F8:
						color = white;
						break;

					case SDLK_F9:
						x = 10;
						y = 550;
						is_command = true;
						break;

					case SDLK_F10:
						x = 0;
						y = 0;
						break;

					case SDLK_PAGEDOWN:
						display_block(++nb_block);
						break;

					case SDLK_PAGEUP:
						if (nb_block-1 < 0)
							break;
						display_block(--nb_block);
						break;

					case SDLK_SPACE:
						str = rindex(word, ' ');

						// Get rid of a potential space
						status = str ? do_cmd(str + 1) : do_cmd(word);

						memset(word, 0, WORD_MAX_LENGTH);
						display_block(nb_block);

						if (status == -1)
							display_text("Error: word not found!", red, 0, 585);

						break;

					case SDLK_UP:
						cursor_display(x, y+10);
						break;
				}
				break;

			case SDL_TEXTINPUT:
				strcat(word, event.text.text);
				break;
		}

		display_text(word, color, 10, 550);
	}

	SDL_DestroyTexture(texture);
	SDL_FreeSurface(surface);
	TTF_CloseFont(font);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	TTF_Quit();
	SDL_Quit();

	munmap(blocks, sbuf.st_size);
	close(fd);
	colorforth_finalize();

	return 0;
}
