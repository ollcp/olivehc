/**
 *
 * Use index as pointer, in order to save memory.
 *
 * Auther: Wu Bingzheng
 *
 **/

#ifndef _IDX_POINTER_H_
#define _IDX_POINTER_H_

#define IPT_ARRAY_SIZE 4096

typedef struct {
	short	lowest;
	void	*array[IPT_ARRAY_SIZE];
} idx_pointer_t;

#define IDX_POINTER_INIT() {0, {0,}}

static inline void *idx_pointer_get(idx_pointer_t *ipt, short index)
{
	return ipt->array[index];
}

static inline short idx_pointer_add(idx_pointer_t *ipt, void *pointer)
{
	short index = ipt->lowest;

	if(index >= IPT_ARRAY_SIZE) {
		return -1;
	}

	while(ipt->array[index] != NULL) {
		index++;
	}
	ipt->lowest = index + 1;

	ipt->array[index] = pointer;
	return index;
}

static inline void idx_pointer_delete(idx_pointer_t *ipt, short index)
{
	ipt->array[index] = NULL;
	if(index < ipt->lowest) {
		ipt->lowest = index;
	}
}

#endif
