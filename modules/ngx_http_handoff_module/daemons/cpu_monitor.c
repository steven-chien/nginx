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

// Function to parse CPU usage from /proc/stat
void get_cpu_times(unsigned long long *idle, unsigned long long *total, unsigned long long *iowait) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("Failed to open /proc/stat");
        exit(EXIT_FAILURE);
    }

    unsigned long long user, nice, system, irq, softirq, steal;
    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", 
               &user, &nice, &system, idle, iowait, &irq, &softirq, &steal) != 8) {
        perror("Failed to parse /proc/stat");
        fclose(fp);
        return;
    }
    fclose(fp);

    *total = user + nice + system + *idle + *iowait + irq + softirq + steal;
    *idle += *iowait; // Idle includes iowait
}

void monitor(int my_id, redisContext *c, uint8_t *shmaddr)
{
    redisReply *reply;
    unsigned long long idle1, total1, idle2, total2;
    unsigned long long iowait1, iowait2;
    
    // Get initial CPU times
    get_cpu_times(&idle1, &total1, &iowait1);
    usleep(500000); // Sleep for 500 milliseconds
    // Get CPU times after delay
    get_cpu_times(&idle2, &total2, &iowait2);
    
    // Compute CPU usage percentage
    if (iowait2 - iowait1 == 0) {
        unsigned long long idle_diff = idle2 - idle1;
        unsigned long long total_diff = total2 - total1;
        int cpu_usage = 100.0 * (1.0 - ((double)idle_diff / total_diff));
 
        //printf("CPU Usage: %.2f%%\n", cpu_usage);

        //shmaddr[my_id] = cpu_usage;
        reply = redisCommand(c, "SET %d %d", my_id, cpu_usage);
        assert (reply != NULL);
        freeReplyObject(reply);
    }

    printf("CPU Usage:");
    reply = redisCommand(c, "mget 0 1 2 3");
    for (int i = 0; i < reply->elements; i++) {
        shmaddr[i] = atoi(reply->element[i]->str);
        printf(" %d", shmaddr[i]);
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    int ret;
    redisContext *c;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s [my id]\n", argv[0]);
        exit(1);
    }

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
        monitor(atoi(argv[1]), c, shmaddr);
    }

    //redisClose(c);
    shmdt(shmaddr);
    shmctl(shmid, IPC_RMID, NULL);

    return 0;
}
