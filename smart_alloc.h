 #define _NFREELISTS 16;
 union _Obj {
        union _Obj* _M_free_list_link;
        char _M_client_data[1];    /* The client sees this.        */
  };
static _Obj* __STL_VOLATILE _S_free_list[_NFREELISTS]; 

static  size_t _S_freelist_index(size_t __bytes) {
        return (((__bytes) + (size_t)_ALIGN-1)/(size_t)_ALIGN - 1); //decide to which  range{1-16}
  }
static size_t
  _S_round_up(size_t __bytes) 
    { return (((__bytes) + (size_t) _ALIGN-1) & ~((size_t) _ALIGN - 1)); }  //8 bei

  static char* _S_start_free;
  static char* _S_end_free;
  static size_t _S_heap_size;

  enum {_ALIGN = 8};
  enum {_MAX_BYTES = 128};
  enum {_NFREELISTS = 16}; 

_S_chunk_alloc(size_t __size,int& __nobjs)
{
	char* __result;
    size_t __total_bytes = __size * __nobjs;
    size_t __bytes_left = _S_end_free - _S_start_free;
    if (__bytes_left >=__total_bytes )
    {
    	__result = _S_start_free;
    	_S_start_free += __total_bytes;
    	return __result;
    }
    else if (__bytes_left >= __size)
    {
    	__nobjs = (int)(__bytes_left/__size);
    	__total_bytes = __size * __nobjs;
    	__result = _S_start_free;
    	_S_start_free += __total_bytes;
    	return __result;
    }
    else
    {
    	size_t __bytes_to_get = 2 * __total_bytes + _S_round_up(_S_heap_size >> 4);
    	if (__bytes_left > 0) 
    	{
    	 	_Obj** __my_free_list = _S_free_list + _S_freelist_index(__bytes_left);
    	 	((_Obj*)_S_start_free) -> _M_free_list_link = *__my_free_list;
            *__my_free_list = (_Obj*)_S_start_free;
    	}
    	_S_start_free = (char*)malloc(__bytes_to_get);
    	if (0 == _S_start_free)
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
    	}
    	_S_heap_size += __bytes_to_get;
    	_S_end_free = _S_start_free + __bytes_to_get;
    	return(_S_chunk_alloc(__size, __nobjs));
    }
}
_S_refill(size_t __n)
{
	 int __nobjs = 20;
	char* __chunk = _S_chunk_alloc(__n, __nobjs);
	_Obj** __my_free_list;
    _Obj* __result;
    _Obj* __current_obj;
    _Obj* __next_obj;
    int __i;
    if (1 == __nobjs) return(__chunk);
    __my_free_list = _S_free_list + _S_freelist_index(__n);
    __result = (_Obj*)__chunk;
    *__my_free_list = __next_obj = (_Obj*)(__chunk + __n);

	for (__i = 1; ; __i++) {
        __current_obj = __next_obj;
        __next_obj = (_Obj*)((char*)__next_obj + __n);
        if (__nobjs - 1 == __i) {
            __current_obj -> _M_free_list_link = 0;
            break;
        } else {
            __current_obj -> _M_free_list_link = __next_obj;
        }
      }
      return(__result);


}

alloc(int n)
  {
  	void *ret = NULL;
  	if (n > 128)
  	{
  		max bluck alloc
  	}
  	else
  	{
  		_Obj** my_free_list =  _S_free_list + _S_freelist_index(n);
  		_Obj* __result = *my_free_list;
  		if (__result == NULL)
  			ret = _S_refill(_S_round_up(__n));
  		else {
        *__my_free_list = __result -> _M_free_list_link;
        __ret = __result;
      }
  	}
  	return __ret;	
  }