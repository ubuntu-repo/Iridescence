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
unsigned int word_index;
int          nb_block = 0;

// Globally defined for display_word() and screen_clear()
int x = 0, y = 0;

#define SPACE_BETWEEN_WORDS 7
#define MASK 0xffffffffL
#define INTERPRET_NUMBER_TAG 8
#define INTERPRET_WORD_TAG 0x00000001

static void
cursor_display(int x, int y)
{
	SDL_Rect r = {.x = x, .y = y, .w = 10, .h = 12};
	SDL_SetRenderDrawColor(renderer, 0, 12, 125, 255);
	SDL_RenderFillRect(renderer, &r);
	SDL_RenderPresent(renderer);
}

static void
display_word(cell_t word)
{
	uint8_t word_color = word & 0x0000000f;
	//bool is_hex = false;
	static SDL_Color color;
	char *unpacked = unpack(word);
	char number[21];
	int w, h; // text width and height

	switch(word_color)
	{
		case 0:
			surface = TTF_RenderText_Solid(font, unpacked, color);
			TTF_SizeText(font, unpacked, &w, &h);
			x -= w; // Go back to hide a space
			break;

		case 1:
			surface = TTF_RenderText_Solid(font, unpacked, yellow);
			TTF_SizeText(font, unpacked, &w, &h);
			color = yellow;
			break;

		case 2:
			snprintf(number, 20, "%d", word >> 5);
			surface = TTF_RenderText_Solid(font, number, dark_yellow);
			TTF_SizeText(font, number, &w, &h);
			color = dark_yellow;
			break;

		case 3:
			TTF_SizeText(font, unpacked, &w, &h);
			if (is_first_definition)
				is_first_definition = false;
			else
				y += h;
			surface = TTF_RenderText_Solid(font, unpacked, red);
			x = 0;
			color = red;
			break;

		case 4:
			surface = TTF_RenderText_Solid(font, unpacked, green);
			TTF_SizeText(font, unpacked, &w, &h);
			color = green;
			break;

		case 5:
			/*if (word & 0x10)
				is_hex = true;*/
			snprintf(number, 20, "%d", word >> 5);
			surface = TTF_RenderText_Solid(font, number, dark_green);
			TTF_SizeText(font, number, &w, &h);
			color = dark_green;
			break;

		case 6:
			snprintf(number, 20, "%d", word >> 5);
			surface = TTF_RenderText_Solid(font, number, green);
			TTF_SizeText(font, number, &w, &h);
			color = green;
			break;

		case 7:
			surface = TTF_RenderText_Solid(font, unpacked, cyan);
			TTF_SizeText(font, unpacked, &w, &h);
			color = cyan;
			break;

		case 8:
			snprintf(number, 20, "%d", word >> 5);
			surface = TTF_RenderText_Solid(font, number, yellow);
			TTF_SizeText(font, number, &w, &h);
			color = yellow;
			break;

		case 9:
		case 0xa:
		case 0xb:
			surface = TTF_RenderText_Solid(font, unpacked, white);
			TTF_SizeText(font, unpacked, &w, &h);
			break;

		case 0xc:
			surface = TTF_RenderText_Solid(font, unpacked, magenta);
			TTF_SizeText(font, unpacked, &w, &h);
			break;

		case 0xf:
			snprintf(number, 20, "%d", word >> 5);
			surface = TTF_RenderText_Solid(font, number, white);
			TTF_SizeText(font, number, &w, &h);
			break;

		default:
			SDL_Log("Error: wrong color code!");
			break;
	}

	int texH = 0, texW = 0;

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_QueryTexture(texture, NULL, NULL, &texW, &texH);

	SDL_Rect location = {x, y, texW, texH};
	SDL_RenderCopy(renderer, texture, NULL, &location);
	SDL_RenderPresent(renderer);

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
	int texH = 0, texW = 0;

	surface = TTF_RenderText_Solid(font, "> ", green);

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_QueryTexture(texture, NULL, NULL, &texW, &texH);

	SDL_Rect location = {0, 560, texW, texH};
	SDL_RenderCopy(renderer, texture, NULL, &location);
	SDL_RenderPresent(renderer);

	cursor_display(15, 562);
}

static void
status_bar_update_block_number(cell_t n)
{
	int texH = 0, texW = 0;
	char block_info[30];

	snprintf(block_info, 30, "Block: %d", n);

	surface = TTF_RenderText_Solid(font, block_info, green);

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_QueryTexture(texture, NULL, NULL, &texW, &texH);

	SDL_Rect location = {680, 570, texW, texH};
	SDL_RenderCopy(renderer, texture, NULL, &location);
	SDL_RenderPresent(renderer);
}

static void
display_block(cell_t n)
{
	unsigned long start, limit;

	start = n * 256;     // Start executing block from here...
	limit = (n+1) * 256; // to this point.

	screen_clear();

	is_first_definition = true;

	for (word_index = start; word_index < limit; word_index++)
	{
		display_word(blocks[word_index]);
	}

	command_prompt_display();
	status_bar_update_block_number(n);
}

bool
is_number(char *ptr)
{
	while (*ptr++)
	{
		switch(*ptr)
		{
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			case '\0':
				break;
			default:
				return false;
		}
	}

	return true;
}

static void
do_cmd(char *word)
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
		{
			int texH = 0, texW = 0;

			surface = TTF_RenderText_Solid(font, "Error: word not found! ", red);

			texture = SDL_CreateTextureFromSurface(renderer, surface);
			SDL_QueryTexture(texture, NULL, NULL, &texW, &texH);

			SDL_Rect location = {0, 580, texW, texH};
			SDL_RenderCopy(renderer, texture, NULL, &location);
			SDL_RenderPresent(renderer);
			return;
		}
	}

	do_word(packed);
	dot_s();
}


int
main(void)
{
	bool done = SDL_FALSE;
	char text[255];
	int texW = 0;
	int texH = 0;
	char *str;

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
	SDL_Color color = white;

	// cf
	int fd;
	struct stat sbuf;

	memset(text, 0, 255);

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
	// cf

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
						y = 560;
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
						str = rindex(text, ' ');

						// Get rid of a potential space
						if (str)
							do_cmd(str + 1);
						else
							do_cmd(text);
						break;
				}
				break;

			case SDL_TEXTINPUT:
				strcat(text, event.text.text);
				break;
		}

		surface = TTF_RenderText_Solid(font, text, color);
		texture = SDL_CreateTextureFromSurface(renderer, surface);

		SDL_QueryTexture(texture, NULL, NULL, &texW, &texH);
		SDL_Rect location = {x, y, texW, texH};

		SDL_RenderCopy(renderer, texture, NULL, &location);
		SDL_RenderPresent(renderer);
	}

	SDL_DestroyTexture(texture);
	SDL_FreeSurface(surface);
	TTF_CloseFont(font);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	TTF_Quit();
	SDL_Quit();

	// cf
	munmap(blocks, sbuf.st_size);
	close(fd);
	colorforth_finalize();

	return 0;
}
