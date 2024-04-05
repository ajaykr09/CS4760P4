#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <string>
#include <cstring>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <errno.h>
#include <chrono>
#include <random>
using namespace std;

#define TERMINATION_CHANCE 10
#define IO_BLOCK_CHANCE 15
#define ACTUAL_IO_BLOCK_CHANCE (TERMINATION_CHANCE + IO_BLOCK_CHANCE)
#define DISPATCH_DURATION 1000
#define CHILD_LAUNCH_DURATION 1000
#define UNBLOCK_DURATION 1000
#define MSGQ_FILE_PATH "msgq.txt"
#define MSGQ_PROJ_ID 333
#define PERMS 0644
#define MSG_TYPE_BLOCKED 2   // BLOCKED
#define MSG_TYPE_RUNNING 1   // STILL RUNNING
#define MSG_TYPE_SUCCESS 0   // TERMINATING

typedef struct SystemClock {
    int seconds;
    int nanoseconds;
} SystemClock;

typedef struct MessageBuffer {
    long mtype;
    char message[100];
    int timeSlice;
    int blockedUntilSeconds;
    int blockedUntilNanoseconds;
    int msgCode;
} MessageBuffer;

int generateRandomNumber(int min, int max, int pid) {
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count() * pid;
    std::default_random_engine generator(seed);
    std::uniform_int_distribution<int> distribution(min, max);
    int randomNumber = distribution(generator);
    return randomNumber;
}

void calculateTimeUntilUnblocked(int secs, int nanos, int nanosBlocked, int *unblockSecs, int *unblockNanos) {
    nanos += nanosBlocked;
    if (nanos >= 1000000000) {
        nanos -= 1000000000;
        secs++;
    }
    *unblockSecs = secs;
    *unblockNanos = nanos;
}

bool willProcessTerminateDuringQuantum(int secs, int nanos, int endSecs, int endNanos, int quantum, int* timeSliceUsed) {
    int secsPlusQuantum = secs;
    int nanosPlusQuantum = nanos + quantum;

    if (nanosPlusQuantum >= 1000000000) {
        nanosPlusQuantum -= 1000000000;
        secsPlusQuantum++;
    }

    int elapsedSecs = endSecs - secsPlusQuantum;
    int elapsedNanos = endNanos - nanosPlusQuantum;

    if (elapsedNanos < 0) {
        elapsedSecs--;
        elapsedNanos += 1000000000;
    }

    if (elapsedSecs < 0) {
        *timeSliceUsed = abs(elapsedSecs * 1000000000 + elapsedNanos);
        return true;
    }

    return false;
}

int main(int argc, char** argv) {
    SystemClock* sharedClock;
    key_t clockKey = ftok("/tmp", 35);
    int sharedMemoryId = shmget(clockKey, sizeof(SystemClock), 0666);
    sharedClock = (SystemClock*)shmat(sharedMemoryId, NULL, 0);

    int startSeconds = sharedClock->seconds;
    int startNanoseconds = sharedClock->nanoseconds;

    int secondsToLive = atoi(argv[1]);
    int nanosecondsToLive = atoi(argv[2]);

    int endSeconds = startSeconds + secondsToLive;
    int endNanoseconds = startNanoseconds + nanosecondsToLive;
    if (endNanoseconds >= 1000000000) {
        endNanoseconds -= 1000000000;
        endSeconds++;
    }

    MessageBuffer messageBuffer, receiveBuffer;
    messageBuffer.mtype = getppid();
    int messageQueueId = 0;
    key_t msgqKey;
    if ((msgqKey = ftok(MSGQ_FILE_PATH, MSGQ_PROJ_ID)) == -1) {
        perror("ftok");
        exit(1);
    }
    if ((messageQueueId = msgget(msgqKey, PERMS)) == -1) {
        perror("msgget in child");
        exit(1);
    }
    printf("%d: Child has access to the message queue\n", getpid());
    printf("Worker PID: %d  PPID: %d  SysClockS: %d  SysClockNano: %d  TermTimeS: %d  TermTimeNano: %d\n--Just Starting\n", getpid(), getppid(), sharedClock->seconds, sharedClock->nanoseconds, endSeconds, endNanoseconds);

    int iterations = 0;
    bool done = false;

    while (!done) {
        iterations++;
        if (msgrcv(messageQueueId, &receiveBuffer, sizeof(MessageBuffer), getpid(), 0) == -1) {
            perror("Failed to receive message from parent\n");
            exit(1);
        }
        printf("%d: Child dispatched by parent and given quantum %d\n", getpid(), receiveBuffer.timeSlice);

        int randomNumber = generateRandomNumber(1, 100, getpid());

        if (randomNumber < TERMINATION_CHANCE) {
            printf("Worker PID: %d  PPID: %d  SysClockS: %d  SysClockNano: %d  TermTimeS: %d  TermTimeNano: %d\n--Terminating after sending message back to OSS after %d iterations.\n", getpid(), getppid(), sharedClock->seconds, sharedClock->nanoseconds, endSeconds, endNanoseconds, iterations);
            done = true;
            messageBuffer.msgCode = MSG_TYPE_SUCCESS;
            messageBuffer.timeSlice = generateRandomNumber(1, 100000000, getpid()) % receiveBuffer.timeSlice;

            strcpy(messageBuffer.message, "Completed Successfully (RANDOM TERMINATION), now terminating...\n");
        } else if (randomNumber < ACTUAL_IO_BLOCK_CHANCE) {
            int nanosBlocked = 1000000000;
            int tempUnblockSeconds, tempUnblockNanoseconds;
            calculateTimeUntilUnblocked(sharedClock->seconds, sharedClock->nanoseconds, nanosBlocked, &tempUnblockSeconds, &tempUnblockNanoseconds);
            messageBuffer.blockedUntilSeconds = tempUnblockSeconds;
            messageBuffer.blockedUntilNanoseconds = tempUnblockNanoseconds;
            messageBuffer.msgCode = MSG_TYPE_BLOCKED;
            messageBuffer.timeSlice = generateRandomNumber(1, 100000000, getpid()) % receiveBuffer.timeSlice;
            strcpy(messageBuffer.message, "IO BLOCKED!!!...\n");
        } else {
            int timeSliceUsed = receiveBuffer.timeSlice;
            if (sharedClock->seconds > endSeconds || (sharedClock->seconds == endSeconds && sharedClock->nanoseconds > endNanoseconds)) {
                printf("Worker PID: %d  PPID: %d  SysClockS: %d  SysClockNano: %d  TermTimeS: %d  TermTimeNano: %d\n--Terminating after sending message back to OSS after %d iterations.\n", getpid(), getppid(), sharedClock->seconds, sharedClock->nanoseconds, endSeconds, endNanoseconds, iterations);
                done = true;
                messageBuffer.msgCode = MSG_TYPE_SUCCESS;
                messageBuffer.timeSlice = 0;
                strcpy(messageBuffer.message, "Completed Successfully (END TIME ELAPSED BEFORE RUNTIME), now terminating...\n");
            } else if (willProcessTerminateDuringQuantum(sharedClock->seconds, sharedClock->nanoseconds, endSeconds, endNanoseconds, receiveBuffer.timeSlice, &timeSliceUsed)) {
                printf("Worker PID: %d  PPID: %d  SysClockS: %d  SysClockNano: %d  TermTimeS: %d  TermTimeNano: %d\n--Terminating after sending message back to OSS after %d iterations.\n", getpid(), getppid(), sharedClock->seconds, sharedClock->nanoseconds, endSeconds, endNanoseconds, iterations);
                done = true;
                messageBuffer.msgCode = MSG_TYPE_SUCCESS;
                messageBuffer.timeSlice = timeSliceUsed;
                strcpy(messageBuffer.message, "Completed Successfully (END TIME ELAPSED DURING RUNTIME), now terminating...\n");
            } else {
                printf("Worker PID: %d  PPID: %d  SysClockS: %d  SysClockNano: %d  TermTimeS: %d  TermTimeNano: %d\n--%d iteration(s) have passed since starting\n", getpid(), getppid(), sharedClock->seconds, sharedClock->nanoseconds, endSeconds, endNanoseconds, iterations);
                messageBuffer.msgCode = MSG_TYPE_RUNNING;
                messageBuffer.timeSlice = receiveBuffer.timeSlice;
                strcpy(messageBuffer.message, "Still Running...\n");
            }
        }
        if (msgsnd(messageQueueId, &messageBuffer, sizeof(MessageBuffer), 1) == -1) {
            perror("Message send to parent failed\n");
            exit(1);
        }
    }

    shmdt(sharedClock);  // Deallocate shared memory and terminate
    printf("%d: Child is terminating...\n", getpid());
    return EXIT_SUCCESS;
}
