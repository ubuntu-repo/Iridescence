/*
 * Copyright (c) 2015-2016 Konstantin Tcholokachvili
 * All rights reserved.
 * Use of this source code is governed by a MIT license that can be 
 * found in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE_HEAP_SIZE (1024 * 100)	// 100 Kb
#define STACK_SIZE     8

#define FORTH_DICTIONARY true
#define MACRO_DICTIONARY false

#define FORTH_TRUE -1      // In Forth world -1 means true
#define FORTH_FALSE 0

/*
 * Data types
 */
typedef int32_t cell_t; // 32-bit words only

struct word_entry
{
	cell_t                 name;
	void                  *code_address;
	void                  *codeword;
	LIST_ENTRY(word_entry) next;
};

typedef void (*FUNCTION_EXEC)(void);

/*
 * Stack macros
 */
#define stack_push(x) *(++tos) = x
#define stack_pop()   *(tos--)
#define nos           tos[-1]	// Next On Stack
#define rpush(x)      *(++rtos) = x
#define rpop()        *(rtos--)
#define start_of(x)   (&x[0])

/* Data stack */
cell_t stack[STACK_SIZE];
cell_t *tos = start_of(stack);	// Top Of Stack

/* Return stack */
unsigned long rstack[STACK_SIZE];
unsigned long *rtos = start_of(rstack);

/*
 * Global variables
 */
unsigned long *code_here;
unsigned long *h;	                // Code is inserted here
bool          selected_dictionary;
cell_t        *blocks;              // Manage looping over the code contained in blocks
unsigned long i;                    // Avoids stopping on variable defaulting to 0

LIST_HEAD(, word_entry) forth_dictionary;
LIST_HEAD(, word_entry) macro_dictionary;

/*
 * Forth traditional registers
 */
unsigned long *P;
unsigned long *W;
unsigned long *I;

/*
 * Prototypes
 */
static void ignore(const cell_t word);
static void interpret_forth_word(const cell_t word);
static void interpret_big_number(const cell_t number);
static void create_word(cell_t word);
static void compile_word(const cell_t word);
static void compile_big_number(const cell_t number);
static void compile_number(const cell_t number);
static void compile_macro(const cell_t word);
static void interpret_number(const cell_t number);
static void variable_word(const cell_t word);
static void literal(void);
static void run_block(const cell_t word);
static void colorforth_initialize(void);
static void colorforth_finalize(void);


/* Word extensions (0), comments (9, 10, 11, 15), compiler feedback (13)
 * and display macro (14) are ignored. */
void (*color_word_action[16])() = {ignore, interpret_forth_word,
	interpret_big_number, create_word, compile_word, compile_big_number,
	compile_number, compile_macro, interpret_number,
	ignore, ignore, ignore, variable_word, ignore, ignore, ignore};

/* 
 * Packing and unpacking words 
 */
char *code = " rtoeanismcylgfwdvpbhxuq0123456789j-k.z/;:!+@*,?";

int get_code_index(const char letter)
{
	// Get the index of a character in the 'code' sequence.
	return strchr(code, letter) - code;
}

#define HIGHBIT 0x80000000L
#define MASK    0xffffffffL

cell_t
pack(const char *word_name)
{
	int word_length, i, bits, length, letter_code, packed;

	word_length = strlen(word_name);
	packed      = 0;
	bits        = 28;

	for (i = 0; i < word_length; i++)
	{
		letter_code = get_code_index(word_name[i]);
		length      = 4 + (letter_code > 7) + (2 * (letter_code > 15));
		letter_code += (8 * (length == 5)) + ((96 - 16) * (length == 7));
		packed      = (packed << length) + letter_code;
		bits        -= length;
	}
	
	packed <<= bits + 4;
	return packed;
}

char *
unpack(const cell_t word)
{
	unsigned char nibble;
	static char text[16];
	unsigned int coded, bits, i;

	coded  = word;
	i      = 0;
	bits   = 32 - 4;
	coded &= ~0xf;

	memset(text, 0, 16);

	while (coded)
	{
		nibble = coded >> 28;
		coded  = (coded << 4) & MASK;
		bits  -= 4;

		if (nibble < 0x8)
		{
			text[i] += code[nibble];
		}
		else if (nibble < 0xc)
		{
			text[i] += code[(((nibble^0xc) << 1) | ((coded&HIGHBIT) > 0))];
			coded    = (coded << 1) & MASK;
			bits    -= 1;
		}
		else
		{
			text[i] += code[(coded >> 29) + (8 * (nibble - 10))];
			coded    = (coded << 3) & MASK;
			bits    -= 3;
		}

		i++;
	}

	return text;
}


/*
 * Threading words
 */
void next(void)
{
	printf("P: %lx -> %lx\n", (unsigned long)P, *(P));
	W =  I;
	I++;
	P = W;
	printf("P: %lx -> %lx\n", (unsigned long)P, *(P));
	((FUNCTION_EXEC)*P)();
}

void docol(void)
{
	rpush((cell_t)I);
	I = W + 1;
	printf("DOCOL: W=%lx, I=%lx\n", (unsigned long)W, (unsigned long)I);
	next();
}

void dosemi(void)
{
	printf("DOSEMI");
	*I = rpop();
	next();
}


/*
 * Built-in words
 */
void comma(void)
{
	*h = stack_pop();
	printf("\t, Comma: h at: %p, pointing to %p, TOS = %x\n", h, (void *)*h, 
			(unsigned int)*(tos));
	h++;
}

void load(void)
{
	cell_t n;

	n = stack_pop();
	run_block(n);
}

void loads(void)
{
	cell_t i, j;

	j = stack_pop();
	i = stack_pop();

	// Load blocks, excluding shadow blocks
	for (; i <= j; i += 2)
	{
		stack_push(i);
		load();
	}
}

void forth(void)
{
	selected_dictionary = FORTH_DICTIONARY;
}

void macro(void)
{
	selected_dictionary = MACRO_DICTIONARY;
}

void exit_word(void)
{
	cell_t n;
printf(" => Exit\n");
	n = rpop();
	(void)n;

}

void add(void)
{
	cell_t n;

	n = stack_pop();
	*tos += n;
}

void sub(void)
{
	cell_t n;

	n = stack_pop();
	*tos -= n;
}

void multiply(void)
{
	cell_t n;

	n = stack_pop();
	*tos *= n;
}

void divide(void)
{
	cell_t n;

	n = stack_pop();
	*tos /= n;
}

void modulo(void)
{
	cell_t n;

	n = stack_pop();
	*tos %= n;
}

void lt(void)
{
	cell_t n;

	n = stack_pop();
	*tos = (*tos < n) ? FORTH_TRUE : FORTH_FALSE;
}

void gt(void)
{
	cell_t n;

	n = stack_pop();
	*tos = (*tos > n) ? FORTH_TRUE : FORTH_FALSE;
}

void ge(void)
{
	cell_t n;

	n = stack_pop();
	*tos = (*tos >= n) ? FORTH_TRUE : FORTH_FALSE;
}

void ne(void)
{
	cell_t n;

	n = stack_pop();
	*tos = (*tos != n) ? FORTH_TRUE : FORTH_FALSE;
}

void eq(void)
{
	cell_t n;

	n = stack_pop();
	*tos = (*tos == n) ? FORTH_TRUE : FORTH_FALSE;
}

void le(void)
{
	cell_t n;

	n = stack_pop();
	*tos = (*tos <= n) ? FORTH_TRUE : FORTH_FALSE;
}

void and(void) {}
void or(void) {}
void xor(void) {}
void not(void) {}

void dup_word(void)
{
	cell_t n;

	n = *tos;
	stack_push(n);

	next();
}

void drop(void)
{
	(void)stack_pop(); // Cast to avoid a warning about not used computed value
}

void over(void)
{
	cell_t n;

	n = nos;
	stack_push(n);
}

void swap(void)
{
	cell_t t;

	t = *tos;
	*tos = nos;
	nos = t;
}

void dot_s(void)
{
	int i, nb_items;

	nb_items = tos - start_of(stack);

	printf("\nStack: ");

	for (i = 1; i < nb_items + 1; i++)
		printf("%d ", (int)stack[i]);
	printf("\n");
}

void store(void)
{
	cell_t address, value;

	address = stack_pop();
	value   = stack_pop();

	*(cell_t *)address = value;
}

void fetch(void)
{
	cell_t address, value;

	address = stack_pop();
	value   = *(cell_t *)address;

	stack_push(value);
}

void dump_dict(void)
{
	struct word_entry *item;

	LIST_FOREACH(item, &forth_dictionary, next)
	{
		printf("word: %10s, %x, code:%p\n", unpack(item->name), (int)item->name, item->code_address);
	}

	LIST_FOREACH(item, &macro_dictionary, next)
	{
		printf("word: %10s, %x, code:%p\n", unpack(item->name), (int)item->name, item->code_address);
	}
}

void here(void)
{
	stack_push((cell_t)h);
}

void zero_branch(void)
{
	cell_t n = stack_pop();

	if (n == FORTH_TRUE)
		I++;
	else
		I = (unsigned long *)*I;
	
	next();
}

void if_(void)
{
	stack_push((cell_t)zero_branch);
	comma();

	here();
	stack_push(0);
	comma();
}

void then(void)
{
	here();
	swap();
	store();
}

void for_(void)
{
	cell_t n = stack_pop();

	rpush(n);
	here();
}

void next_(void)
{
	cell_t n = rpop();
	
	if (n > 0)
	{
		n--;
		rpush(n);

		here();
		store();
	}
}

void to_r(void)
{
	cell_t n;

	n = stack_pop();
	rpush(n);
}

void from_r(void)
{
	cell_t n;

	n = rpop();
	stack_push(n);
}

void rdrop(void)
{
	(void)rpop();
}

void dot(void)
{
	printf("%d ", (int)stack_pop());
}

void minus_one(void)
{
	*tos -= 1; 
}

void i_word(void)
{
	cell_t n;

	n = rpop();
	stack_push(n);
}


/*
 * Helper functions
 */
static void
do_word(const cell_t word)
{
	uint8_t color = (int)word & 0x0000000f;
	
	if (color == 2 || color == 5 || color == 6 || color == 8 || color == 15)
	{
		printf("Color = %1d, Word = %10d, packed = %8x\n", 
				color, word >> 5, word);
	}
	else
	{
		printf("Color = %1d, Word = %10s, packed = %8x\n", 
				color, unpack(word), word);
	}
	
	(*color_word_action[color])(word);
}

void
run_block(const cell_t n)
{
	unsigned long start, limit;

	start = n * 256;     // Start executing block from here...
	limit = (n+1) * 256; // ...to this point.

	for (i = start; i < limit-1; i++)
	{
		if (blocks[i] == 0)
			return; // To avoid displaying msgs for debug purpose

		do_word(blocks[i]);
	}
}

struct word_entry *
lookup_word(cell_t name, const bool force_dictionary)
{
	struct word_entry *item;

	name &= 0xfffffff0; // Don't care about the color byte
	printf("Lookup : %x\n", name);

	if (force_dictionary == FORTH_DICTIONARY)
	{
		LIST_FOREACH(item, &forth_dictionary, next)
		{
			if (name == item->name)
				return item;
		}
	}
	else
	{
		LIST_FOREACH(item, &macro_dictionary, next)
		{
			if (name == item->name)
				return item;
		}
	}

	return NULL;
}

static void
insert_builtins_into_forth_dictionary(void)
{
	struct word_entry *_comma, *_load, *_loads, *_forth, *_macro, 
					  *_exit_word, *_store, *_fetch, *_add, *_sub, 
					  *_mult, *_div, *_ne, *_dup, *_dot, *_here, *_i;

	_comma      = calloc(1, sizeof(struct word_entry));
	_load	    = calloc(1, sizeof(struct word_entry));
	_loads	    = calloc(1, sizeof(struct word_entry));
	_forth	    = calloc(1, sizeof(struct word_entry));
	_macro	    = calloc(1, sizeof(struct word_entry));
	_exit_word  = calloc(1, sizeof(struct word_entry));
	_store      = calloc(1, sizeof(struct word_entry));
	_fetch      = calloc(1, sizeof(struct word_entry));
	_add        = calloc(1, sizeof(struct word_entry));
	_sub        = calloc(1, sizeof(struct word_entry));
	_mult       = calloc(1, sizeof(struct word_entry));
	_div        = calloc(1, sizeof(struct word_entry));
	_ne         = calloc(1, sizeof(struct word_entry));
	_dup        = calloc(1, sizeof(struct word_entry));
	_dot        = calloc(1, sizeof(struct word_entry));
	_here       = calloc(1, sizeof(struct word_entry));
	_i          = calloc(1, sizeof(struct word_entry));


	if (!_comma || !_load || !_loads || !_forth || !_macro || !_exit_word
			|| !_store || !_fetch || !_add || !_sub || !_mult || !_div 
			|| !_ne || !_dup || !_dot || !_here || !_i)
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		free(code_here);
		exit(EXIT_FAILURE);
	}

	_comma->name             = pack(",");
	_comma->code_address     = comma;

	_load->name              = pack("load");
	_load->code_address      = load;
	
	_loads->name             = pack("loads");
	_loads->code_address     = loads;

	_forth->name             = pack("forth");
	_forth->code_address     = forth;

	_macro->name             = pack("macro");
	_macro->code_address     = macro;

	_exit_word->name         = pack(";");
	_exit_word->code_address = exit_word;
	_exit_word->codeword     = &(_exit_word->code_address);

	_store->name             = pack("!");
	_store->code_address     = store;
	_store->codeword         = &(_store->code_address);

	_fetch->name             = pack("@");
	_fetch->code_address     = fetch;
	_fetch->codeword         = &(_fetch->code_address);

	_add->name               = pack("+");
	_add->code_address       = add;
	_add->codeword           = &(_add->code_address);

	_sub->name               = pack("-");
	_sub->code_address       = sub;

	_mult->name              = pack("*");
	_mult->code_address      = multiply;
	_mult->codeword          = &(_mult->code_address);

	_div->name               = pack("/");
	_div->code_address       = divide;

	_ne->name                = pack("ne");
	_ne->code_address        = ne;

	_dup->name               = pack("dup");
	_dup->code_address       = dup_word;
	_dup->codeword           = &(_dup->code_address);

	_dot->name               = pack(".");
	_dot->code_address       = dot;
	_dot->codeword           = &(_dot->code_address);

	_here->name              = pack("here");
	_here->code_address      = here;

	_i->name                 = pack("i");
	_i->code_address         = i_word;

	LIST_INSERT_HEAD(&forth_dictionary, _comma,      next);
	LIST_INSERT_HEAD(&forth_dictionary, _load,       next);
	LIST_INSERT_HEAD(&forth_dictionary, _loads,      next);
	LIST_INSERT_HEAD(&forth_dictionary, _forth,      next);
	LIST_INSERT_HEAD(&forth_dictionary, _macro,      next);
	LIST_INSERT_HEAD(&forth_dictionary, _exit_word,  next);
	LIST_INSERT_HEAD(&forth_dictionary, _store,      next);
	LIST_INSERT_HEAD(&forth_dictionary, _fetch,      next);
	LIST_INSERT_HEAD(&forth_dictionary, _add,        next);
	LIST_INSERT_HEAD(&forth_dictionary, _sub,        next);
	LIST_INSERT_HEAD(&forth_dictionary, _mult,       next);
	LIST_INSERT_HEAD(&forth_dictionary, _div,        next);
	LIST_INSERT_HEAD(&forth_dictionary, _ne,         next);
	LIST_INSERT_HEAD(&forth_dictionary, _dup,        next);
	LIST_INSERT_HEAD(&forth_dictionary, _dot,        next);
	LIST_INSERT_HEAD(&forth_dictionary, _here,       next);
	LIST_INSERT_HEAD(&forth_dictionary, _i,          next);
}

static void
insert_builtins_into_macro_dictionary(void)
{
	struct word_entry *_zero_branch, *_to_r, *_from_r, *_rdrop,
					  *_minus_one, *_ne, *_swap,
					  *_if, *_then, *_for, *_next;

	_zero_branch = calloc(1, sizeof(struct word_entry));
	_to_r        = calloc(1, sizeof(struct word_entry));
	_from_r      = calloc(1, sizeof(struct word_entry));
	_rdrop       = calloc(1, sizeof(struct word_entry));
	_minus_one   = calloc(1, sizeof(struct word_entry));
	_ne          = calloc(1, sizeof(struct word_entry));
	_swap        = calloc(1, sizeof(struct word_entry));
	_if          = calloc(1, sizeof(struct word_entry));
	_then        = calloc(1, sizeof(struct word_entry));
	_for         = calloc(1, sizeof(struct word_entry));
	_next        = calloc(1, sizeof(struct word_entry));

	if (!_zero_branch || !_to_r || !_from_r || !_rdrop || !_minus_one
			|| !_ne || !_swap || !_if || !_then || !_for || !_next)
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		free(code_here);
		exit(EXIT_FAILURE);
	}

	_zero_branch->name         = pack("zb");
	_zero_branch->code_address = zero_branch;
	_zero_branch->codeword     = &(_zero_branch->code_address);

	_to_r->name                = pack("+r");
	_to_r->code_address        = to_r;

	_from_r->name              = pack("r-");
	_from_r->code_address      = from_r;

	_rdrop->name               = pack("rdrop");
	_rdrop->code_address       = rdrop;

	_minus_one->name           = pack("1-");
	_minus_one->code_address   = minus_one;

	_ne->name                  = pack("ne");
	_ne->code_address          = ne;

	_swap->name                = pack("swap");
	_swap->code_address        = swap;

	_if->name                  = pack("if");
	_if->code_address          = if_;
	_if->codeword              = &(_if->code_address);

	_then->name                = pack("then");
	_then->code_address        = then;
	_then->codeword            = &(_then->code_address);

	_for->name                 = pack("for");
	_for->code_address         = for_;

	_next->name                = pack("next");
	_next->code_address        = next_;

	LIST_INSERT_HEAD(&macro_dictionary, _zero_branch, next);
	LIST_INSERT_HEAD(&macro_dictionary, _to_r,        next);
	LIST_INSERT_HEAD(&macro_dictionary, _from_r,      next);
	LIST_INSERT_HEAD(&macro_dictionary, _rdrop,       next);
	LIST_INSERT_HEAD(&macro_dictionary, _minus_one,   next);
	LIST_INSERT_HEAD(&macro_dictionary, _ne,          next);
	LIST_INSERT_HEAD(&macro_dictionary, _swap,        next);
	LIST_INSERT_HEAD(&macro_dictionary, _if,          next);
	LIST_INSERT_HEAD(&macro_dictionary, _then,        next);
	LIST_INSERT_HEAD(&macro_dictionary, _for,         next);
	LIST_INSERT_HEAD(&macro_dictionary, _next,        next);
}

void
literal(void)
{
	cell_t n;

	// Skip literal's address
	printf("Literal: %lx %lx\n", (unsigned long)I, (unsigned long)(*(cell_t *)I)>>5);
	
	// Fetch the number from the next cell
	n = *(cell_t *)I;
	n >>= 5;  // Make it a number again ;-)
	printf("N = %d\n", n);

	// Push the number on the stack
	stack_push(n);

	I++;
	next();
}

void
variable(void)
{
	I++; // Fetch the variable's address from the next cell
	stack_push((cell_t)I); // Push it on the stack
	W--;
}

static void
execute(const struct word_entry *word)
{
	printf("EXEC: %lx -> %lx\n", (unsigned long)word->code_address, *(unsigned long *)word->code_address);
	((FUNCTION_EXEC)*(unsigned long *)word->codeword)();
}



/*
 * Colorful words handling
 */
static void
ignore(const cell_t word)
{
	(void)word; // Avoid an useless warning and do nothing!
}

static void
interpret_forth_word(const cell_t word)
{
	struct word_entry *entry;
printf("IFW: %s\n", unpack(word));
	entry = lookup_word(word, FORTH_DICTIONARY);

	if (entry)
		execute(entry);
}

static void
interpret_big_number(const cell_t number)
{
	(void)number;
}

static void
interpret_number(const cell_t number)
{
	stack_push(number >> 5);
}

static void
compile_word(const cell_t word)
{
	struct word_entry *entry;

	entry = lookup_word(word, MACRO_DICTIONARY);

	if (entry)
	{
		// Execute macro word
		printf("Execute Macro: name = %s, code_address = %p\n", 
				unpack(entry->name), entry->code_address);
		execute(entry);
	}
	else
	{
		entry = lookup_word(word, FORTH_DICTIONARY);

		if (entry)
		{
			// Compile a call to that word
			stack_push((cell_t)entry->code_address);
			comma();
			printf("To compile: %s, %x, at address: %p\n", unpack(entry->name),
					(int)entry->name, h);
		}
	}
}

static void
compile_number(const cell_t number)
{
	stack_push((cell_t)literal);
	comma();

	stack_push(number);
	comma();
}

static void
compile_big_number(const cell_t number)
{
	(void)number;
}

static void
compile_macro(const cell_t word)
{
	struct word_entry *entry;

	entry = lookup_word(word, MACRO_DICTIONARY);

	if (entry)
	{
		// Compile a call to that macro
		printf("Macro: %s -> %lx\n", unpack(word), (unsigned long)entry->code_address);
		stack_push((cell_t)entry->code_address);
		comma();
	}
}

static void
create_word(cell_t word)
{
	struct word_entry *entry;

	entry = calloc(1, sizeof(struct word_entry));

	if (!entry)
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		colorforth_finalize();
		exit(EXIT_FAILURE);
	}

	word &= 0xfffffff0;

	printf("docol: %p\n", docol);
	entry->name         = word;
	entry->code_address = h;
	entry->codeword     = h;

	stack_push((cell_t)docol);
	comma();

	printf("create_word(): at %p, name = %x\n", entry->code_address, 
			(int)entry->name);

	if (selected_dictionary == MACRO_DICTIONARY)
		LIST_INSERT_HEAD(&macro_dictionary, entry, next);
	else
		LIST_INSERT_HEAD(&forth_dictionary, entry, next);
}

static void
variable_word(const cell_t word)
{
	// A variable must be defined in forth dictionary
	forth();

	create_word(word);

	// Variable's handler
	stack_push((cell_t)variable);
	comma();

	// The default value of a variable is 0 (green number)
	stack_push(0);
	comma();

	// Exit
	stack_push((cell_t)exit_word);
	comma();

	// Skip the value for run_block() routine
	i++;
}

/*
 * Initializing and deinitalizing colorForth
 */
static void
colorforth_initialize(void)
{
	code_here = malloc(CODE_HEAP_SIZE);

	if (!code_here)
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		exit(EXIT_FAILURE);
	}

	h = code_here;

	P = h;
	W = h;
	I = h;

	LIST_INIT(&forth_dictionary);
	LIST_INIT(&macro_dictionary);

	// FORTH is the default dictionary
	forth();

	insert_builtins_into_forth_dictionary();
	insert_builtins_into_macro_dictionary();
	dump_dict();
}

static void
colorforth_finalize(void)
{
	struct word_entry *item;

	while ((item = LIST_FIRST(&forth_dictionary)))
	{
		LIST_REMOVE(item, next);
		free(item);
	}

	while ((item = LIST_FIRST(&macro_dictionary)))
	{
		LIST_REMOVE(item, next);
		free(item);
	}

	free(code_here);
}


int main(int argc, char *argv[])
{
	int fd;
	struct stat sbuf;

	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s block-file\n", argv[0]);
		return EXIT_FAILURE;
	}

	if ((fd = open(argv[1], O_RDONLY)) == -1)
	{
		perror("open");
		exit(EXIT_FAILURE);
	}

	if (stat(argv[1], &sbuf) == -1)
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

	// Load block 0
	stack_push(6);
	load();

	dot_s();

	munmap(blocks, sbuf.st_size);
	close(fd);
	colorforth_finalize();

	return EXIT_SUCCESS;
}

