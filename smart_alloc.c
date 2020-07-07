void* S_chunk_alloc(size, nobjs)
{
	char* result;
	size_t total_bytes = size * nobjs;
	size_t bytes_left = S_end_free - S_start_free;
	if (bytes_left >= total_bytes )
	{
		result = S_start_free;
		S_start_free += total_bytes;
		return result;
	}
	else if(bytes_left >= size)
	{
		nobjs = (int)(bytes_left/size);
		total_bytes = size * nobjs;
		result = S_start_free;
		S_start_free += total_bytes;
		return result;
	}
	else
	{
		size_t bytes_to_get = 2 * total_bytes + S_round_up(S_heap_size >> 4);
		if (bytes_left > 0)
		{
			Obj** my_free_list = S_free_list + S_freelist_index(bytes_left);
    	 	((_Obj*)S_start_free) -> M_free_list_link = *my_free_list;
            *my_free_list = (_Obj*)S_start_free;
		}
		S_start_free = (char*)malloc(bytes_to_get);
		/*if (0 == S_start_free)
    	{
    		size_t __i;
    		_Obj** __my_free_list;
    		_Obj* __p;
    		for (__i = __size;__i <= (size_t) _MAX_BYTES;__i += (size_t) _ALIGN) {
    			__my_free_list = _S_free_list + _S_freelist_index(__i);
                __p = *__my_free_list;
                if (0 != __p) {
                    *__my_free_list = __p -> _M_free_list_link;
                    _S_start_free = (char*)__p;
                    _S_end_free = _S_start_free + __i;
                    return(_S_chunk_alloc(__size, __nobjs));
                    // Any leftover piece will eventually make it to the
                    // right free list.
                }
            }
            _S_end_free = 0;	// In case of exception.
            _S_start_free = (char*)malloc_alloc::allocate(__bytes_to_get);
    	}*/
    	S_heap_size += bytes_to_get;
    	S_end_free = S_start_free + bytes_to_get;
    	return(S_chunk_alloc(size, nobjs));
	}
};	



void* S_refill(size_t n)
{
	int nobjs = 20;
	char* chunk = S_chunk_alloc(n, nobjs);
	Obj** my_free_list;
	Obj* result;
	Obj* current_obj;
    Obj* next_obj;
    int i;
    if (1 == nobjs) 
    	return(chunk);

    my_free_list = S_free_list + S_freelist_index(n);
    result = (Obj*)chunk;
    *my_free_list = next_obj = (Obj*)(chunk + n);

	for (i = 1; ; i++) 
	{
        current_obj = next_obj;
        next_obj = (Obj*)((char*)next_obj + n);
        if (nobjs - 1 == i) 
        {
            current_obj -> M_free_list_link = 0;
            break;
        } 
        else 
        {
            current_obj -> M_free_list_link = next_obj;
        }
    }
    return(result);
}

void* alloc(int n)
{
	void *ret = NULL;
  	if (n > 128)
  	{
  		max bluck alloc
  		??????????????
  		???????????????
  		?????????????????
  	}
  	else
  	{
  		Obj** my_free_list = S_free_list + S_freelist_index(n);
  		Obj* result = *my_free_list;		
  		if (result == NULL)
  			ret = S_refill(S_round_up(n));
  		else
  		{
  			*my_free_list = result -> M_free_list_link;
  			ret = result;
  		}
  	}
  	return ret;	
}

static void deallocate(void* p, size_t n)
{
    if (n > (size_t) MAX_BYTES)
      malloc_alloc::deallocate(p, n);
    else 
   	{
    	Obj**  my_free_list = S_free_list + S_freelist_index(n);
    	Obj* q = (Obj*)p;

      // acquire lock
#       ifndef _NOTHREADS
      /*REFERENCED*/
      _Lock __lock_instance;
#       endif /* _NOTHREADS */
      q -> M_free_list_link = *my_free_list;
      *my_free_list = q;
      // lock is released here
    } 
}