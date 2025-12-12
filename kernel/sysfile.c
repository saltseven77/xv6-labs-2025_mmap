//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

#ifndef __ARGFD_FWD_DECL
#define __ARGFD_FWD_DECL
static int argfd(int n, int *pfd, struct file **pf);
#endif

#ifdef LAB_MMAP
static struct vma *find_free_vma(struct proc *p)
{
  for (int i = 0; i < MAXVMA; i++) {
    if (!p->vmas[i].used)
      return &p->vmas[i];
  }
  return 0;
}
// return 1 if ranges [a0,a0+l0) and [a1,a1+l1) overlap
static int range_overlap(uint64 a0, uint64 l0, uint64 a1, uint64 l1)
{
  uint64 e0 = a0 + l0;
  uint64 e1 = a1 + l1;
  return !(e0 <= a1 || e1 <= a0);
}

// find a free VA range of length "length" starting at or after base,
// avoiding overlap with existing VMAs. returns 0 on failure.
static uint64 find_free_range(struct proc *p, uint64 base, uint64 length)
{
  uint64 addr = base;
  if (length == 0)
    return 0;
  // highest usable user VA (exclude trapframe & trampoline pages)
  uint64 upper = MAXVA - 2*PGSIZE;
  // simple first-fit scan by bumping past overlapping VMAs
  for (;;) {
    int overlapped = 0;
    for (int i = 0; i < MAXVMA; i++) {
      if (!p->vmas[i].used) continue;
      uint64 s = p->vmas[i].start;
      uint64 e = s + p->vmas[i].length;
      if (range_overlap(addr, length, s, e - s)) {
        // bump just past this VMA and restart scan
        addr = e;
        overlapped = 1;
        break;
      }
    }
    if (!overlapped)
      break;
    // avoid wrapping and extremely large addresses
    if (addr + length > upper)
      return 0;
  }
  if (addr + length > upper)
    return 0;
  return addr;
}
static struct vma *find_vma_covering(struct proc *p, uint64 addr, uint64 len)
{
  for (int i = 0; i < MAXVMA; i++) {
    if (!p->vmas[i].used) continue;
    uint64 start = p->vmas[i].start;
    uint64 end = start + p->vmas[i].length;
    if (addr >= start && addr + len <= end)
      return &p->vmas[i];
  }
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 uaddr;
  size_t length;
  int prot, flags, fd;
  off_t off;
  struct file *f;
  struct proc *p = myproc();

  argaddr(0, &uaddr);
  argaddr(1, (uint64 *)&length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, (uint64 *)&off);

  if (length == 0)
    return (uint64)-1;
  // page-align length and require page-aligned offset
  if (off % PGSIZE)
    return (uint64)-1;
  length = PGROUNDUP(length);

  if (argfd(4, 0, &f) < 0)
    return (uint64)-1;

  // if shared+write, require fd writable
  if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !f->writable)
    return (uint64)-1;

  // pick an address if not provided
  uint64 addr = uaddr;
  if (addr == 0) {
    //addr = PGROUNDUP(p->sz);
    uint64 base = PGROUNDUP(p->sz);
    addr = find_free_range(p, base, length);
    if (addr == 0)
      return (uint64)-1;
    // do not change p->sz; keep separate region
  } else {
    if (addr % PGSIZE)
      return (uint64)-1;
    // bounds check against upper limit
    uint64 upper = MAXVA - 2*PGSIZE;
    if (addr + length > upper)
      return (uint64)-1;
    // explicit address: reject if overlaps an existing VMA
    for (int i = 0; i < MAXVMA; i++) {
      if (!p->vmas[i].used) continue;
      if (range_overlap(addr, length, p->vmas[i].start, p->vmas[i].length))
        return (uint64)-1;
    }  
}

  struct vma *v = find_free_vma(p);
  if (v == 0)
    return (uint64)-1;

  // install VMA
  v->start = addr;
  v->length = length;
  v->prot = prot;
  v->flags = flags;
  v->f = filedup(f);
  v->off = off;
  v->used = 1;

  return addr;
}

static int vma_writeback_and_unmap(struct proc *p, struct vma *v, uint64 addr, uint64 len)
{
  // writeback for MAP_SHARED & PROT_WRITE
  int do_wb = (v->flags & MAP_SHARED) && (v->prot & PROT_WRITE);
  for (uint64 a = addr; a < addr + len; a += PGSIZE) {
    pte_t *pte = walk(p->pagetable, a, 0);
    if (pte && (*pte & PTE_V)) {
      uint64 pa = PTE2PA(*pte);
      if (do_wb && v->f && v->f->type == FD_INODE) {
        uint64 off_in_map = (a - v->start);
        uint file_off = v->off + off_in_map;
        begin_op();
        ilock(v->f->ip);
        //(void) writei(v->f->ip, 0, (uint64)pa, file_off, PGSIZE);
        // write only the part that fits in the original file size; do not extend file
        uint fsize = v->f->ip->size;
        uint n = 0;
        if (file_off < fsize) {
          uint rem = fsize - file_off;
          n = rem > PGSIZE ? PGSIZE : rem;
        }
        if (n > 0) {
          (void) writei(v->f->ip, 0, (uint64)pa, file_off, n);
        }
        iunlock(v->f->ip);
        end_op();
      }
      kfree((void *)pa);
      *pte = 0;
    }
  }
  return 0;
}

uint64
sys_munmap(void)
{
  uint64 uaddr;
  size_t length;
  struct proc *p = myproc();

  argaddr(0, &uaddr);
  argaddr(1, (uint64 *)&length);
  if (length == 0)
    return -1;
  if (uaddr % PGSIZE)
    return -1;
  length = PGROUNDUP(length);

  struct vma *v = find_vma_covering(p, uaddr, length);
  if (v == 0)
    return -1;

  // unmap and optional writeback
  vma_writeback_and_unmap(p, v, uaddr, length);

  // adjust vma (support unmap from head or tail; not middle)
  if (uaddr == v->start && length == v->length) {
    // remove whole VMA
    if (v->f) {
      fileclose(v->f);
      v->f = 0;
    }
    v->used = 0;
  } else if (uaddr == v->start) {
    v->start += length;
    v->off += length;
    v->length -= length;
  } else if (uaddr + length == v->start + v->length) {
    v->length -= length;
  } else {
    // unsupported split
    return -1;
  }

  return 0;
}
#endif

#ifdef LAB_MMAP
void
vma_cleanup(struct proc *p)
{
  for (int i = 0; i < MAXVMA; i++) {
    if (!p->vmas[i].used)
      continue;
    struct vma *v = &p->vmas[i];
    // unmap whole region with writeback if needed
    vma_writeback_and_unmap(p, v, v->start, v->length);
    if (v->f) {
      fileclose(v->f);
      v->f = 0;
    }
    v->used = 0;
  }
}
#endif
// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

