#define M 6

typedef struct Mem
{
	struct Mem *prev;
	struct Mem *next;
	int tag;
	int kval;
}Mem;

typedef struct List {
	struct Mem *Head;
	int size;
}List[M];
char Memory[4*8*1024*1024];
void InitSpace(FreeList avail);

WORD* AllocBuddy(FreeList avail, int n);
static Mem* Buddy(Mem *p);