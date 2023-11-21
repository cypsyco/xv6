#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

//pa3
uint total_weight;
uint int_limit = 2147483647;

//pa4
struct mmap_area mmap_area_array[64];
int firstRun = 1;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

int weightArray[40] = 
{
/*  0  */ 88761,  71755,  56483,  46273,  36291,
/*  5  */ 29154,  23254,  18705,  14949,  11916,
/*  10 */ 9548,   7620,   6100,   4904,   3906,
/*  15 */ 3121,   2501,   1991,   1586,   1277,
/*  20 */ 1024,   820,    655,    526,    423,
/*  25 */ 335,    272,    215,    172,    137,
/*  30 */ 110,    87,     70,     56,     45,
/*  35 */ 36,     29,     23,     18,     15
};

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority = 20; //jyh: set default priority value as 20

  p->weight = 1024; //pa3: default weight value
  p->runtime = 0; //pa3: acrual runtime
  p->vruntime = 0; //pa3: virtual runtime
  p->timeslice = 0; //pa3: timeslice
  p->runtick = 0; //pa3: runtick
  p->vrunoverflow = 0;//pa3: vrunoverflow

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  int curindex, nindex;
  uint size = 0;

  //pa4: initialize mmap_area_array
  if(firstRun){
    for(int i=0; i<64; i++){
      mmap_area_array[i].proc = 0;
      mmap_area_array[i].addr = 0;
      mmap_area_array[i].offset = 0;
      mmap_area_array[i].f = 0;
      mmap_area_array[i].flags = 0;
      mmap_area_array[i].length = 0;
      mmap_area_array[i].prot = 0;
      mmap_area_array[i].valid = -1;
    }
    firstRun = 0;
  }

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;

  //pa3: process inherits parent process's vruntime, vrunoverflow
  np->vruntime = curproc->vruntime;
  np->vrunoverflow = curproc->vrunoverflow;

  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  //pa4: copy parent’s page table and map physical page
  for(curindex=0; curindex<64; curindex++){
        if(mmap_area_array[curindex].proc == curproc && (mmap_area_array[curindex].valid != -1)){
      for(nindex=0; nindex<64; nindex++){
        if(mmap_area_array[nindex].valid == -1){
          break;
        }
      }
      if ((curindex != 63) && (nindex != 63)){
        mmap_area_array[nindex].f = mmap_area_array[curindex].f;
        mmap_area_array[nindex].addr = mmap_area_array[curindex].addr;
        mmap_area_array[nindex].length = mmap_area_array[curindex].length;
        mmap_area_array[nindex].offset = mmap_area_array[curindex].offset;
        mmap_area_array[nindex].prot = mmap_area_array[curindex].prot;
        mmap_area_array[nindex].flags = mmap_area_array[curindex].flags;
        mmap_area_array[nindex].valid = mmap_area_array[curindex].valid;
        mmap_area_array[nindex].proc = np;

        if(mmap_area_array[nindex].flags == MAP_POPULATE || mmap_area_array[nindex].flags==0){
            mmap_area_array[nindex].f->off = mmap_area_array[nindex].offset;
            for(size=0; size<mmap_area_array[nindex].length; size+=PGSIZE){
              char *memory = kalloc();
              if(memory==0){
                return 0;
              }
              memset(memory, 0, PGSIZE);
              fileread(mmap_area_array[nindex].f, memory, PGSIZE);
              mappages(mmap_area_array[nindex].proc->pgdir, (void*)(mmap_area_array[nindex].addr + size), PGSIZE, V2P(memory), mmap_area_array[nindex].prot|PTE_U);
            }
            mmap_area_array[nindex].valid = 1;
        }

        if(mmap_area_array[nindex].flags == (MAP_POPULATE | MAP_ANONYMOUS) || mmap_area_array[nindex].flags==1){
            mmap_area_array[nindex].f->off = mmap_area_array[nindex].offset;
            for(size=0; size<mmap_area_array[nindex].length; size+=PGSIZE){
              char *memory = kalloc();
              if(memory==0){
                return 0;
              }
              memset(memory, 0, PGSIZE);
              mappages(mmap_area_array[nindex].proc->pgdir, (void*)(mmap_area_array[nindex].addr + size), PGSIZE, V2P(memory), mmap_area_array[nindex].prot|PTE_U);
            }
            mmap_area_array[nindex].valid = 1;
        }
      }
    }
  }
  //pa4

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  curproc->runtime += curproc->runtick;
  if((curproc->vruntime + curproc->runtick * 1024/weightArray[curproc->priority]) > int_limit){
    curproc->vruntime -= int_limit;
    curproc->vruntime += curproc->runtick * 1024/weightArray[curproc->priority];
    curproc->vrunoverflow++;
  }
  else{
    curproc->vruntime += curproc->runtick * 1024/weightArray[curproc->priority];
  }
  curproc->runtick = 0;

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  struct proc *minimum;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    total_weight = 0;

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;
    

    minimum = p;

    //pa3: calculate total weight, vruntime of each process
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == RUNNABLE){
        total_weight += weightArray[p->priority];
        p->weight = weightArray[p->priority];
      }
    }

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == RUNNABLE && p->vruntime < minimum->vruntime && p->vrunoverflow <= minimum->vrunoverflow){
        minimum = p;
      }
    }

    p = minimum;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      p->timeslice = (10000 * weightArray[p->priority]) / total_weight;

      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);

  myproc()->runtime += myproc()->runtick;
  if((myproc()->vruntime + myproc()->runtick * 1024/weightArray[myproc()->priority]) > int_limit){
    myproc()->vruntime -= int_limit;
    myproc()->vruntime += myproc()->runtick * 1024/weightArray[myproc()->priority];
    myproc()->vrunoverflow++;
  }
  else{
    myproc()->vruntime += myproc()->runtick * 1024/weightArray[myproc()->priority];
  }
  myproc()->runtick = 0;
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  p->runtime += p->runtick;
  if((p->vruntime + p->runtick * 1024/weightArray[p->priority]) > int_limit){
    p->vruntime -= int_limit;
    p->vruntime += p->runtick * 1024/weightArray[p->priority];
    p->vrunoverflow++;
  }
  else{
    p->vruntime += p->runtick * 1024/weightArray[p->priority];
  }
  p->runtick = 0;
  //cprintf("runtime = %d vruntime = %d weight = %d\n", p->runtime, p->vruntime, weightArray[p->priority]);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct proc *q; //pa3: q is for searching runnable process
  struct proc *min; //pa3: min is for saving minimum process
  int flag = 0; //pa3: flag is for determining whether there is a runnable process or not

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
    }
  }

  for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
    if(q->state == RUNNABLE){
      total_weight += weightArray[q->priority];
      flag = 1;
    }
  }

  min = q;

  for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
    if(q->state == RUNNABLE && q->vruntime < min->vruntime && q->vrunoverflow <= min->vrunoverflow){
      min = q;
    }
  }

  q = min;

  //pa3: If there is running process, p->vruntime is set through equation
  //pa3: If there is no running process, p->vruntime is set 0
  if(flag){
    p->vruntime = q->vruntime - 1000*1024/p->weight;
    p->vrunoverflow = q->vrunoverflow;
  }
  else{
    p->vruntime = 0;
    p->vrunoverflow = 0;
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getpname(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p -> pid == pid){
      cprintf("%s\n", p->name);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
getnice(int pid)
{
  struct proc *p;
  int nice = 20;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p -> pid == pid){
      nice = p->priority;
      release(&ptable.lock);
      return nice;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
setnice(int pid, int value)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p -> pid == pid){
      if (value >= 0 && value <= 39){
        p->priority = value;
        p->weight = weightArray[value];
        release(&ptable.lock);
        return 0;
      }
      else{
        release(&ptable.lock);
        return -1;
      }
    }
  }
  release(&ptable.lock);
  return -1;
}

//pa3: convert number into string
char* intToString(int num) {
    char str[20];
    int i = 0;
    
    if (num == 0) {
        str[i++] = '0';
    } else {
        while (num > 0) {
            str[i++] = '0' + num % 10;
            num /= 10;
        }
    }
    
    char* result = kalloc();
    if (result == 0) {
        return 0;
    }
    
    int j = 0;
    while (i > 0) {
        result[j++] = str[--i];
    }
    
    result[j] = '\0';
    
    return result;
}

//pa3: add strings as numbers
char* addStrings(char* num1, char* num2) {
    int len1 = strlen(num1);
    int len2 = strlen(num2);
    int maxLen = (len1 > len2) ? len1 : len2;
    int carry = 0;
    
    char result[maxLen + 2];
    
    int i = len1 - 1;
    int j = len2 - 1;
    int k = 0;
    
    while (i >= 0 || j >= 0 || carry) {
        int digit1 = (i >= 0) ? (num1[i] - '0') : 0;
        int digit2 = (j >= 0) ? (num2[j] - '0') : 0;
        
        int sum = digit1 + digit2 + carry;
        carry = sum / 10;
        
        result[k] = (sum % 10) + '0';
        
        i--;
        j--;
        k++;
    }
    
    result[k] = '\0';
    
    for (i = 0, j = k - 1; i < j; i++, j--) {
        char temp = result[i];
        result[i] = result[j];
        result[j] = temp;
    }
    
    char* resultStr = kalloc();
    if (resultStr == 0) {
        return 0;
    }
    
    strncpy(resultStr, result, strlen(result) + 1);
    
    return resultStr;
}

//pa3: convert vruntime into real vruntime considering overflow
char* convert(int num1, int num2) {
    char* strA = intToString(num1);
    char* strB = intToString(2147483647);
    char* sum = strA;
    
    for (int i = 1; i <= num2; i++) {
        char* temp = addStrings(sum, strB);
        if (temp == 0) {
            return 0;
        }
        kfree(sum);
        sum = temp;
    }
    return sum;
}

void
ps(int pid)
{
  struct proc *p;

  static char *states[] = {
  [UNUSED]    "UNUSED  ",
  [EMBRYO]    "EMBRYO  ",
  [SLEEPING]  "SLEEPING",
  [RUNNABLE]  "RUNNABLE",
  [RUNNING]   "RUNNING ",
  [ZOMBIE]    "ZOMBIE  "
  };

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if((pid == 0 || p->pid == pid) && p->state != UNUSED){
      cprintf("%s\t\t%s\t\t%s\t\t%s\t\t%s\t\t%s\t\t\t%s\t\t%s %d\n", "name", "pid", "state", "priority", "runtime/weight", "runtime", "vruntime", "tick", ticks * 1000);
      break;
    }
  }
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(pid == 0 && p->state != UNUSED){
      cprintf("%s\t\t%d\t\t%s\t%d\t\t\t%d\t\t\t%d\t\t\t%s\n", p->name, p->pid, states[p->state], p->priority,p->runtime/p->weight,p->runtime,convert(p->vruntime,p->vrunoverflow));
      continue;
    }
    else if(p->pid == pid && p->state != UNUSED){
      cprintf("%s\t\t%d\t\t%s\t%d\t\t\t%d\t\t\t%d\t\t\t%s\n", p->name, p->pid, states[p->state], p->priority,p->runtime/p->weight,p->runtime,convert(p->vruntime,p->vrunoverflow));
      break;
    }
  }
  release(&ptable.lock);
}

uint
mmap(uint addr, int length, int prot, int flags, int fd, int offset){
  struct proc *p = myproc();
  struct file *f;
  int index = 0;
  struct mmap_area* array;
  uint size = 0;

  addr += 0x40000000;

  if(fd == -1){
    f = 0;
  }
  else{
    f = p->ofile[fd];
  }

  if((flags == MAP_ANONYMOUS) && fd == -1){
    return 0;
  }
  if((prot == PROT_READ) && !(f->readable)){
    return 0;
  }
  if((prot == PROT_WRITE) && !(f->writable)){
    return 0;
  }
  if((prot == (PROT_READ|PROT_WRITE)) && !(f->writable)){
    return 0;
  }
  if((prot == (PROT_READ|PROT_WRITE)) && !(f->readable)){
    return 0;
  }
  
  array = &mmap_area_array[0];
  while(array->valid != -1) {
    index++;
    array = &mmap_area_array[index];
    if(index==63){
      break;
      return -1;
    }
  }

  mmap_area_array[index].f = f;
  mmap_area_array[index].addr = addr;
  mmap_area_array[index].length = length;
  mmap_area_array[index].offset = offset;
  mmap_area_array[index].prot = prot;
  mmap_area_array[index].flags = flags;
  mmap_area_array[index].valid = 0;
  mmap_area_array[index].proc = p;

  if(flags == 0 || flags == MAP_ANONYMOUS){
    return addr;
  }

  if(flags == MAP_POPULATE || flags == (MAP_POPULATE | MAP_ANONYMOUS)){
    f->off = offset;
    for(size=0; size<length; size+=PGSIZE){
      char *memory = kalloc();
      if(memory==0){
        return 0;
      }
      memset(memory, 0, PGSIZE);
      if(flags == MAP_POPULATE){
        fileread(f, memory, PGSIZE);
      }
      mappages(p->pgdir, (void*)(addr + size), PGSIZE, V2P(memory), prot|PTE_U);
    }
    mmap_area_array[index].valid = 1;
  }

  return addr;
}

int
page_fault_handler(uint fault_addr, uint is_write){
  struct proc *p = myproc();
  int index = 0;
  struct file *f;

  //pa4: find according mapping region in mmap_area
  while(index < 64){
    if(mmap_area_array[index].proc == p){
      if((mmap_area_array[index].addr <= fault_addr) && (fault_addr < (mmap_area_array[index].addr + mmap_area_array[index].length))){
        break;
      }
    }
    index++;
  }
  //pa4: faulted address has no corresponding mmap_area
  if(index == 64){
    return -1;
  }

  //pa4: fault was write while mmap_area is write prohibited, then return -1
  if(is_write && ((mmap_area_array[index].prot & PROT_WRITE) != PROT_WRITE)){
    return -1;
  }

  f = mmap_area_array[index].f;
  f->off = mmap_area_array[index].offset;

  char* memory = kalloc();
  if(memory==0){
    return 0;
  }
  memset(memory, 0, PGSIZE);
  if(mmap_area_array[index].flags == MAP_POPULATE){
    fileread(f, memory, PGSIZE);
  }
  
  mappages(p->pgdir, (void*)fault_addr, PGSIZE, V2P(memory), mmap_area_array[index].prot | PTE_W | PTE_U);
  
  mmap_area_array[index].valid = 1;
  
  return 0;
}

int munmap(uint addr){
  struct proc *p = myproc();
  int index = 0;

  //pa4: find according mapping region in mmap_area
  while(index < 64){
    if(mmap_area_array[index].proc == p){
      if((mmap_area_array[index].addr == addr)){
        break;
      }
    }
    index++;
  }
  //pa4: address has no corresponding mmap_area
  if(index == 64){
    return -1;
  }

  //pa4: physical page is allocated
  if(mmap_area_array[index].valid == 1){
    uint length = mmap_area_array[index].length;
    for (uint offset = 0; offset < length; offset += PGSIZE){
      uint va = addr + offset;
      pte_t *pte = walkpgdir(p->pgdir, (void *)va, 0);
       if (pte && (*pte & PTE_P)){
        char *memory = (char *)P2V(PTE_ADDR(*pte));
        memset(memory, 1, PGSIZE); //pa4: fill with 1
        kfree(memory);
        *pte = 0;
       }
    }
    mmap_area_array[index].valid = -1;
    return 1;
  }
  //pa4: physical page is not allocated
  else if(mmap_area_array[index].valid == 0){
    mmap_area_array[index].valid = -1;
    return 1;
  }

  return 1;
}

int freemem(){
  int memvalue = freememReturn();
  return memvalue;
}