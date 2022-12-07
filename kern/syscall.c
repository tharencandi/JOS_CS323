/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
  // Check that the user has permission to read memory [s, s+len).
  // Destroy the environment if not.
      user_mem_assert(curenv, s, len, PTE_U);
  // LAB 3: Your code here.

  // Print the string supplied by the user.
  cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
  return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
  return curenv->env_id;
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static int
sys_env_destroy(envid_t envid)
{
  int r;
  struct Env *e;

  if ((r = envid2env(envid, &e, 1)) < 0)
    return r;
  if (e == curenv)
    cprintf("[%08x] exiting gracefully\n", curenv->env_id);
  else
    cprintf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);
  env_destroy(e);
  return 0;
}

/*
  Allocate a new environment.
  Returns envid of new environment, or < 0 on error.  Errors are:
    -E_NO_FREE_ENV if no free environment is available.
    -E_NO_MEM on memory exhaustion.
*/
static int sys_exofork(void) {

  // Create the new environment with env_alloc(), from kern/env.c.
  // It should be left as env_alloc created it, except that
  // status is set to ENV_NOT_RUNNABLE, and the register set is copied
  // from the current environment -- but tweaked so sys_exofork
  // will appear to return 0.

  struct Env *child_env;
  int ret  = env_alloc(&child_env, curenv->env_id);
  if (ret < 0)
    return ret;

  child_env->env_status = ENV_NOT_RUNNABLE;
  //copy register state
  memcpy(&child_env->env_tf, &curenv->env_tf, sizeof(child_env->env_tf));

  // Clear %eax so that fork returns 0 in the child.
  child_env->env_tf.tf_regs.reg_eax = 0;

  return child_env->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int sys_env_set_status(envid_t eid, int status ) {
  struct Env *e;
  if (envid2env(eid, &e, 1) < 0)
    return -E_BAD_ENV;

  if (!(status == ENV_RUNNABLE || status == ENV_NOT_RUNNABLE))
    return -E_INVAL;
  
  e->env_status = status;

  return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables
static int sys_page_alloc(envid_t eid, void *va, int perm) {

  if (!(perm & PTE_U) || !(perm & PTE_P) || (perm & ~(PTE_U | PTE_P | PTE_AVAIL | PTE_W)))
    return -E_INVAL;
  
  if ((uintptr_t)va >= UTOP || (uintptr_t)va % PGSIZE != 0)
    return -E_INVAL;
  
  struct Env *e;
  if (envid2env(eid, &e, 1) < 0)
    return -E_BAD_ENV;
  
  struct PageInfo * new_page = page_alloc(1);
  if (new_page == NULL)
    return -E_NO_MEM;
  
  return page_insert(e->env_pgdir, new_page, va, perm);
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
             envid_t dstenvid, void *dstva, int perm)
{
  // Hint: This function is a wrapper around page_lookup() and
  //   page_insert() from kern/pmap.c.
  //   Again, most of the new code you write should be to check the
  //   parameters for correctness.
  //   Use the third argument to page_lookup() to
  //   check the current permissions on the page.
  if (!(perm & PTE_U) || !(perm & PTE_P) || (perm & ~(PTE_U | PTE_P | PTE_AVAIL | PTE_W)))
    return -E_INVAL;
  
  struct Env *src;
  struct Env *dst;
  if (envid2env(srcenvid, &src, 1) < 0)
    return -E_BAD_ENV;
  if (envid2env(dstenvid, &dst, 1) < 0)
    return -E_BAD_ENV;
  
  if ((uintptr_t)srcva >= UTOP || (uintptr_t)srcva % PGSIZE != 0 
    || (uintptr_t)dstva >= UTOP || (uintptr_t)dstva % PGSIZE != 0 ) 
    return -E_INVAL;
  pte_t *srcpte;
  struct PageInfo * srcpage = page_lookup(src->env_pgdir, srcva, &srcpte);
  if (srcpage == NULL)
    return -E_INVAL;

  if ((perm & PTE_W) && !(*srcpte & PTE_W))
    return -E_INVAL;
  
  return page_insert(dst->env_pgdir, srcpage, dstva, perm);
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
  // Hint: This function is a wrapper around page_remove().

  if ( (uintptr_t)va >= UTOP || (uintptr_t) va % PGSIZE != 0)
    return -E_INVAL;

  struct Env *e;
  if (envid2env(envid, &e, 1) < 0)
    return -E_BAD_ENV; 

  page_remove(e->env_pgdir, va);

  return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
  struct Env *e;
  if (envid2env(envid, &e, 1) < 0)
    return -E_BAD_ENV;

  e->env_pgfault_upcall = func;

  return 0;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
  // Call the function corresponding to the 'syscallno' parameter.
  // Return any appropriate return value.
  // LAB 3: Your code here.
  
  switch (syscallno) {
    case SYS_cputs:
      sys_cputs((const char *)a1, (size_t) a2);
      break;
    case SYS_cgetc:
      return sys_cgetc();
      break;
    case SYS_getenvid:
      return sys_getenvid();
      break;
    case SYS_env_destroy:
      return sys_env_destroy((envid_t)a1);
      break;
    case SYS_yield:
      sched_yield();
    case SYS_exofork:
      return sys_exofork();
    case SYS_env_set_status:
      return sys_env_set_status(a1, a2);
    case SYS_page_alloc:
      return sys_page_alloc(a1,(void *) a2, a3);
    case SYS_page_map:
      return sys_page_map(a1, (void*)a2, a3, (void*)a4, a5);
    case SYS_page_unmap:
      return sys_page_unmap(a1, (void*) a2);
    case SYS_env_set_pgfault_upcall:
      return sys_env_set_pgfault_upcall(a1, (void*) a2);
    default:
      return -E_INVAL; 

  }

  return 0;

}

