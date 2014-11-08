#ifndef _LINUX_STDDEF_H
#define _LINUX_STDDEF_H

#include <uapi/linux/stddef.h>


#undef NULL
#define NULL ((void *)0)

enum {
	false	= 0,
	true	= 1
};

#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE,MEMBER) __compiler_offsetof(TYPE,MEMBER)
#else
/*!!C 0번지를 TYPE으로 TYPE Casting 하여 원하는 멤버의 offset 을 구한다. */ 
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif
#endif
