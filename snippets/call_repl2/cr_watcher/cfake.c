#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/cdev.h>

#include <asm/uaccess.h>	/* copy_*_user */

#include <asm/insn.h>		/* instruction decoder machinery */

MODULE_AUTHOR("Eugene");

/* Because this module uses the instruction decoder which is distributed
 * under GPL, I have no choice but to distribute this module under GPL too.
 * */
MODULE_LICENSE("GPL");

/* We need find_module(), etc.
 * */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
#  error "Kernel version 2.6.30 or newer is required for this module to run."
#endif

/* ========== Module Parameters ========== */
/* Name of the module to analyse. It can be passed to 'insmod' as 
 * an argument, for example,
 * 	/sbin/insmod cr_watcher.ko target="vboxvfs"
 * */
static char* target = "cr_target";
module_param(target, charp, S_IRUGO);
/* ================================================================ */

/* The module being analysed. */
struct module* mod = NULL;

/* ================================================================ */
/* Helpers */

/* CALL_ADDR_FROM_OFFSET()
 * 
 * Calculate the memory address being the operand of a given instruction 
 * (usually, 'call'). 
 *   'insn_addr' is the address of the instruction itself,
 *   'insn_len' is length of the instruction in bytes,
 *   'offset' is the offset of the destination address from the first byte
 *   past the instruction.
 * 
 * For x86_64 architecture, the offset value is sign-extended here first.
 * 
 * "Intel x86 Instruction Set Reference" states the following 
 * concerning 'call rel32':
 * 
 * "Call near, relative, displacement relative to next instruction.
 * 32-bit displacement sign extended to 64 bits in 64-bit mode."
 * *****************************************************************
 * 
 * CALL_OFFSET_FROM_ADDR()
 * 
 * The reverse of CALL_ADDR_FROM_OFFSET: calculates the offset value
 * to be used in 'call' instruction given the address and length of the
 * instruction and the address of the destination function.
 * 
 * */
#ifdef CONFIG_X86_64
#  define CALL_ADDR_FROM_OFFSET(insn_addr, insn_len, offset) \
	(void*)((s64)(insn_addr) + (s64)(insn_len) + (s64)(s32)(offset))

#else /* CONFIG_X86_32 */
#  define CALL_ADDR_FROM_OFFSET(insn_addr, insn_len, offset) \
	(void*)((u32)(insn_addr) + (u32)(insn_len) + (u32)(offset))
#endif

#define CALL_OFFSET_FROM_ADDR(insn_addr, insn_len, dest_addr) \
	(u32)(dest_addr - (insn_addr + (u32)insn_len))


/* ================================================================ */
/* Declarations of replacement functions (should be the same as for 
 * the target functions but with a different name.) 
 * */
void*
repl___kmalloc(size_t size, gfp_t flags);

void 
repl_kfree(const void* p);

void*
repl_kmem_cache_alloc(struct kmem_cache* mc, gfp_t flags);

void 
repl_kmem_cache_free(struct kmem_cache* mc, void* p);

long 
repl_copy_from_user(void* to, const void __user * from, unsigned long n);

long 
repl_copy_to_user(void __user * to, const void* from, unsigned long n);
/* ================================================================ */

/* Names and addresses of the functions of interest */
static void* target_func_addrs[] = {
	(void*)&__kmalloc,
	(void*)&kfree,
	(void*)&kmem_cache_alloc,
	(void*)&kmem_cache_free,
	(void*)&copy_from_user,
	(void*)&copy_to_user
};

/* Addresses of the replacement functions */
static void* repl_func_addrs[] = {
	(void*)&repl___kmalloc,
	(void*)&repl_kfree,
	(void*)&repl_kmem_cache_alloc,
	(void*)&repl_kmem_cache_free,
	(void*)&repl_copy_from_user,
	(void*)&repl_copy_to_user
};

/* ================================================================ */
/* Decode and process the instruction ('c_insn') at
 * the address 'kaddr' - see the description of do_process_area for details. 
 * 
 * Check if we get past the end of the buffer [kaddr, end_kaddr)
 * 
 * The function returns the length of the instruction in bytes. 
 * 0 is returned in case of failure.
 */
static unsigned int
do_process_insn(struct insn* c_insn, void* kaddr, void* end_kaddr,
	void** from_funcs, void** to_funcs, unsigned int nfuncs)
{
	/* ptr to the 32-bit offset argument in the instruction */
	u32* offset = NULL; 
	
	/* address of the function being called */
	void* addr = NULL;
	
	static const unsigned char op = 0xe8; /* 'call <offset>' */
	
	int i;
	
	BUG_ON(from_funcs == NULL || to_funcs == NULL);
	
	/* Decode the instruction and populate 'insn' structure */
	kernel_insn_init(c_insn, kaddr);
	insn_get_length(c_insn);
	
	if (c_insn->length == 0)
	{
		return 0;
	}
	
	if (kaddr + c_insn->length > end_kaddr)
	{
	/* Note: it is OK to stop at 'end_kaddr' but no further */
		printk( KERN_WARNING "[cr_watcher] "
	"Instruction decoder stopped past the end of the section.\n");
	}
		
/* This call may be overkill as insn_get_length() probably has to decode 
 * the instruction completely.
 * Still, to operate safely, we need insn_get_opcode() before we can access
 * c_insn->opcode. 
 * The call is cheap anyway, no re-decoding is performed.
 */
	insn_get_opcode(c_insn); 
	if (c_insn->opcode.value != op)
	{
		/* Not a 'call' instruction, nothing to do. */
		return c_insn->length;
	}
	
/* [NB] For some reason, the decoder stores the argument of 'call' and 'jmp'
 * as 'immediate' rather than 'displacement' (as Intel manuals name it).
 * May be it is a bug, may be it is not. 
 * Meanwhile, I'll call this value 'offset' to avoid confusion.
 */

	/* Call this before trying to access c_insn->immediate */
	insn_get_immediate(c_insn);
	if (c_insn->immediate.nbytes != 4)
	{
		printk( KERN_WARNING "[cr_watcher] At 0x%p: "
	"opcode: 0x%x, "
	"immediate field is %u rather than 32 bits in size; "
	"insn.length = %u, insn.imm = %u, off_immed = %d\n",
			kaddr,
			(unsigned int)c_insn->opcode.value,
			8 * (unsigned int)c_insn->immediate.nbytes,
			c_insn->length,
			(unsigned int)c_insn->immediate.value,
			insn_offset_immediate(c_insn));
		
		return c_insn->length;
	}
	
	offset = (u32*)(kaddr + insn_offset_immediate(c_insn));
	addr = CALL_ADDR_FROM_OFFSET(kaddr, c_insn->length, *offset);
	
	/* Check if one of the functions of interest is called */
	for (i = 0; i < nfuncs; ++i)
	{
		if (addr == from_funcs[i])
		{
		/* Change the address of the function to be called */
			BUG_ON(to_funcs[i] == NULL);
			
			printk( KERN_INFO "[cr_watcher] At 0x%p: "
		"changing address 0x%p to 0x%p (displ: 0x%x to 0x%x)\n",
				kaddr,
				from_funcs[i], 
				to_funcs[i],
				(unsigned int)(*offset),
				(unsigned int)CALL_OFFSET_FROM_ADDR(
					kaddr, c_insn->length, to_funcs[i])
			);
			
			*offset = CALL_OFFSET_FROM_ADDR(
				kaddr, 
				c_insn->length,
				to_funcs[i]
			);
			
			break;
		}
	}
	
	return c_insn->length;
}

/* Process the instructions in [kbeg, kend) area.
 * Each 'call' instruction calling one of the target functions will be 
 * changed so as to call the corresponding replacement function instead.
 * The addresses of target and replacement fucntions are given in
 * 'from_funcs' and 'to_funcs', respectively, the number of the elements
 * to process in these arrays being 'nfuncs'.
 * For each i=0..nfuncs-1, from_funcs[i] corresponds to to_funcs[i].
 */
static void
do_process_area(void* kbeg, void* kend, 
	void** from_funcs, void** to_funcs, unsigned int nfuncs)
{
	struct insn c_insn; /* current instruction */
	unsigned int i;
	void* pos = NULL;
	
	/* TODO: provide assert-like wrapper */
	BUG_ON(kbeg == NULL);
	BUG_ON(kend == NULL);
	BUG_ON(kend < kbeg);
		
	pos = kbeg;
	for (i = 0; ; ++i)
	{
		unsigned int len;
		unsigned int k;

		len = do_process_insn(&c_insn, pos, kend,
			from_funcs, to_funcs, nfuncs);
		if (len == 0)	
		{
			printk( KERN_WARNING "[cr_watcher] "
			"do_process_insn() returned 0\n");
			break;
		}

		if (pos + len > kend)
		{
			break;
		}
		
/* If the decoded instruction contains only zero bytes (this is the case,
 * for example, for one flavour of 'add'), skip to the first nonzero byte
 * after it. 
 * This is to avoid problems if there are two or more sections in the area
 * being analysed. Such situation is very unlikely - still have to find 
 * the example. Note that ctors and dtors seem to be placed to the same 
 * '.text' section as the ordinary functions ('.ctors' and '.dtors' sections
 * probably contain just the lists of their addresses or something similar).
 * 
 * As we are not interested in instrumenting 'add' or the like, we can skip 
 * to the next instruction that does not begin with 0 byte. If we are 
 * actually past the last instruction in the section, we get to the next 
 * section or to the end of the area this way which is what we want in this
 * case.
 */
		for (k = 0; k < len; ++k)
		{
			if (*((unsigned char*)pos + k) != 0) 
			{
				break;
			}
		}
		pos += len;
		
		if (k == len) 
		{
			/* all bytes are zero, skip the following 0s */
			while (pos < kend && *(unsigned char*)pos == 0)
			{
				++pos;
			}
		}

		if (pos >= kend)
		{
			break;
		}
	}
	
	return;
}

/* Replace all calls to to the target functions with calls to the 
 * replacement-functions in the module. 
 */
static void 
replace_calls_in_module(struct module* mod)
{
	BUG_ON(mod == NULL);
	BUG_ON(mod->module_core == NULL);
	
	if (mod->module_init != NULL)
	{
		printk( KERN_INFO "[cr_watcher] Module \"%s\", "
		"processing \"init\" area\n",
			module_name(mod));
			
		do_process_area(mod->module_init, 
			mod->module_init + mod->init_text_size,
			&target_func_addrs[0],
			&repl_func_addrs[0],
			ARRAY_SIZE(target_func_addrs));
	}

	printk( KERN_INFO "[cr_watcher] Module \"%s\", "
		"processing \"core\" area\n",
		module_name(mod));
		
	do_process_area(mod->module_core, 
		mod->module_core + mod->core_text_size,
		&target_func_addrs[0],
		&repl_func_addrs[0],
		ARRAY_SIZE(target_func_addrs));
	return;
}

/* Revert all changed 'call' instructions to the original state. */
static void 
restore_calls_in_module(struct module* mod)
{
	BUG_ON(mod == NULL);
	BUG_ON(mod->module_core == NULL);
	
	if (mod->module_init != NULL)
	{
		printk( KERN_INFO "[cr_watcher] Module \"%s\", "
		"restoring \"init\" area\n",
			module_name(mod));
			
		do_process_area(mod->module_init, 
			mod->module_init + mod->init_text_size,
			&repl_func_addrs[0],
			&target_func_addrs[0],
			ARRAY_SIZE(target_func_addrs));
	}
	
	printk( KERN_INFO "[cr_watcher] Module \"%s\", "
		"restoring \"core\" area\n",
		module_name(mod));
		
	do_process_area(mod->module_core, 
		mod->module_core + mod->core_text_size,
		&repl_func_addrs[0],
		&target_func_addrs[0],
		ARRAY_SIZE(target_func_addrs));
	return;
}

/* ================================================================== */

static void
cfake_cleanup_module(void)
{
	if (mod != NULL)
	{
		module_put(mod);
		
		printk(KERN_INFO "[cr_watcher] "
	"Restoring call addresses in \"%s\" module" "\n",
			module_name(mod));
		restore_calls_in_module(mod);
	}
	printk(KERN_INFO "[cr_watcher] Cleanup successful\n");
	return;
}

static int __init
cfake_init_module(void)
{
	int result;
	BUG_ON(	ARRAY_SIZE(target_func_addrs) != 
		ARRAY_SIZE(repl_func_addrs));

	printk(KERN_INFO "[cr_watcher] Initializing\n");
	
	/* Ensure noone else meddles with module structures now.
	 * Holding this mutex is necessary as we are going to call
	 * find_module() */
	if (mutex_lock_interruptible(&module_mutex) != 0) {
		result = -EINTR;
		goto fail;
	}
	
	mod = find_module(target);
	if (mod == NULL) {
		printk(	KERN_WARNING "[cr_watcher] "
		"Not found target module \"%s\"\n",
			target);
		result = -ENOENT;
		goto fail_unlock;
	}
	
	/* Increase reference count for the target module to ensure
	 * it will not be unloaded before we are done with it.
	 */
	if (try_module_get(mod) == 0)
	{
		printk(	KERN_WARNING "[cr_watcher] "
		"Failed to add reference count for target module \"%s\"\n",
			target);
		
		mod = NULL;
		result = -ENOENT;
		goto fail_unlock;
	}
	
	replace_calls_in_module(mod);
		
	mutex_unlock(&module_mutex);
	return 0; /* success */

fail_unlock:
	mutex_unlock(&module_mutex);
	
fail:
	cfake_cleanup_module();
	return result;
}

module_init(cfake_init_module);
module_exit(cfake_cleanup_module);
/* ================================================================ */

/* Definitions of replacement functions
 */
void*
repl___kmalloc(size_t size, gfp_t flags)
{
	void* result = __kmalloc(size, flags);
	printk(	KERN_INFO "[cr_watcher] Called: "
		"__kmalloc(%zu, %x), result: %p\n",
		size, 
		(unsigned int)flags,
		result
	);
	return result;
}

void 
repl_kfree(const void* p)
{
	kfree(p);
	printk(	KERN_INFO "[cr_watcher] Called: "
		"kfree(%p)\n",
		p
	);
	return;
}

void*
repl_kmem_cache_alloc(struct kmem_cache* mc, gfp_t flags)
{
	void* result = kmem_cache_alloc(mc, flags);
	printk(	KERN_INFO "[cr_watcher] Called: "
		"kmem_cache_alloc(%p, %x), result: %p\n",
		mc, 
		(unsigned int)flags,
		result
	);
	return result;
}

void 
repl_kmem_cache_free(struct kmem_cache* mc, void* p)
{
	kmem_cache_free(mc, p);
	printk(	KERN_INFO "[cr_watcher] Called: "
		"kmem_cache_free(%p, %p)\n",
		mc,
		p
	);
	return;
}

long 
repl_copy_from_user(void* to, const void __user * from, unsigned long n)
{
	long result = copy_from_user(to, from, n);
	printk(	KERN_INFO "[cr_watcher] Called: "
		"copy_from_user(%p, %p, %lu), result: %ld\n",
		to,
		from,
		n,
		result
	);
	return result;
}

long 
repl_copy_to_user(void __user * to, const void* from, unsigned long n)
{
	long result = copy_to_user(to, from, n);
	printk(	KERN_INFO "[cr_watcher] Called: "
		"copy_to_user(%p, %p, %lu), result: %ld\n",
		to,
		from,
		n,
		result
	);
	return result;
}

/* ================================================================ */

