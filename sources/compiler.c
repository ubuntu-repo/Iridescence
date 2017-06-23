/*
 * Copyright (c) 2015-2017 Konstantin Tcholokachvili
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
#include <assert.h>

#include "colorforth.h"

#define CODE_HEAP_SIZE (1024 * 100)	// 100 Kb
#define STACK_SIZE     8

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
long stack[STACK_SIZE];
long *tos = start_of(stack);	// Top Of Stack

/* Return stack */
unsigned long rstack[STACK_SIZE];
unsigned long *rtos = start_of(rstack);

/*
 * Global variables
 */
unsigned long *code_here;
unsigned long *h;			// Code is inserted here
bool          selected_dictionary;
cell_t        *blocks;			// Manage looping over the code contained in blocks
unsigned long *IP;			// Instruction Pointer

LIST_HEAD(, word_entry) forth_dictionary;
LIST_HEAD(, word_entry) macro_dictionary;

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

int
get_code_index(const char letter)
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
	assert(word_length != 0);

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
			text[i] += code[(((nibble ^ 0xc) << 1) | ((coded & HIGHBIT) > 0))];
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

void
NEXT(void)
{
	IP++;
	((FUNCTION_EXEC)*IP)();
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
	long n = stack_pop();
	run_block(n);
}

void loads(void)
{
	long i, j;

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

void exit_definition(void)
{
	long n = rpop();
	printf(" => Exit\n");
	(void)n;
}

void add(void)
{
	long n = stack_pop();
	*tos += n;
}

void one_complement(void)
{
	long n = stack_pop();
	*tos = ~n;
}

void multiply(void)
{
	long n = stack_pop();
	*tos *= n;
}

void divide(void)
{
	long n = stack_pop();
	*tos /= n;
}

void modulo(void)
{
	long n = stack_pop();
	*tos %= n;
}

void lt(void)
{
	long n = stack_pop();
	*tos = (*tos < n) ? FORTH_TRUE : FORTH_FALSE;
}

void gt(void)
{
	long n = stack_pop();
	*tos = (*tos > n) ? FORTH_TRUE : FORTH_FALSE;
}

void ge(void)
{
	long n = stack_pop();
	*tos = (*tos >= n) ? FORTH_TRUE : FORTH_FALSE;
}

void ne(void)
{
	long n = stack_pop();
	*tos = (*tos != n) ? FORTH_TRUE : FORTH_FALSE;
}

void eq(void)
{
	long n = stack_pop();
	*tos = (*tos == n) ? FORTH_TRUE : FORTH_FALSE;
}

void le(void)
{
	long n = stack_pop();
	*tos = (*tos <= n) ? FORTH_TRUE : FORTH_FALSE;
}

void and(void)
{
	long a = stack_pop();
	long b = stack_pop();
	long result = a & b;
	stack_push(result);
}

void negate(void)
{
	long n = stack_pop();
	stack_push(-n);
}

// It is actually a xor.
void or(void)
{
	long a = stack_pop();
	long b = stack_pop();
	long result = a ^ b;
	stack_push(result);
}

void dup_word(void)
{
	long n = *tos;
	stack_push(n);

	NEXT();
}

void drop(void)
{
	(void)stack_pop(); // Cast to avoid a warning about not used computed value
}

void nip(void)
{
	long n = stack_pop();
	(void)stack_pop();
	stack_push(n);
}

void over(void)
{
	long n = nos;
	stack_push(n);
}

void swap(void)
{
	long top = *tos;
	*tos = nos;
	nos = top;
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
	long address = stack_pop();
	long value   = stack_pop();

	*(long *)address = value;
}

void fetch(void)
{
	long address = stack_pop();
	long value   = *(long *)address;

	stack_push(value);
}

void here(void)
{
	stack_push((long)h);
}

void zero_branch(void)
{
	long n = stack_pop();

	if (n == FORTH_TRUE)
		IP++;
	else
		IP = (unsigned long *)*IP;

	NEXT();
}

void if_(void)
{
	stack_push((long)zero_branch);
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

void for_aux(void)
{
	long n = stack_pop();
	rpush(n);

	NEXT();
}

void next_aux(void)
{
	long n = rpop();
	long addr = rpop();

	rpush(addr);

	n--;
	rpush(n);

	if (n > 0)
	{
		IP = (unsigned long *)addr;
		((FUNCTION_EXEC)*IP)();
	}
}

void for_(void)
{
	stack_push((long)for_aux);
	comma();

	rpush((long)h);
}

void next_(void)
{
	stack_push((long)next_aux);
	comma();
}

void rdrop(void)
{
	(void)rpop();
}

void dot(void)
{
	printf("%d ", (int)stack_pop());
}

void i_word(void)
{
	long n;

	n = rpop();
	stack_push(n);
}


/*
 * Helper functions
 */
void
dump_dict(void)
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

void
do_word(const cell_t word)
{
	uint8_t color = (int)word & 0x0000000f;

	if (color == 2 || color == 5 || color == 6 || color == 8 || color == 15)
	{
		printf("Color = %1d, Word = %10d, packed = %8x\n",
				color, word >> 5, word);
	}
	else if (color != 0)
	{
		printf("Color = %1d, Word = %10s, packed = %8x\n",
				color, unpack(word), word);
	}

	(*color_word_action[color])(word);
}

void
run_block(const cell_t n)
{
	unsigned long start, limit, i;

	start = n * 256;     // Start executing block from here...
	limit = (n+1) * 256; // ...to this point.

	for (i = start; i < limit-1; i++)
		do_word(blocks[i]);
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
		*_exit, *_store, *_fetch, *_add, *_one_complement, *_mult,
		*_div, *_ne, *_dup, *_drop, *_nip, *_negate, *_dot, *_here, *_i;

	_comma		= calloc(1, sizeof(struct word_entry));
	_load		= calloc(1, sizeof(struct word_entry));
	_loads		= calloc(1, sizeof(struct word_entry));
	_forth		= calloc(1, sizeof(struct word_entry));
	_macro		= calloc(1, sizeof(struct word_entry));
	_exit		= calloc(1, sizeof(struct word_entry));
	_store		= calloc(1, sizeof(struct word_entry));
	_fetch		= calloc(1, sizeof(struct word_entry));
	_add		= calloc(1, sizeof(struct word_entry));
	_one_complement	= calloc(1, sizeof(struct word_entry));
	_mult		= calloc(1, sizeof(struct word_entry));
	_div		= calloc(1, sizeof(struct word_entry));
	_ne		= calloc(1, sizeof(struct word_entry));
	_dup		= calloc(1, sizeof(struct word_entry));
	_drop		= calloc(1, sizeof(struct word_entry));
	_nip		= calloc(1, sizeof(struct word_entry));
	_negate		= calloc(1, sizeof(struct word_entry));
	_dot		= calloc(1, sizeof(struct word_entry));
	_here		= calloc(1, sizeof(struct word_entry));
	_i		= calloc(1, sizeof(struct word_entry));

	if (!(_comma && _load && _loads && _forth && _macro && _exit
			&& _store && _fetch && _add && _one_complement
			&& _mult && _div && _ne && _dup && _drop && _nip
			&& _negate && _dot && _here && _i))
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		free(code_here);
		exit(EXIT_FAILURE);
	}

	_comma->name		= pack(",");
	_comma->code_address	= comma;
	_comma->codeword	= &(_comma->code_address);

	_load->name		= pack("load");
	_load->code_address	= load;
	_load->codeword		= &(_load->code_address);

	_loads->name		= pack("loads");
	_loads->code_address	= loads;
	_loads->codeword	= &(_loads->code_address);

	_forth->name		= pack("forth");
	_forth->code_address	= forth;
	_forth->codeword	= &(_forth->code_address);

	_macro->name		= pack("macro");
	_macro->code_address	= macro;
	_macro->codeword	= &(_macro->code_address);

	_exit->name		= pack(";");
	_exit->code_address	= exit_definition;
	_exit->codeword		= &(_exit->code_address);

	_store->name		= pack("!");
	_store->code_address	= store;
	_store->codeword	= &(_store->code_address);

	_fetch->name		= pack("@");
	_fetch->code_address	= fetch;
	_fetch->codeword	= &(_fetch->code_address);

	_add->name		= pack("+");
	_add->code_address	= add;
	_add->codeword		= &(_add->code_address);

	_one_complement->name		= pack("-");
	_one_complement->code_address	= one_complement;
	_one_complement->codeword	= &(_one_complement->code_address);

	_mult->name		= pack("*");
	_mult->code_address	= multiply;
	_mult->codeword		= &(_mult->code_address);

	_div->name		= pack("/");
	_div->code_address	= divide;
	_div->codeword		= &(_div->code_address);

	_ne->name		= pack("ne");
	_ne->code_address	= ne;

	_dup->name		= pack("dup");
	_dup->code_address	= dup_word;
	_dup->codeword		= &(_dup->code_address);

	_drop->name		= pack("drop");
	_drop->code_address	= drop;
	_drop->codeword		= &(_drop->code_address);

	_nip->name		= pack("nip");
	_nip->code_address	= nip;
	_nip->codeword		= &(_nip->code_address);

	_negate->name		= pack("negate");
	_negate->code_address	= negate;
	_negate->codeword	= &(_negate->code_address);

	_dot->name		= pack(".");
	_dot->code_address	= dot;
	_dot->codeword		= &(_dot->code_address);

	_here->name		= pack("here");
	_here->code_address	= here;
	_here->codeword		= &(_here->code_address);

	_i->name		= pack("i");
	_i->code_address	= i_word;
	_i->codeword		= &(_i->code_address);

	LIST_INSERT_HEAD(&forth_dictionary, _comma,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _load,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _loads,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _forth,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _macro,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _exit,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _store,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _fetch,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _add,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _one_complement,	next);
	LIST_INSERT_HEAD(&forth_dictionary, _mult,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _div,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _ne,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _dup,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _drop,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _nip,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _negate,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _dot,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _here,		next);
	LIST_INSERT_HEAD(&forth_dictionary, _i,			next);
}

static void
insert_builtins_into_macro_dictionary(void)
{
	struct word_entry *_rdrop, *_ne, *_swap, *_if, *_then, *_for, *_next;

	_rdrop       = calloc(1, sizeof(struct word_entry));
	_ne          = calloc(1, sizeof(struct word_entry));
	_swap        = calloc(1, sizeof(struct word_entry));
	_if          = calloc(1, sizeof(struct word_entry));
	_then        = calloc(1, sizeof(struct word_entry));
	_for         = calloc(1, sizeof(struct word_entry));
	_next        = calloc(1, sizeof(struct word_entry));

	if (!(_rdrop && _ne && _swap && _if && _then && _for && _next))
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		free(code_here);
		exit(EXIT_FAILURE);
	}

	_rdrop->name               = pack("rdrop");
	_rdrop->code_address       = rdrop;
	_rdrop->codeword           = &(_rdrop->code_address);

	_ne->name                  = pack("ne");
	_ne->code_address          = ne;
	_ne->codeword              = &(_ne->code_address);

	_swap->name                = pack("swap");
	_swap->code_address        = swap;
	_swap->codeword            = &(_swap->code_address);

	_if->name                  = pack("if");
	_if->code_address          = if_;
	_if->codeword              = &(_if->code_address);

	_then->name                = pack("then");
	_then->code_address        = then;
	_then->codeword            = &(_then->code_address);

	_for->name                 = pack("for");
	_for->code_address         = for_;
	_for->codeword             = &(_for->code_address);

	_next->name                = pack("next");
	_next->code_address        = next_;
	_next->codeword            = &(_next->code_address);

	LIST_INSERT_HEAD(&macro_dictionary, _rdrop,	next);
	LIST_INSERT_HEAD(&macro_dictionary, _ne,	next);
	LIST_INSERT_HEAD(&macro_dictionary, _swap,	next);
	LIST_INSERT_HEAD(&macro_dictionary, _if,	next);
	LIST_INSERT_HEAD(&macro_dictionary, _then,	next);
	LIST_INSERT_HEAD(&macro_dictionary, _for,	next);
	LIST_INSERT_HEAD(&macro_dictionary, _next,	next);
}

void
literal(void)
{
	long n;

	// Skip literal's address
	IP++;

	// Fetch the number from the next cell
	n = *(long *)IP;
	n >>= 5;  // Make it a number again ;-)

	// Push the number on the stack
	stack_push(n);

	NEXT();
}

void
variable(void)
{
	IP++; // Fetch the variable's address from the next cell
	stack_push((long)IP); // Push it on the stack
}

static void
execute(const struct word_entry *word)
{
	printf("EXEC: %lx -> %lx\n", (unsigned long)word->code_address, *(unsigned long *)word->code_address);
	IP = word->code_address;
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
	struct word_entry *entry = lookup_word(word, MACRO_DICTIONARY);

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
			stack_push((long)entry->code_address);
			comma();
			printf("To compile: %s, %x, at address: %p\n", unpack(entry->name),
					(int)entry->name, h);
		}
	}
}

static void
compile_number(const cell_t number)
{
	stack_push((long)literal);
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
		stack_push((long)entry->code_address);
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

	entry->name         = word;
	entry->code_address = h;
	entry->codeword     = h;

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
	stack_push((long)variable);
	comma();

	// The default value of a variable is 0 (green number)
	stack_push(0);
	comma();
}

/*
 * Initializing and deinitalizing colorForth
 */
void
colorforth_initialize(void)
{
	code_here = malloc(CODE_HEAP_SIZE);

	if (!code_here)
	{
		fprintf(stderr, "Error: Not enough memory!\n");
		exit(EXIT_FAILURE);
	}

	h = code_here;

	LIST_INIT(&forth_dictionary);
	LIST_INIT(&macro_dictionary);

	// FORTH is the default dictionary
	forth();

	insert_builtins_into_forth_dictionary();
	insert_builtins_into_macro_dictionary();
	dump_dict();
}

void
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
