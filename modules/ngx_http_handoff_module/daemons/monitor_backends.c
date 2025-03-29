#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <hiredis/hiredis.h>

#define MAX_PEERS 4

#define SHM_SIZE (sizeof(uint8_t))  // Shared memory size

void monitor(redisContext *c, uint8_t *shmaddr)
{
    redisReply *reply;
    reply = redisCommand(c, "mget 0 1 2 3");
    
    printf("CPU Usage:");
    for (int i = 0; i < reply->elements; i++) {
        shmaddr[i] = atoi(reply->element[i]->str);
        printf(" %d", shmaddr[i]);
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    int ret;
    redisContext *c;

    int shmid;
    uint8_t *shmaddr;
    key_t key = 1234;  // Unique key for the shared memory

    // Step 1: Create shared memory segment
    if ((shmid = shmget(key, sizeof(uint8_t)*MAX_PEERS, IPC_CREAT | 0666)) == -1) {
        perror("shmget failed");
        exit(1);
    }

    // Step 2: Attach shared memory to parent process
    if ((shmaddr = shmat(shmid, NULL, 0)) == (void *)-1) {
        perror("shmat failed");
        exit(1);
    }

   c = redisConnect("n30", 6379);
   if (c->err) {
        fprintf(stderr, "error: %s\n", c->errstr);
        exit(1);
   }

    while (true) {
        monitor(c, shmaddr);
        sleep(1);
    }

    //redisClose(c);
    shmdt(shmaddr);
    shmctl(shmid, IPC_RMID, NULL);

    return 0;
}
