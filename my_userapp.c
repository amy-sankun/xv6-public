#include "types.h"
#include "stat.h"
#include "user.h"

void* func(void* parm)     // 반환값과 매개변수가 없음
{
    return 0;
}

int
main(int argc, char* argv[])
{
    // void* (*fp)(void*) = func; 
    // int ret;
    // void 
    // thread_create()
    // int returned_pid;
    // printf(1, "Hello, my pid is %d\n", getpid());

    // if((returned_pid=fork()) < 0){
    //     printf(1, "fork error"); exit();
    // }
    // else if(returned_pid == 0){
    //     printf(1, "child: pid = %d, ppid = %d\n", getpid(), getppid());
    // }
    // else{
    //     wait(); // 자식 프로세스가 모두 끝날 때까지 기다린다.
    //     printf(1, "parent: I created child with returned_pid=%d\n", returned_pid);
    // }

    // printf(1, "Bye, my pid is %d\n", getpid());

    // exit();
	// printf(1, "My pid is %d\n", getpid());
	// printf(1, "My ppid is %d\n", getppid());
    // exit();
}