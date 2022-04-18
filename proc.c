#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"


struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  init_stride();
  
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
  putMLFQ(p);


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
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

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
        delete_stride(pid);
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
int tt = 0;
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();  
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    p = get_stride();
    
    if(p == 0 || p->state != RUNNABLE){
      goto OUT;
    }
      
    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;

    swtch(&(c->scheduler), p->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    OUT:
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
}


void
myyield(void)
{
  yield();
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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
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




int mlfq_limit_ticks[MLFQ_SIZE][2] = {{1,5},{2,10},{4,1000}};


void initMLFQ(){
  for (int i = 0; i < MLFQ_SIZE; ++i){
    mlfq.front[i] = 0;
    mlfq.rear[i] = 0;    
  }
  for (int i = 0; i < NPROC; ++i){
    mlfq.procs[i].p = 0;
  }
  mlfq.ticks = 0;
}

void putMLFQ(struct proc* p){
  int idx = 0;
  for (int i = 0; i < NPROC; ++i){
    if(mlfq.procs[i].p == 0) idx = i;
  }
  // init
  mlfq.procs[idx].next = 0;
  mlfq.procs[idx].prev = 0; 
  mlfq.procs[idx].ticks_a = 0;
  mlfq.procs[idx].ticks_q = 0;
  mlfq.procs[idx].priority = 0;
  mlfq.procs[idx].p = p;
  //insert queue
  if(mlfq.front[0] == 0) {
    mlfq.rear[0] = &mlfq.procs[idx];
    mlfq.front[0] = &mlfq.procs[idx];
  }
  else{
    mlfq.rear[0]->next = &mlfq.procs[idx];
    mlfq.procs[idx].prev = mlfq.rear[0];
    mlfq.rear[0] = &mlfq.procs[idx];
  }
}


struct mproc* changeMLFQ(struct mproc* mp, int priority){
  struct mproc* next = mp->next;
  //delete old priority queue
  if(mp->prev != 0){
    mp->prev->next = mp->next;
  }
  else{
    mlfq.front[mp->priority] = mp->next;
    if(mlfq.front[mp->priority] != 0)
      mlfq.front[mp->priority]->prev = 0;
  }
  if(mp->next != 0){
    mp->next->prev = mp->prev;
  }
  else{
    mlfq.rear[mp->priority] = mp->prev;
    if(mlfq.rear[mp->priority] != 0)
      mlfq.rear[mp->priority]->next = 0;
  }

  //insert new priority queue
  mp->priority = priority;
  mp->next = 0;
  mp->prev = 0;
  if(mlfq.rear[priority] == 0){
    mlfq.front[priority] = mp;
    mlfq.rear[priority] = mp;
  }
  else{
    mlfq.rear[priority]->next = mp;
    mp->prev = mlfq.rear[priority];
    mlfq.rear[priority] = mp;
  }

  return next;
}

void boostMLFQ(){
  struct mproc* tmp;
  for(int i = 1 ; i < MLFQ_SIZE; ++i){
    tmp = mlfq.front[i];
    while(tmp != 0){
      tmp->ticks_a = 0;
      tmp->ticks_q = 0;
      tmp = changeMLFQ(tmp, 0);
    }
  }
  mlfq.ticks = 0;

  return;
}

struct proc* deleteMLFQ(int pid){
  struct proc* p;
  struct mproc* mp;
  for(int i = 0 ; i < MLFQ_SIZE; ++i){
    mp = mlfq.front[i];
    while(mp != 0){
      if(mp->p->pid == pid){
        p = mp->p;
        mp->p = 0;
        if(mp->prev != 0){
          mp->prev->next = mp->next;
        }
        else{
          mlfq.front[mp->priority] = mp->next;
          if(mlfq.front[mp->priority] != 0)
            mlfq.front[mp->priority]->prev = 0;
        }
        if(mp->next != 0){
          mp->next->prev = mp->prev;
        }
        else{
          mlfq.rear[mp->priority] = mp->prev;
          if(mlfq.rear[mp->priority] != 0)
            mlfq.rear[mp->priority]->next = 0;
        }
        return p;
      }
      else mp = mp->next;
    }
  }
  return 0;
}


struct mproc* getMLFQ(){
  struct mproc* tmp;
  if(mlfq.ticks >= 100) boostMLFQ();
  for(int i = 0 ; i < MLFQ_SIZE; ++i){
    tmp = mlfq.front[i];
    while(tmp != 0){
      if(tmp->p->state == UNUSED) {
        tmp = tmp->next;
        deleteMLFQ(tmp->p->pid);
      }

      if(tmp->ticks_a >= mlfq_limit_ticks[i][1]){ //alloment
        tmp->ticks_a = 0;
        tmp->ticks_q = 0;
        tmp = changeMLFQ(tmp, (i+1)%MLFQ_SIZE);
      }
      else if(tmp->ticks_q >= mlfq_limit_ticks[i][0]){//quantum
        tmp->ticks_q = 0;
        tmp = changeMLFQ(tmp, i);
      }
      else  tmp = tmp->next;
    }
  }
  mlfq.ticks++;
  for(int i = 0 ; i < MLFQ_SIZE; ++i){
    tmp = mlfq.front[i];
    if(tmp != 0){
      tmp->ticks_a++;
      tmp->ticks_q++;
      if(tmp->p->state != RUNNABLE){
        changeMLFQ(tmp, tmp->priority);
      }
      return tmp;
    }
  }
  return 0;
}


void printMLFQ(void){
  struct mproc* tmp;
  for(int i = 0 ; i < MLFQ_SIZE; ++i){
    tmp = mlfq.front[i];
    cprintf("priority %d : ", i);
    while(tmp != 0){
      cprintf("(pid : %d, status : %d) ", tmp->p->pid, tmp->p->state);
      tmp = tmp->next;
    }
    cprintf("\n");
  }
}


struct proc* get_stride(){
  int idx;
  struct mproc* mp;
  while(1){
    idx = find_min_stride();
    if(idx != -1 && stride.procs[idx].p->state == UNUSED) {
      delete_stride(stride.procs[idx].p->pid);
      continue;
    }

    break;
  }
  if(idx == -1 || stride.mlfq.pass_value <= stride.procs[idx].pass_value){
    mp= getMLFQ();
    if(stride.mlfq.pass_value > stride.t_total * 10){
      reduce_all_pass(stride.mlfq.pass_value);
    }
    if(mp->p->state != RUNNABLE){
      // cprintf("goto stride\n");
      goto STRIDE;
      RET:
      return mp->p;
    }
    // cprintf("select mlfq\n");
    stride.mlfq.pass_value += stride.mlfq.stride;
    stride.p_total += stride.mlfq.stride;
    return mp->p;
  }
  else{
    STRIDE:
    if(idx == -1) return 0;
    if(stride.procs[idx].pass_value > stride.t_total * 10){
      reduce_all_pass(stride.procs[idx].pass_value);
    }
    stride.procs[idx].pass_value += stride.procs[idx].stride;
    stride.p_total += stride.procs[idx].stride;
    return stride.procs[idx].p;

  }
  if(mp != 0 && mp->p->state != RUNNABLE) goto RET;
  return 0;

}

void init_stride(){
  initMLFQ();
  stride.t_total = 6400;
  stride.p_total = 0;
  stride.num = 1;
  stride.mlfq.ticket = 6400;
  stride.mlfq.stride = stride.t_total * 10  / stride.mlfq.ticket;
  stride.mlfq.portion = 100;
  stride.mlfq.pass_value = 0;
  for(int i = 0 ; i < NPROC; ++i){
    stride.procs[i].p = 0;
  }
}

int allocate_stride(int portion){
  struct proc* p = myproc();
  if(portion < 0 || portion > 80|| stride.mlfq.portion - portion < 20 || p == 0) return -1;
  struct mproc* tmp;
  for(int i = 0 ; i < MLFQ_SIZE; ++i){
    tmp = mlfq.front[i];
    while(tmp != 0){
      if(tmp->p == p){
        for(int j = 0 ; j < NPROC; ++j){
          if(stride.procs[j].p == 0){
            deleteMLFQ(p->pid);
            stride.procs[j].p = p;
            stride.procs[j].portion = portion;
            stride.procs[j].pass_value = stride.p_total / stride.num;
            stride.procs[j].ticket = stride.t_total * portion / 100;
            stride.procs[j].stride = stride.t_total * 10 / stride.procs[j].ticket;
            stride.mlfq.ticket -= stride.procs[j].ticket;
            stride.mlfq.portion -=  portion;
            stride.mlfq.stride =  stride.t_total * 10 / stride.mlfq.ticket;
            stride.num++;
            stride.p_total += stride.procs[j].pass_value;
            return 0;
          }
        }

      }
    }
  }
  return -2;
}

void delete_stride(int pid){
  for(int i = 0 ; i < NPROC; ++i){
    if(stride.procs[i].p->pid == pid){
      stride.procs[i].p = 0;
      stride.mlfq.ticket += stride.procs[i].ticket;
      stride.mlfq.portion += stride.procs[i].portion;
      stride.mlfq.stride =  stride.t_total * 10 / stride.mlfq.ticket;
      stride.p_total -= stride.procs[i].pass_value;
      stride.num -= 1;
      return;
    }
  }
  deleteMLFQ(pid);
}

int find_min_stride(){
  int min_pass = __INT_MAX__;
  int idx = -1;
  for(int i = 0 ; i < NPROC; ++i){
    if(stride.procs[i].p != 0 && stride.procs[i].pass_value < min_pass){
      min_pass = stride.procs[i].pass_value;
      idx = i;
    }
  }
  return idx;
}
void reduce_all_pass(int value){
  stride.p_total -= value*stride.num;
  stride.mlfq.pass_value -= value;
  for(int i = 0 ; i < NPROC; ++i){
    if(stride.procs[i].p != 0){
      stride.procs[i].pass_value -= value;
    }
  }
  return;
}

void print_scheduler(){
  cprintf("Sride\n");
  for(int i = 0 ; i < NPROC; ++i){
    if(stride.procs[i].p != 0){
      cprintf("(pid %d state %d pass %d portion %d stride %d) ",stride.procs[i].p->pid, stride.procs[i].p->state, stride.procs[i].pass_value, stride.procs[i].portion,stride.procs[i].stride);
    }
  }
  cprintf("\nMLFQ(pass %d portion %d stride %d)\n", stride.mlfq.pass_value, stride.mlfq.portion, stride.mlfq.stride);
  printMLFQ();  
  cprintf("\n");
}

