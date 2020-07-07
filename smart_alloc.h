#define NFREELISTS 16;

enum {ALIGN = 8};
enum {MAX_BYTES = 128};
enum {NFREELISTS = 16};


union Obj {
        union Obj* M_free_list_link;
        char M_client_data[1];    /* The client sees this.        */
 };

static Obj*  S_free_list[NFREELISTS]; 
static char* S_start_free;
static char* S_end_free;
static size_t S_heap_size;

void* alloc(int n);


static  size_t S_freelist_index(size_t __bytes) 
{
	return (((__bytes) + (size_t)ALIGN-1)/(size_t)ALIGN - 1); //decide to which  range{1-16}
}

static size_t S_round_up(size_t __bytes) 
{ 
	return (((__bytes) + (size_t) ALIGN-1) & ~((size_t) ALIGN - 1)); 
} 