/* 
 * File:   oss.c
 * Author: Michael Beckering
 * Project 5
 * Spring 2018 CS-4760-E01
 * Created on April 4, 2018, 10:12 AM
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>

#include <semaphore.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SNAME "clocksem"
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define SHMKEY_state 4020014
#define SHMKEY_msgq 4020015
#define BUFF_SZ sizeof (unsigned int)
#define BILLION 1000000000

/********************* FUNCTION PROTOTYPES ************************************/
static int setperiodic(double); //periodic interrupt setup
static int setinterrupt(); //periodic interrupt setup
static void interrupt(int signo, siginfo_t *info, void *context); //handler
static void siginthandler(int sig_num); //sigint handler

void incrementClock(unsigned int, unsigned int); //add time to sim clock
void printAlloc(); //print memory resource allocation table
void printAvail(); //print the system's available resources
void initIPC(); //initialize IPC stuff. shared mem, semaphores, queues, etc
void clearIPC(); //clear all that stuff
int safe(); //determine if current system is in a safe state
int numUsersRunning(int[]); //returns current number of user processes

/************************ GLOBAL VARIABLES ************************************/
sem_t *sem; //sim clock mutual exclusion
int R = 20; // Number of resource types
int P = 18; // Max number of user processes
int rclaim_bound = 3; // upper bound for maximum resource claim by users
int bitVector[18]; // keeps track of what simulated pids are running
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_state; //shared memory ID holder for the liveState struct
int shmid_qid; //shared memory ID holder for message queue
static unsigned int *SC_secs; //pointer to shm sim clock (seconds)
static unsigned int *SC_ns; //pointer to shm sim clock (nanoseconds)

//struct for shared memory, holds information about the state of the system
struct state {
    int resource[20]; //total system resources initially available
    int avail[20]; //number of available resources of each type (types 0-19)
    int max_claim[18][20]; //max claimable resource demand of each user process
    int alloc[18][20]; //resources of each type currently allocated to each user
    //int need[18][20]; //max - allocation: possible remaining need of each user
};
struct state *liveState; //struct for our system state
struct state liveStateStruct; //pointer to that struct for shmat


/********************************* MAIN ***************************************/
int main(int argc, char** argv) {
    char str_arg1[10]; //strings for execl call args
    char str_arg2[10];
    int option; //option int for getopt args
    int verbose = 0; //option for verbose mode
    int next_pnum = 0; //next process number to launch
    double runtime = 2; //runtime limit
    pid_t childpid; //child pid
    int i, j; //iterators
    int status;
    int seed = getpid();
    
    signal (SIGINT, siginthandler);
    
    if (setperiodic(runtime) == -1) {
        perror("Failed to setup periodic interrupt");
        return 1;
    }
    if (setinterrupt() == -1) {
        perror("Failed to set up SIGALRM handler");
        return 1;
    }
    
    while ((option = getopt(argc, argv, "hv")) != -1) {
        switch(option) {
            case 'h':
                printf("Usage: ./oss [-v]\n");
                printf("-v: verbose mode on\n");
                exit (0);
                break;
            case 'v':
                verbose = 1;
                printf("OSS: verbose mode ON\n");
                break;
            default:
                break;
        }
    }
    if (verbose == 0) {
        printf("OSS: verbose mode OFF\n");
    }
    
     initIPC();
    
    // Determine initial available resources
    for (j=0; j<R; j++) {
        (*liveState).resource[j] = rand_r(&seed) % 10 + 1;
        (*liveState).avail[j] = (*liveState).resource[j];
    }
    
    printAlloc();
    if ( (childpid = fork()) < 0 ){ //terminate code
        perror("OSS: Error forking user");
        clearIPC();
        exit(0);
    }
    if (childpid == 0) { //child code
        sprintf(str_arg1, "%i", rclaim_bound); //build arg1 string
        sprintf(str_arg2, "%i", next_pnum); //build arg2 string
        execlp("./user", "./user", str_arg1, str_arg2, (char *)NULL);
        perror("OSS: execl() failure"); //report & exit if exec fails
        exit(0);
    }
    pid_t waitpid;
    while( (waitpid = wait(&status)) > 0);
    clearIPC();
    printf("OSS: normal exit\n");
    return (EXIT_SUCCESS);
}

/******************************* FUNCTIONS ************************************/

//Function to determine if the system is in a safe state (banker's algorithm)
int safe () {
    int i=0, j=0, k=0, possible=0, found=0, count=0;
    int need[P][R]; //max additional needs of each resource by each process
    int pcount = numUsersRunning(bitVector);
    //copy currently available resources into local array for simulation
    int currentavail[R];
    for (i=0; i<R; i++) {
        currentavail[i] = (*liveState).avail[i];
    }
    //copy list of running user pids into local array for simulation
    int testVector[P];
    for (i=0; i<P; i++) {
        testVector[i] = bitVector[i];
    }
    //find max additional needs of each resource by each process
    for (i=0; i<P; i++) {
        for (j=0; j<R; j++) {
            need[i][j] = (*liveState).max_claim[i][j] - (*liveState).alloc[i][j];
        }
    }
    possible = 1;
    
    while (count < P) {
        found = 0;
        //for each process
        for (i=0; i<P; i++) {
            //that is RUNNING in our system copy
            if (testVector[i] == 1) {
                //and for each resource type for this running process
                for (j=0; j<R; j++) {
                    //if this process could claim every resource it could
                    //possibly need from currently available resources
                    if (need[i][j] <= currentavail[j]) {
                        //if this loop made it to R-1, then this could be
                        //granted all resources it needs to run to completion
                        if (j == R-1) {
                            found = 1;
                            count++; //move toward exiting while loop
                            //simulate termination of this process
                            testVector[i] = 0;
                            //increment sim available resources we would gain
                            //from termination of this process
                            for (k=0; k<R; k++){
                                currentavail[k] = currentavail[k] 
                                        + (*liveState).alloc[i][k];
                            }
                        }
                    }
                    //go on to next process if there aren't enough available
                    //resources to satisfy all of this user's possible needs
                    else break;
                }//end resource for loop
            }//end running process if statement
        } //end process for loop
        pcount = numUsersRunning(testVector);
        if (found == 1 && pcount == 0) {
            break;
        }
        else if (found == 0) {
            printf("OSS: System not in safe state\n");
            return 0;
        }
    }
    printf("OSS: System is in safe state\n");
    return 1;
}

int numUsersRunning(int vector[]) {
    int i = 0;
    int pcount = 0;
    for (i=0; i<P; i++) {
        if (vector[i] == 1) {
            pcount++;
        }
    }
    return pcount;
}

void initIPC() {
    //create semaphore for mutex protection of sim clock
    sem = sem_open(SNAME, O_CREAT, 0777, 1);
    
    //sim clock seconds
    shmid_sim_secs = shmget(SHMKEY_sim_s, BUFF_SZ, 0777 | IPC_CREAT);
        if (shmid_sim_secs == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_sim_secs");
            exit(1);
        }
    SC_secs = (unsigned int*) shmat(shmid_sim_secs, 0, 0);
    //sim clock nanoseconds
    shmid_sim_ns = shmget(SHMKEY_sim_ns, BUFF_SZ, 0777 | IPC_CREAT);
        if (shmid_sim_ns == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_sim_ns");
            exit(1);
        }
    SC_ns = (unsigned int*) shmat(shmid_sim_ns, 0, 0);
    //shared memory for system state struct
    shmid_state = shmget(SHMKEY_state, sizeof(struct state), 0777 | IPC_CREAT);
    if (shmid_state == -1) { //terminate if shmget failed
            perror("OSS: error in shmget state");
            exit(1);
        }
    liveState = (struct state *) shmat(shmid_state, NULL, 0);
    if (liveState == (struct state*)(-1) ) {
        perror("OSS: error in shmat liveState");
        exit(1);
    }
    //message queue
    if ( (shmid_qid = msgget(SHMKEY_msgq, 0777 | IPC_CREAT)) == -1 ) {
        perror("OSS: Error generating message queue");
        exit(0);
    }

}

void printAvail() {
    int i;
    printf("System  R0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\navail:  ");
    for(i=0; i<R; i++){
        printf("%i\t", (*liveState).avail[i]);
    }
    printf("\n");
}

void incrementClock(unsigned int add_secs, unsigned int add_ns) {
    sem_wait(sem); //mutex protection
    unsigned int localsec = *SC_secs;
    unsigned int localns = *SC_ns;
    unsigned int temp;
    localsec = localsec + add_secs;
    localns = localns + add_ns;
    //rollover nanoseconds offset if needed
    if (localns >= BILLION) {
        localsec++;
        temp = localns - BILLION;
        localns = temp;
    }
    //update the sim clock in shared memory
    *SC_secs = localsec;
    *SC_ns = localns;
    sem_post(sem);
}

void printAlloc() {
    int r, c;
    printf("Current system resource allocation:\n");
    printf("\tR0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\n");
    for (r=0; r<P; r++) {
        printf("P%i\t", r);
        for (c=0; c<R; c++) {
            printf("%i\t", (*liveState).alloc[r][c]);
        }
        printf("\n");
    }
    printAvail();
}

void clearIPC() {
    printf("OSS: Clearing IPC resources...\n");
    //unlink & close the semaphore
    if (sem_unlink(SNAME) == -1) {
        perror("OSS: Error unlinking semaphore");
    }
    if (sem_close(sem) == -1) {
        perror("OSS: Error closing semaphore");
    }
    //close shared memory (sim clock)
    if ( shmctl(shmid_sim_secs, IPC_RMID, NULL) == -1) {
        perror("OSS: error removing shared memory");
    }
    if ( shmctl(shmid_sim_ns, IPC_RMID, NULL) == -1) {
        perror("OSS: error removing shared memory");
    }
    //close shared memory system struct
    if ( shmctl(shmid_state, IPC_RMID, NULL) == -1) {
        perror("OSS: error removing shared memory");
    }
    //close message queue
    if ( msgctl(shmid_qid, IPC_RMID, NULL) == -1 ) {
        perror("OSS: Error removing message queue");
        exit(0);
    }
}

/************************* INTERRUPT HANDLING *********************************/
//this function taken from UNIX text
static int setperiodic(double sec) {
    timer_t timerid;
    struct itimerspec value;
    
    if (timer_create(CLOCK_REALTIME, NULL, &timerid) == -1)
        return -1;
    value.it_interval.tv_sec = (long)sec;
    value.it_interval.tv_nsec = (sec - value.it_interval.tv_sec)*BILLION;
    if (value.it_interval.tv_nsec >= BILLION) {
        value.it_interval.tv_sec++;
        value.it_interval.tv_nsec -= BILLION;
    }
    value.it_value = value.it_interval;
    return timer_settime(timerid, 0, &value, NULL);
}

//this function taken from UNIX text
static int setinterrupt() {
    struct sigaction act;
    
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = interrupt;
    if ((sigemptyset(&act.sa_mask) == -1) ||
            (sigaction(SIGALRM, &act, NULL) == -1))
        return -1;
    return 0;
}

static void interrupt(int signo, siginfo_t *info, void *context) {
    printf("Master: Timer Interrupt Detected! signo = %d\n", signo);
    //killchildren();
    clearIPC();
    //close log file
    //fprintf(mlog, "Master: Terminated: Timed Out\n");
    //fclose(mlog);
    printf("Master: Terminated: Timed Out\n");
    exit(0);
}

static void siginthandler(int sig_num) {
    //printf("Master pid=%ld: Ctrl+C interrupt detected! signo = %d\n", getpid(), sig_num);
    
    //killchildren();
    clearIPC();
    
    //fprintf(mlog, "Master: Terminated: Interrupted\n");
    //fclose(mlog);
    
    printf("Master: Terminated: Interrupted\n");
    exit(0);
}