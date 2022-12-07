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
  if (! (uvpt[PGNUM(addr)] & PTE_W && uvpt[PGNUM(addr)] & PTE_COW))
    panic("page not writeable and copy-on-writable");
  

  // Allocate a new page, map it at a temporary location (PFTEMP),
  // copy the data from the old page to the new page, then move the new
  // page to the old page's address.
  // Hint:
  //   You should make three system calls.
  uint32_t envid = sys_getenvid();
  if (envid < 0)
    panic("bruh");
  r = sys_page_alloc(envid, (void*) PFTEMP,  PTE_P | PTE_U | PTE_W);
  if (r < 0)
    panic("bruh");
  memcpy((void*) PFTEMP, addr, PGSIZE);
  r = sys_page_map(envid,(void*) PFTEMP, envid, addr, PTE_P | PTE_U | PTE_W );  
  if (r < 0)
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
  uint32_t src_envid = sys_getenvid();
  if (src_envid < 0 )
    return src_envid;

  uint32_t pte = uvpt[pn];
  uint32_t perms = pte & 0xFFF;
  if (perms & PTE_W)
    perms |= PTE_COW;

  r = sys_page_map(src_envid, (void*)(pn*PGSIZE), envid, (void*)(pn*PGSIZE), perms);
  
  if (r < 0)
    return r;


  uvpt[pn] |= perms;



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
  set_pgfault_handler(pgfault);
  int child_env_id = sys_exofork();
  if (child_env_id < 0) {     
    return child_env_id;
  }
  uintptr_t va;
  //For each writable or copy-on-write page in its 
  //address space below UTOP, the parent calls duppage
  //remap the page copy-on-write in its own address space
  for(va = 0; va < UTOP - PGSIZE ; va += PGSIZE) {
    //The PTE for page number N is stored in uvpt[N] 
    uint32_t ptd = uvpd[PDX(va)];
    uint32_t pte = uvpt[PGNUM(va)];

    if (!(pte & PTE_P && ptd & PTE_P) ) // skip not present
      continue;
    
    duppage(child_env_id, PGNUM(va)); 

  }
      
  //allocate exception stack in child.
  sys_page_alloc(child_env_id, (void*) UXSTACKTOP - PGSIZE, PTE_P | PTE_W | PTE_U);
  
  sys_env_set_pgfault_upcall(child_env_id, pgfault);
  sys_env_set_status(child_env_id, ENV_RUNNABLE);
  return child_env_id;
}

// Challenge!
int
sfork(void)
{
  panic("sfork not implemented");
  return -E_INVAL;
}
