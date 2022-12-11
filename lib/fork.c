// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW         0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
  void *addr = (void*)utf->utf_fault_va;
  uint32_t err = utf->utf_err;
  int r;
  addr = ROUNDDOWN(addr, PGSIZE);

  // Check that the faulting access was (1) a write, and (2) to a
  // copy-on-write page.  If not, panic.
  // Hint:
  //   Use the read-only page table mappings at uvpt
  //   (see <inc/memlayout.h>).
  if (!((err & FEC_WR) && (uvpt[PGNUM(addr)] & PTE_COW)))
    panic("not copy-on-write");

  // Allocate a new page, map it at a temporary location (PFTEMP),
  // copy the data from the old page to the new page, then move the new
  // page to the old page's address.
  // Hint:
  //   You should make three system calls.
  uint32_t envid;
  if ((envid = sys_getenvid()) < 0)
    panic("bruh");
  
  if ((r = sys_page_alloc(envid, (void*) PFTEMP,  PTE_P | PTE_U | PTE_W)) < 0)
    panic("bruh");
  
  memcpy((void*) PFTEMP, addr, PGSIZE);
  
  if ((r = sys_page_map(envid,(void*) PFTEMP, envid, addr, PTE_P | PTE_U | PTE_W )) < 0)
    panic("bruh");
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
  int r;
  uint32_t c_id;

  // get current env id
  if ((c_id = sys_getenvid()) < 0)
    return c_id;

	void *va = (void*)(pn*PGSIZE);


  // SHARE
  if (uvpt[pn] & PTE_SHARE) {
    if ( (r = sys_page_map(c_id, va, envid, va, uvpt[pn] & PTE_SYSCALL)) < 0)
      return r;
  }

  // COW AND OTHER

  // map envid and cid envs w/ cow flag if cow or write
  uint16_t cow = 0;
	if ( ((uvpt[pn] & PTE_W) || (uvpt[pn] & PTE_COW)) ) {
    cow = PTE_COW;
	} 

  if ( (r = sys_page_map(c_id, va, envid, va, cow | PTE_U | PTE_P)) < 0)
    return r;
  if ((r = sys_page_map(c_id, va, c_id, va, cow | PTE_U | PTE_P)) < 0)
    return r;

	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
  // LAB 4: Your code here.
  set_pgfault_handler(pgfault);
  int child_env_id;

  if ((child_env_id = sys_exofork()) < 0 )
    return child_env_id;

  if (child_env_id == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
  
  uint32_t va;
  //For each writable or copy-on-write page in its 
  //address space below UTOP, the parent calls duppage
  //remap the page copy-on-write in its own address space
  for(va = 0; va < UTOP-PGSIZE ; va += PGSIZE) {
    //The PTE for page number N is stored in uvpt[N] 
    //ptd = uvpd[PDX(va)];
    //pte = uvpt[PGNUM(va)];
    if (!(!(uvpd[PDX(va)] & PTE_P) || !(uvpt[PGNUM(va)] & PTE_P) || !(uvpt[PGNUM(va)] & PTE_U))) {
       duppage(child_env_id, PGNUM(va)); 
         
		}
  }

  //allocate exception stack in child.
  if (sys_page_alloc(child_env_id, (void*) (UXSTACKTOP - PGSIZE), PTE_P | PTE_W | PTE_U) < 0)
    panic("cant allocate uxstack in fork\n");
  
  extern void _pgfault_upcall();

  if (sys_env_set_pgfault_upcall(child_env_id, _pgfault_upcall) < 0)
    panic("cant set upcall in fork\n");
  
  if (sys_env_set_status(child_env_id, ENV_RUNNABLE) < 0)
    panic("cant set forked env to runnable\n");

  return child_env_id;
}

// Challenge!
int
sfork(void)
{
  panic("sfork not implemented");
  return -E_INVAL;
}
