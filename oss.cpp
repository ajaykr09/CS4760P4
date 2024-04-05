#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <queue>
#include <string>
#include <fstream>
#include <chrono>
#include <random>
using namespace std;

#define DISPATCH_DURATION 1000
#define CHILD_LAUNCH_DURATION 1000
#define UNBLOCK_DURATION 1000
#define MSGQ_FILE_PATH "msgq.txt"
#define MSGQ_PROJ_ID 333
#define PERMS 0644
#define MSG_TYPE_BLOCKED 2   // Blocked
#define MSG_TYPE_RUNNING 1   // Still running
#define MSG_TYPE_SUCCESS 0   // Terminating


typedef struct SystemClock {
    int seconds;
    int nanoseconds;
} SystemClock;

void incrementClock(SystemClock* clock, int incrementAmount) {
    clock->nanoseconds += incrementAmount;
    if (clock->nanoseconds >= 1e9) {
        clock->nanoseconds -= 1e9;
        clock->seconds++;
    }
}

typedef struct MessageBuffer {
    long mtype;
    char message[100];
    int timeSlice;
    int blockedUntilSeconds;
    int blockedUntilNanoseconds;
    int msgCode;
} MessageBuffer;

struct ProcessControlBlock {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNanoseconds;
    int blocked;
    int eventBlockedUntilSec;
    int eventBlockedUntilNano;
};

void initProcessTable(ProcessControlBlock processTable[]) {
    for (int i = 0; i < 20; i++) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].startSeconds = 0;
        processTable[i].startNanoseconds = 0;
        processTable[i].blocked = 0;
        processTable[i].eventBlockedUntilSec = 0;
        processTable[i].eventBlockedUntilNano = 0;
    }
}

int findVacantSlot(ProcessControlBlock processTable[], int maxProcesses) {
    for (int i = 0; i < maxProcesses; i++) {
        if (!processTable[i].occupied) {
            return (i + 1);
        }
    }
    return 0;
}

int countRunningProcesses(ProcessControlBlock processTable[], int maxProcesses) {
    int numProcesses = 0;
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].occupied) {
            numProcesses++;
        }
    }
    return (numProcesses == 0) ? 1 : numProcesses;
}

bool isProcessTableEmpty(ProcessControlBlock processTable[], int maxProcesses) {
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].occupied) {
            return false;
        }
    }
    return true;
}

bool areAllProcessesBlocked(ProcessControlBlock processTable[], int maxProcesses) {
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].occupied && !processTable[i].blocked) {
            return false;
        }
    }
    return true;
}

double subtract_time(int minuendSecs, int minuendNanos, int subtrahendSecs, int subtrahendNanos) {
    minuendNanos -= subtrahendNanos;
    if (minuendNanos < 0) {
        minuendNanos += 1e9;
        minuendSecs--;
    }
    minuendSecs -= subtrahendSecs;
    return (minuendSecs + (minuendNanos / 1e9));
}

void printProcessTable(ProcessControlBlock processTable[], int maxProcesses, int currentSeconds, int currentNanoseconds, std::ostream& outputFile) {
    static int nextPrintSeconds = 0;
    static int nextPrintNanoseconds = 0;

    if (currentSeconds > nextPrintSeconds || (currentSeconds == nextPrintSeconds && currentNanoseconds > nextPrintNanoseconds)) {
        printf("OSS PID: %d  SysClockS: %d  SysClockNano: %d  \nProcess Table:\nEntry\tOccupied  PID\tStartS\tStartN\t\tBlocked\tUnblockedS  UnblockedN\n", getpid(), currentSeconds, currentNanoseconds);
        outputFile << "OSS PID: " << getpid() << "  SysClockS: " << currentSeconds << "  SysClockNano " << currentNanoseconds << "  \nProcess Table:\nEntry\tOccupied  PID\tStartS\tStartN\t\tBlocked\tUnblockedS  UnblockedN\n";
        for (int i = 0; i < maxProcesses; i++) {
            std::string tab = (processTable[i].startNanoseconds == 0) ? "\t\t" : "\t";
            std::cout << std::to_string(i + 1) << "\t" << std::to_string(processTable[i].occupied) << "\t" << std::to_string(processTable[i].pid) << "\t" << std::to_string(processTable[i].startSeconds) << "\t" << std::to_string(processTable[i].startNanoseconds) << tab << std::to_string(processTable[i].blocked) << "\t" << std::to_string(processTable[i].eventBlockedUntilSec) << "\t\t" << std::to_string(processTable[i].eventBlockedUntilNano) << std::endl;
            outputFile << std::to_string(i + 1) << "\t" << std::to_string(processTable[i].occupied) << "\t" << std::to_string(processTable[i].pid) << "\t" << std::to_string(processTable[i].startSeconds) << "\t" << std::to_string(processTable[i].startNanoseconds) << tab << std::to_string(processTable[i].blocked) << "\t" << std::to_string(processTable[i].eventBlockedUntilSec) << "\t\t" << std::to_string(processTable[i].eventBlockedUntilNano) << std::endl;
        }
        nextPrintNanoseconds += 500000000;
        if (nextPrintNanoseconds >= 1000000000) {
            nextPrintNanoseconds -= 1000000000;
            nextPrintSeconds++;
        }
    }
}

void updateProcessTableForTerminatedChild(ProcessControlBlock processTable[], pid_t pid, int maxProcesses) {
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].pid == pid) {
            processTable[i].occupied = 0;
            processTable[i].pid = 0;
            processTable[i].startSeconds = 0;
            processTable[i].startNanoseconds = 0;
            processTable[i].blocked = 0;
            processTable[i].eventBlockedUntilSec = 0;
            processTable[i].eventBlockedUntilNano = 0;
            return;
        }
    }
}

void updateProcessTableForBlockedChild(ProcessControlBlock processTable[], pid_t pid, int maxProcesses, int blockedUntilSec, int blockedUntilNano) {
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].pid == pid) {
            processTable[i].blocked = 1;
            processTable[i].eventBlockedUntilSec = blockedUntilSec;
            processTable[i].eventBlockedUntilNano = blockedUntilNano;
            return;
        }
    }
}

void terminateAllProcesses(ProcessControlBlock processTable[], int maxProcesses) {
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].occupied) {
            kill(processTable[i].pid, SIGKILL);
        }
    }
}

std::queue<pid_t> queueHighPriority;  // High Priority Queue
std::queue<pid_t> queueMediumPriority; // Medium Priority Queue
std::queue<pid_t> queueLowPriority;   // Low Priority Queue

int findProcessPosition(ProcessControlBlock processTable[], int maxProcesses, pid_t pid);
int checkForUnblockedProcesses(ProcessControlBlock processTable[], int maxProcesses, int currentSeconds, int currentNanoseconds);
void cleanUpResources(std::string message);

// Scheduler function to determine the next process to run and its time slice
int scheduler(ProcessControlBlock processTable[], int maxProcesses, int* nextProcessIndex, int* timeSlice, int* unblocks, int currentSeconds, int currentNanoseconds) {
    *unblocks = checkForUnblockedProcesses(processTable, maxProcesses, currentSeconds, currentNanoseconds);
    int queueLevel = 0;

    if (isProcessTableEmpty(processTable, maxProcesses) || areAllProcessesBlocked(processTable, maxProcesses)) {
        *nextProcessIndex = -1;
        *timeSlice = 0;
        return queueLevel;
    }

    pid_t nextPid;

    if (!queueHighPriority.empty()) {
        nextPid = queueHighPriority.front();
        *timeSlice = 10000000;  // 10ms
        queueLevel = 0;
    } else if (!queueMediumPriority.empty()) {
        nextPid = queueMediumPriority.front();
        *timeSlice = 20000000;  // 20ms
        queueLevel = 1;
    } else if (!queueLowPriority.empty()) {
        nextPid = queueLowPriority.front();
        *timeSlice = 40000000;  // 40ms
        queueLevel = 2;
    }

    *nextProcessIndex = findProcessPosition(processTable, maxProcesses, nextPid);

    return queueLevel;
}

int findProcessPosition(ProcessControlBlock processTable[], int maxProcesses, pid_t pid) {
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].pid == pid) {
            return i;
        }
    }
    perror("Error: Failed to find PID of scheduled process in the process table.");
    cleanUpResources("Scheduler Error encountered.");
    exit(1);
}

void moveProcessToNextQueue(pid_t pid) {
    if (queueHighPriority.front() == pid) {
        queueHighPriority.pop();
        queueMediumPriority.push(pid);
    } else if (queueMediumPriority.front() == pid) {
        queueMediumPriority.pop();
        queueLowPriority.push(pid);
    } else if (queueLowPriority.front() == pid) {
        queueLowPriority.pop();
        queueLowPriority.push(pid);
    } else {
        perror("Error: Failed to find process to move in scheduling queues");
        cleanUpResources("Scheduler Error encountered.");
        exit(1);
    }
}

void removeProcessFromQueues(pid_t pid, ProcessControlBlock processTable[], int maxProcesses) {
    if (queueLowPriority.front() == pid) {
        queueLowPriority.pop();
    } else if (queueMediumPriority.front() == pid) {
        queueMediumPriority.pop();
    } else if (queueHighPriority.front() == pid) {
        queueHighPriority.pop();
    } else {
        perror("Error: Failed to find process to remove in scheduling queues");
        cleanUpResources("Scheduler Error encountered.");
        exit(1);
    }
}

void addProcessToHighPriorityQueue(pid_t pid) {
    queueHighPriority.push(pid);
}

int checkForUnblockedProcesses(ProcessControlBlock processTable[], int maxProcesses, int currentSeconds, int currentNanoseconds) {
    int unblocks = 0;
    for (int i = 0; i < maxProcesses; i++) {
        if (processTable[i].blocked && (currentSeconds > processTable[i].eventBlockedUntilSec || (currentSeconds == processTable[i].eventBlockedUntilSec && currentNanoseconds > processTable[i].eventBlockedUntilNano))) {
            addProcessToHighPriorityQueue(processTable[i].pid);
            processTable[i].blocked = 0;
            processTable[i].eventBlockedUntilSec = 0;
            processTable[i].eventBlockedUntilNano = 0;
            unblocks++;
        }
    }
    return unblocks;
}

int generateRandomNumber(int min, int max, int pid) {
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count() * pid;
    std::default_random_engine generator(seed);
    std::uniform_int_distribution<int> distribution(min, max);
    int randomNumber = distribution(generator);
    return randomNumber;
}

void launchChildProcess(ProcessControlBlock processTable[], int timeLimit, int maxProcesses);
bool isLaunchIntervalSatisfied(int launchInterval);
void displayHelpMessage();
void handleTimeout(int signal);
void handleCtrlC(int signal);
void cleanUpResources(std::string message);
void outputStatistics(int totalChildren, double totalTimeInSystem, double totalBlockedTime, double totalCPUTime);

volatile sig_atomic_t terminateFlag = 0;  // Global flag for signal handling
ProcessControlBlock processTable[20]; // Initialize Process Table Array of PCB structs

// Globals needed for signal handlers to clean up at anytime
SystemClock* sharedClock;  // Declare global shared memory clock
key_t clockKey = ftok("/tmp", 35);
int sharedMemoryId = shmget(clockKey, sizeof(SystemClock), IPC_CREAT | 0666);    // Initialize shared memory clock
std::ofstream outputFile;   // Initialize file object for logging
int messageQueueId;         // Global for message queue cleanup
int maxSimultaneousProcesses = 1;

void launchChildProcess(ProcessControlBlock processTable[], int timeLimit, int maxProcesses) {
    string randSeconds = std::to_string(generateRandomNumber(1, (timeLimit - 1), getpid()));
    string randNanoseconds = std::to_string(generateRandomNumber(0, 999999999, getpid()));

    pid_t childPid = fork();
    if (childPid == 0) {            // Each child uses exec to run ./worker
        char* args[] = {const_cast<char*>("./worker"), const_cast<char*>(randSeconds.c_str()), const_cast<char*>(randNanoseconds.c_str()), nullptr};
        execvp(args[0], args);
        perror("Error: Failed to execute worker program");
        exit(EXIT_FAILURE);
    } else if (childPid == -1) {  // Fork failed
        perror("Error: Fork has failed");
        exit(EXIT_FAILURE);
    } else {            // Parent updates Process Table with child info after fork()
        int i = (findVacantSlot(processTable, maxProcesses) - 1);
        processTable[i].occupied = 1;
        processTable[i].pid = childPid;
        processTable[i].startSeconds = sharedClock->seconds;
        processTable[i].startNanoseconds = sharedClock->nanoseconds;
        processTable[i].blocked = 0;
        processTable[i].eventBlockedUntilSec = 0;
        processTable[i].eventBlockedUntilNano = 0;
        addProcessToHighPriorityQueue(processTable[i].pid);
        incrementClock(sharedClock, CHILD_LAUNCH_DURATION);  // Simulate child launch overhead
    }
}

bool isLaunchIntervalSatisfied(int launchInterval) {
    static int lastLaunchSeconds = 0;  // Static variables to keep track of last launch time
    static int lastLaunchNanoseconds = 0;

    int elapsedSeconds = sharedClock->seconds - lastLaunchSeconds;
    int elapsedNanoseconds = sharedClock->nanoseconds - lastLaunchNanoseconds;

    while (elapsedNanoseconds < 0) {
        elapsedSeconds--;
        elapsedNanoseconds += 1000000000;
    }

    if (elapsedSeconds > 0 || (elapsedSeconds == 0 && elapsedNanoseconds >= launchInterval)) {
        lastLaunchSeconds = sharedClock->seconds;  // Update the last launch time
        lastLaunchNanoseconds = sharedClock->nanoseconds;
        return true;
    } else {
        return false;
    }
}

void handleTimeout(int signal) {
    cleanUpResources("Timeout Occurred.");
}

void handleCtrlC(int signal) {
    cleanUpResources("Ctrl+C detected.");
}

void cleanUpResources(string message) {
    terminateAllProcesses(processTable, maxSimultaneousProcesses);
    outputFile.close();
    shmdt(sharedClock);
    if (shmctl(sharedMemoryId, IPC_RMID, NULL) == -1) {
        perror("Shmctl failed!!");
        exit(1);
    }
    if (msgctl(messageQueueId, IPC_RMID, NULL) == -1) {
        perror("Msgctl to get rid of queue in parent failed");
        exit(1);
    }
    std::exit(EXIT_SUCCESS);
}

void outputStatistics(int totalChildren, double totalTimeInSystem, double totalBlockedTime, double totalCPUTime) {
    double totalClockTime = sharedClock->seconds + (sharedClock->nanoseconds)/1e9;
    double totalWaitTime = totalTimeInSystem - (totalBlockedTime + totalCPUTime);
    std::cout << "\nFINAL REPORT" << std::endl;
    std::cout << std::fixed << std::setprecision(2) << "Average Wait Time: " << totalWaitTime/totalChildren << " seconds" << std::endl;
    std::cout << "Average CPU Utilization: " << (totalCPUTime/totalClockTime)*100 << "%" << std::endl;
    std::cout << "Average Blocked Time: " << totalBlockedTime/totalChildren << " seconds" << std::endl;
    std::cout << "Total Idle CPU Time: " << totalClockTime - totalCPUTime << " seconds\n" << std::endl;

    outputFile << "\nRUN RESULT REPORT" << std::endl;
    outputFile << std::fixed << std::setprecision(2) << "Average Wait Time: " << totalWaitTime/totalChildren << " seconds" << std::endl;
    outputFile << "Average CPU Utilization: " << (totalCPUTime/totalClockTime)*100 << "%" << std::endl;
    outputFile << "Average Blocked Time: " << totalBlockedTime/totalChildren << " seconds" << std::endl;
    outputFile << "Total Idle CPU Time: " << totalClockTime - totalCPUTime << " seconds\n" << std::endl;
}

int main(int argc, char** argv) {
    int option, numChildren = 1, timeLimit = 2, launchInterval = 100;

    int totalChildren;
    double totalBlockedTime = 0, totalCPUTime = 0, totalTimeInSystem = 0;
    string logfile = "logfile.txt";
    while ((option = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (option) {
            case 'h':
                printf(" [-n proc] [-s simul] [-t timelimitForChildren]\n"
 "[-i intervalInMsToLaunchChildren] [-f logfile]");
                return 0;
                break;
            case 'n':
                numChildren = atoi(optarg);
                totalChildren = numChildren;
                break;
            case 's':
                maxSimultaneousProcesses = atoi(optarg);
                break;
            case 't':
                timeLimit = atoi(optarg);
                break;
            case 'i':
                launchInterval = (1000000 * atoi(optarg));
                break;
            case 'f':
                logfile = optarg;
                break;
        }
    }

    std::signal(SIGALRM, handleTimeout);  // Initialize signal handlers
    std::signal(SIGINT, handleCtrlC);
    alarm(60);   // Timeout timer

    initProcessTable(processTable);      // Initialize local process table
    sharedClock = (SystemClock*)shmat(sharedMemoryId, NULL, 0);    // Attach to global clock
    sharedClock->seconds = 0;
    sharedClock->nanoseconds = 0;

    outputFile.open(logfile);
    if (!outputFile.is_open()) {
        std::cerr << "Error: Logfile didn't open" << std::endl;
        return 1; // Exit with error
    }

    MessageBuffer message;     // Initialize message queue
    key_t msgq_key;
    system("touch msgq.txt");
    if ((msgq_key = ftok(MSGQ_FILE_PATH, MSGQ_PROJ_ID)) == -1) {
        perror("ftok");
        exit(1);
    }
    if ((messageQueueId = msgget(msgq_key, PERMS | IPC_CREAT)) == -1) {
        perror("msgget in parent");
        exit(1);
    }

    int nextProcessIndex;          // Holds PCB location of next process
    int nextTimeSlice;
    int numUnblocks;

    while (numChildren > 0 || !isProcessTableEmpty(processTable, maxSimultaneousProcesses)) {
        int queueLevel = scheduler(processTable, maxSimultaneousProcesses, &nextProcessIndex, &nextTimeSlice, &numUnblocks, sharedClock->seconds, sharedClock->nanoseconds); // Assigns nextProcessIndex to next child
        incrementClock(sharedClock, (DISPATCH_DURATION + (numUnblocks * UNBLOCK_DURATION)));
        printProcessTable(processTable, maxSimultaneousProcesses, sharedClock->seconds, sharedClock->nanoseconds, outputFile);

        // Send message to next child
        if (!isProcessTableEmpty(processTable, maxSimultaneousProcesses) && nextProcessIndex != -1) {
            message.mtype = processTable[nextProcessIndex].pid;     // Send message to child nonblocking
            message.timeSlice = nextTimeSlice;
            message.msgCode = MSG_TYPE_RUNNING;
            message.blockedUntilSeconds = 0;
            message.blockedUntilNanoseconds = 0;
            strcpy(message.message, "Message to child\n");
            if (msgsnd(messageQueueId, &message, sizeof(MessageBuffer), 0) == -1) {
                perror(("oss.cpp: Error: msgsnd to child " + to_string(nextProcessIndex + 1) + " failed\n").c_str());
                cleanUpResources("Error encountered during message send.");
                exit(1);
            }
            // Log message send
            cout << "OSS: Dispatching worker " <<  nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " from queue " << queueLevel << " giving quantum " << message.timeSlice << " at time " << sharedClock->seconds << ":" << sharedClock->nanoseconds << std::endl;
            outputFile << "OSS: Dispatching worker " <<  nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " from queue " << queueLevel << " giving quantum " << message.timeSlice << " at time " << sharedClock->seconds << ":" << sharedClock->nanoseconds << std::endl;
            incrementClock(sharedClock, DISPATCH_DURATION);
            // Receive message from child
            MessageBuffer receiveBuffer;     // Blocking wait to receive message from child
            if (msgrcv(messageQueueId, &receiveBuffer, sizeof(MessageBuffer), getpid(), 0) == -1) {
                perror("oss.cpp: Error: failed to receive message in parent\n");
                cleanUpResources("Error encountered during message receive.");
                exit(1);
            }
            // Log message receive
            incrementClock(sharedClock, receiveBuffer.timeSlice); // Increment clock by time used by child

            if (nextTimeSlice == receiveBuffer.timeSlice) { // If full time slice used
                totalCPUTime += (receiveBuffer.timeSlice)/1e9;   // Log quantum used
                cout << "OSS: Receiving that worker " << nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " ran for full quantum " << receiveBuffer.timeSlice << " ns" << std::endl;
                outputFile << "OSS: Receiving that worker " << nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " ran for full quantum " << receiveBuffer.timeSlice << " ns" << std::endl;
                moveProcessToNextQueue(processTable[nextProcessIndex].pid);

            } else if (receiveBuffer.msgCode == MSG_TYPE_BLOCKED) {  // If process blocked
                totalCPUTime += (receiveBuffer.timeSlice)/1e9;   // Log quantum used
                totalBlockedTime += subtract_time(receiveBuffer.blockedUntilSeconds, receiveBuffer.blockedUntilNanoseconds, sharedClock->seconds, sharedClock->nanoseconds);   // Log blocked time here, subtract blocked until time by current time
                cout << "OSS: Receiving that worker " << nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " ran for " << receiveBuffer.timeSlice << " ns of full quantum " << nextTimeSlice << " ns before becoming blocked" << std::endl;
                outputFile << "OSS: Receiving that worker " << nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " ran for " << receiveBuffer.timeSlice << " ns of full quantum " << nextTimeSlice << " ns before becoming blocked" << std::endl;
                removeProcessFromQueues(processTable[nextProcessIndex].pid, processTable, maxSimultaneousProcesses);
                updateProcessTableForBlockedChild(processTable, processTable[nextProcessIndex].pid, maxSimultaneousProcesses, receiveBuffer.blockedUntilSeconds, receiveBuffer.blockedUntilNanoseconds);
            } else if (receiveBuffer.msgCode == MSG_TYPE_SUCCESS) {     // If child is terminating
                totalCPUTime += (receiveBuffer.timeSlice)/1e9;   // Log quantum used
                totalTimeInSystem += subtract_time(sharedClock->seconds, sharedClock->nanoseconds, processTable[nextProcessIndex].startSeconds, processTable[nextProcessIndex].startNanoseconds); // Log total time in system (current time - start time)
                cout << "OSS: Receiving that worker " << nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " is planning to terminate" << std::endl;

                outputFile << "OSS: Receiving that worker " << nextProcessIndex + 1 << " PID " << processTable[nextProcessIndex].pid << " is planning to terminate" << std::endl;
                wait(0);  // Give terminating process time to clear out of system
                removeProcessFromQueues(processTable[nextProcessIndex].pid, processTable, maxSimultaneousProcesses);
                updateProcessTableForTerminatedChild(processTable, processTable[nextProcessIndex].pid, maxSimultaneousProcesses);
            }
        }
        // Check if conditions are right to launch another child
        if (numChildren > 0 && isLaunchIntervalSatisfied(launchInterval) && findVacantSlot(processTable, maxSimultaneousProcesses)) {
            numChildren--;
            launchChildProcess(processTable, timeLimit, maxSimultaneousProcesses);
        }
    }
    outputStatistics(totalChildren, totalTimeInSystem, totalBlockedTime, totalCPUTime);

    outputFile.close();  // Close file object

    cleanUpResources("OSS has completed running");  // Function to clean up shared memory, message queue, and processes

    return 0;
}
