#include "param.h"
// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

// -------------------------------------------------------------------------

// custom proc structure for using mlfq
struct mproc {
  struct proc* p;
  int priority;
  struct mproc* prev;
  struct mproc* next;
  int ticks_q;                   // ticks for using mlfq time quantum
  int ticks_a;                   // ticks for using mlfq time allotment
};

#define MLFQ_SIZE 3  // number of mlfq level

typedef struct MLFQ{
    // int p_priority[NPROC];          // p_priority[pid] = priority
    struct mproc procs[NPROC];    // pid; queue[0]:High, queue[1]:Mid, queue[2]:Low 
    struct mproc* front[MLFQ_SIZE];           // q front 
    struct mproc* rear[MLFQ_SIZE];           // number of processs in queue
}Mlfq;


void set_cpu_share(int share, int mode); // share is percentages


void initMLFQ(int pid){
  for (int i = 0; i < MLFQ_SIZE; ++i){
    mlfq.front[i] = 0;
    // mlfq.nproc[i] = 0;
  }
}

void putMLFQ(struct proc* p);
void changeMLFQ(struct mproc* mp, int priority);
int getMLFQ();

typedef struct Pass{
    int pid;
    int stride;
    int ticket;
    int pass_value;
}Pass;

typedef struct _Stride{
    int t_total; // number of tickets total
    int rate; //stride rate (max 80)
    Pass* procs; // processes
    Pass mlfq; // mlfq
} Stride;

int mlfq_limit_ticks[MLFQ_SIZE][2] = {{1,5},{2,10},{4,1000}};
Mlfq mlfq;
Stride stride;