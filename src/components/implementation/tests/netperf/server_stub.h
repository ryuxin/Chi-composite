#ifndef SERVER_STUB_H
#define SERVER_STUB_H

#define SERVER_FN(fn) __inv_test_##fn

#define DECLARE_INTERFACE(fn)               \
	void *                                  \
	SERVER_FN(fn)(int a, int b, int c);

#define IMPL_INTERFACE(fn)              \
.globl SERVER_FN(fn);                 	\
.type  SERVER_FN(fn), @function;		\
SERVER_FN(fn):							\
	movl %ebp, %esp;					\
	xor %ebp, %ebp;						\
	pushl %edi;							\
	pushl %esi;							\
	pushl %ebx;							\
	call fn;							\
	addl $12, %esp;						\
	movl %eax, %ecx;					\
	movl $RET_CAP, %eax;				\
	sysenter;

#endif /* SERVER_STUB_H */
