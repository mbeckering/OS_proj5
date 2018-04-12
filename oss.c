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

void incrementBlocked();
void printStats();
void checkBlocked(); //parse blocked queue for wake-up candidates
void allocateResource(int, int, int); //(type, quantity, user number)
void terminateUser(int);
void blockUser(int);
int unblockUser(int);
void releaseResource(int, int, int);
void killchildren();
void clearMsg();
int getOpenBitVector(); //returns first open position in bitvector array
int isTimeToSpawnProc(); //returns 1 if it's time to spawn a new user process
void setTimeToNextProc(); //schedule next user process spawn
void incrementClock(unsigned int, unsigned int); //add time to sim clock
void printAlloc(); //print memory resource allocation table
void printAvail(); //print the system's available resources
void printBlocked();
void printRequests();
void initIPC(); //initialize IPC stuff. shared mem, semaphores, queues, etc
void clearIPC(); //clear all that stuff
int safe(int, int, int); //determine if current system is in a safe state
int numUsersRunning(int[]); //returns current number of user processes

/************************ GLOBAL VARIABLES ************************************/
static FILE *mlog;
sem_t *sem; //sim clock mutual exclusion
int seed; //seed for random rolls
int numCurrentUsers = 0;
int R = 20; // Number of resource types
int P = 18; // Max number of user processes
int rclaim_bound = 3; // upper bound for maximum resource claim by users
int blocked[18]; //blocked queue
int bitVector[18]; // keeps track of what simulated pids are running
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_state; //shared memory ID holder for the liveState struct
int shmid_qid; //shared memory ID holder for message queue
static unsigned int *SC_secs; //pointer to shm sim clock (seconds)
static unsigned int *SC_ns; //pointer to shm sim clock (nanoseconds)
unsigned int timeToNextProcNS, timeToNextProcSecs;
unsigned int maxTimeBetweenProcsSecs, maxTimeBetweenProcsNS;
unsigned int spawnNextProcSecs, spawnNextProcNS;
pid_t childpids[18];

//struct for keeping stats to report at the end of a run
struct statistics {
    int req_granted;
    int req_denied;
    unsigned int tot_blocked_secs;
    unsigned int tot_blocked_ns;
    unsigned int blocked_start[18][2];
    int banker_runs;
    int table_counter;
};

struct statistics stats;

//struct for shared memory, holds information about the state of the system
struct state {
    int resource[20]; //total system resources initially available
    int avail[20]; //number of available resources of each type (types 0-19)
    int max_claim[18][20]; //max claimable resource demand of each user process
    int alloc[18][20]; //resources of each type currently allocated to each user
    int pending_req[18][20]; //pending requests (to store if blocked)
    //int need[18][20]; //max - allocation: possible remaining need of each user
};
struct state *liveState; //struct pointer for our system state
struct state liveStateStruct; //struct
struct state testState;

//struct for communications message queue
struct commsbuf {
    long msgtyp;
    int user_sim_pid;
    pid_t user_sys_pid;
    int r_type; //resource type
    int r_qty; //resource quantity
    int user_requesting;//
    int user_terminating; //from user. 1=terminating, 0=not terminating
    int user_denied; //from oss. 1=resources denied (put into blocked queue)
    int user_granted; //from oss. 1=resource request granted
    int user_releasing; //from user. 1=releasing resources r_qty, r_type
};

struct commsbuf msg;


/********************************* MAIN ***************************************/
int main(int argc, char** argv) {
    char str_arg1[10]; //strings for execl call args
    char str_arg2[10];
    maxTimeBetweenProcsNS = 500000000;
    maxTimeBetweenProcsSecs = 0;
    int option; //option int for getopt args
    int verbose = 0; //option for verbose mode
    int next_pnum = 0; //next process number to launch
    double runtime = 2; //runtime limit
    pid_t childpid; //child pid
    int i, j, p, r; //iterators
    int status;
    seed = (int)getpid();
    
    signal (SIGINT, siginthandler);
    
    if (setperiodic(runtime) == -1) {
        perror("Failed to setup periodic interrupt");
        return 1;
    }
    if (setinterrupt() == -1) {
        perror("Failed to set up SIGALRM handler");
        return 1;
    }
    
    // Set up logging
    mlog = fopen("master.log", "w");
    if (mlog == NULL) {
        perror("OSS: error opening log file");
        return -1;
    }
    fprintf(mlog, "OSS: Launched.\n");
    
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
                fprintf(mlog, "OSS: verbose mode ON\n");
                break;
            default:
                break;
        }
    }
    if (verbose == 0) {
        printf("OSS: verbose mode OFF\n");
        fprintf(mlog, "OSS: verbose mode OFF\n");
    }
    
    initIPC();
    
    //initialize blocked queue (-1 indicates vacant queue slot)
    for (i=0; i<P; i++) {
        blocked[i] = -1;
    }
    
    // Determine initial available resources
    for (j=0; j<R; j++) {
        (*liveState).resource[j] = rand_r(&seed) % 10 + 1;
        (*liveState).avail[j] = (*liveState).resource[j];
    }
    
    //set time for first user to spawn
    setTimeToNextProc();
    //go ahead and increment sim clock to that time to prevent useless looping
    incrementClock(spawnNextProcSecs, spawnNextProcNS);
    
    while(1) {
        incrementClock(0, 85981);
        //if it's time, and we're under the user limit, fork a new user
        if(isTimeToSpawnProc() && numCurrentUsers < P) { 
            next_pnum = getOpenBitVector();
            printf("OSS: Generating User %i\n", next_pnum);
            if ( (childpid = fork()) < 0 ){ //terminate code
                perror("OSS: Error forking user");
                killchildren();
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
            bitVector[next_pnum] = 1;
            childpids[next_pnum] = childpid;
            numCurrentUsers++;
            setTimeToNextProc();
        }
        //schedule next user spawn if it's time but we're at max capacity
        else if (numCurrentUsers == P && isTimeToSpawnProc()) {
            setTimeToNextProc();
        }
        
        //if the blocked queue is occupied
        
        
        clearMsg();
        
        //if there's a message in the queue (always msgtype 99 from users)
        if ( msgrcv(shmid_qid, &msg, sizeof(msg), 99, IPC_NOWAIT) != -1 ) {
            //if the user is terminating
            if(msg.user_terminating == 1) {
                terminateUser(msg.user_sim_pid);
                incrementClock(0, 5356);
                //check blocked queue to see if the freed resources from
                //the terminating user can be granted to a blocked user
                checkBlocked();
            }
            //if the user is requesting resources
            else if(msg.user_requesting == 1) {
                if (verbose) {
                    fprintf(mlog, "OSS: User %i is requesting %i of "
                        "resource %i at time %u:%09u \n", 
                        msg.user_sim_pid, msg.r_qty, msg.r_type, 
                        *SC_secs, *SC_ns);
                    stats.table_counter++;
                    if (stats.table_counter == 20) {
                        stats.table_counter = 0;
                        printAlloc();
                    }
                }
                //if the request is more than this user's max claim
                if ( (*liveState).alloc[msg.user_sim_pid][msg.r_type] + msg.r_qty >
                        (*liveState).max_claim[msg.user_sim_pid][msg.r_type]) {
                    //then report it and terminate all
                    printf("OSS: Error: User requesting more than max claim\n");
                    killchildren();
                    clearIPC();
                    exit(1);
                }
                //if request is more of the resource than is available
                else if (msg.r_qty > (*liveState).avail[msg.r_type]) {
                    stats.req_denied ++;
                    //block it
                    blockUser(msg.user_sim_pid);
                }
                //else simulate allocation to determine answer
                else {
                    //do banker's algo: would it result in safe state?
                    if (safe(msg.r_type, msg.r_qty, msg.user_sim_pid)) {
                        //if yes, do the allocation
                        stats.req_granted++;
                        allocateResource(msg.r_type, msg.r_qty, msg.user_sim_pid);
                        incrementClock(0, 25463);
                    }
                    //if no, send this process to the blocked queue
                    else {
                        stats.req_denied ++;
                        blockUser(msg.user_sim_pid);
                        incrementClock(0, 25777);
                    }
                }
                
            }
            //if the user is releasing resources
            else if(msg.user_releasing == 1) {
                if (verbose) {
                fprintf(mlog, "OSS: User %i is releasing %i of resource %i "
                        "at time %u:%09u\n", 
                        msg.user_sim_pid, msg.r_qty, msg.r_type, 
                        *SC_secs, *SC_ns);
                }
                releaseResource(msg.r_type, msg.r_qty, msg.user_sim_pid);
                incrementClock(0, 12345);
                //check blocked queue and see if we can grant a request
                checkBlocked();
            }
        }
        
    }
    
    killchildren();
    printStats();
    clearIPC();
    printf("OSS: normal exit\n");
    return (EXIT_SUCCESS);
}

/******************************* FUNCTIONS ************************************/

//Function to determine if the system is in a safe state (banker's algorithm)
int safe (int rq_type, int rq_amount, int rq_pid) {
    int i=0, j=0, k=0, possible=0, found=0, count=0;
    stats.banker_runs++;
    int need[P][R]; //max additional needs of each resource by each process
    int pcount = numUsersRunning(bitVector);
    //copy currently available resources into local array for simulation
    for (i=0; i<R; i++) {
        testState.avail[i] = (*liveState).avail[i];
    }
    //update test struct to reflect new request
    testState.avail[rq_type] -= rq_amount;
    testState.alloc[rq_pid][rq_type] += rq_amount;
    
    //copy list of running user pids into local array for simulation
    int testVector[P];
    for (i=0; i<P; i++) {
        testVector[i] = bitVector[i];
    }
    //find max additional needs of each resource by each process
    for (i=0; i<P; i++) {
        for (j=0; j<R; j++) {
            need[i][j] = (*liveState).max_claim[i][j] - testState.alloc[i][j];
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
                    if (need[i][j] <= testState.avail[j]) {
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
                                testState.avail[k] = testState.avail[k] 
                                        + testState.alloc[i][k];
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
            incrementClock(0, 35981);
            return 0;
        }
    }
    incrementClock(0, 35982);
    return 1;
}

void checkBlocked() {
    int i, r;
    for(i=0; i<P; i++) {
        //if there's a blocked process here
        if (blocked[i] > -1) {
            //find out what resource is wants
            for (r=0; r<R; r++) {
                if ( (*liveState).pending_req[blocked[i]][r] > 0 ) {
                    //if it's safe to grant it
                    if ((*liveState).pending_req[blocked[i]][r] > (*liveState).avail[r]) {
                    }
                    else if ( safe(r, (*liveState).pending_req[blocked[i]][r], blocked[i])){
                        //then unblock that user (also grants the resource)
                        fprintf(mlog, "OSS: unblocking User %i and granting stored "
                            "request for %i of resource %i at time %u:%09u\n",
                            blocked[i], (*liveState).pending_req[blocked[i]][r],
                            r, *SC_secs, *SC_ns);
                        unblockUser(blocked[i]);
                        stats.req_granted++;
                    }
                    break;
                }
            }
        }
        else break; //once we reach an unoccupied spot in the queue, stop lookin
    }
    incrementClock(0, 25540);
}

void blockUser(int blockpid) {
    int i;
    //store starting time of this blocked sequence for stats
    stats.blocked_start[blockpid][0] = *SC_secs;
    stats.blocked_start[blockpid][1] = *SC_ns;
    
    //store sim pid to next open spot in blocked queue
    for (i=0; i<P; i++) {
        if (blocked[i] == -1) {
            blocked[i] = blockpid;
            break;
        }
    }
    //save the resource request
    (*liveState).pending_req[blockpid][msg.r_type] = msg.r_qty;
    
    //send message to user indicating it's blocked until further notice
    msg.msgtyp = blockpid + 100;
    msg.user_denied = 1;
    msg.user_granted = 0;
    if ( msgsnd(shmid_qid, &msg, sizeof(msg), 0) == -1 ) {
        perror("OSS: error sending init msg");
        killchildren();
        clearIPC();
        exit(0);
    }
    fprintf(mlog, "OSS: blocked User %i's request for %i of resource %i "
        "at time %u:%09u\n", blockpid,
        (*liveState).pending_req[blockpid][msg.r_type], msg.r_type,
        *SC_secs, *SC_ns);
}

int unblockUser(int proc_num) {
    int i, r;
    unsigned int bl_inc_secs, bl_inc_ns, localsec, localns, temp;
    localsec = *SC_secs;
    localns = *SC_ns;
    //increment total blocked time for stat report
    bl_inc_secs = localsec - stats.blocked_start[proc_num][0];
    if (localns >= stats.blocked_start[proc_num][1]) {
        bl_inc_ns = localns - stats.blocked_start[proc_num][1];
    }
    else {
        temp = (BILLION - localns) + stats.blocked_start[proc_num][1];
        bl_inc_ns = temp;
        if (bl_inc_ns >= BILLION) {
            bl_inc_secs++;
            temp = bl_inc_ns - BILLION;
            bl_inc_ns = temp;
        }
    }
    incrementBlocked(bl_inc_secs, bl_inc_ns);
            
    for (i=0; i<P; i++) {
        if (blocked[i] == proc_num) { //found the process to remove from queue
            while(i+1 < P) { //shift next in queue down 1, repeat
                blocked[i] = blocked[i+1];
                i++;
            }
            blocked[17] = -1; //once 17 is moved to 16, clear it by setting -1
            //find the blocked user's request and grant it
            for (r=0; r<R; r++) {
                if ( (*liveState).pending_req[proc_num][r] > 0 ) {
                    allocateResource(r, (*liveState).pending_req[proc_num][r], proc_num);
                    //clear the pending request that was just granted
                    (*liveState).pending_req[proc_num][r] = 0;
                    break;
                }
            }
            return 1;
        }
    }
    printf("OSS: unblockUser: Couldn't find proc_num in queue\n");
    return -1;
}

void allocateResource(int rtype, int ramount, int userpid) {
    //allocate the resource to the user
    (*liveState).alloc[userpid][rtype] += ramount;
    //decrement available resources
    (*liveState).avail[rtype] -= ramount;
    //send message unblocking user & granting resource
    clearMsg();
    msg.msgtyp = userpid + 100;
    msg.user_denied = 0;
    msg.user_granted = 1;
    if ( msgsnd(shmid_qid, &msg, sizeof(msg), 0) == -1 ) {
        perror("OSS: error sending init msg");
        killchildren();
        clearIPC();
        exit(0);
    }
}

void releaseResource(int rtype, int ramount, int userpid) {
    //remove the resources from the allocation table
    (*liveState).alloc[userpid][rtype] -= ramount;
    //add resources back to available array
    (*liveState).avail[rtype] += ramount;
}
void terminateUser(int termpid) {
    int status, i;
    unsigned int temp;
    //make sure the user has terminated
    waitpid(msg.user_sys_pid, &status, 0);
    //add user's life time to total
    /*
    totalUserLifetime_secs += pct[termpid].totalLIFEtime_secs;
    totalUserLifetime_ns += pct[termpid].totalLIFEtime_ns;
    if (totalUserLifetime_ns >= BILLION) {
        totalUserLifetime_secs++;
        temp = totalUserLifetime_ns - BILLION;
        totalUserLifetime_ns = temp;
    }
    //add user's wait time to total
    
    totalWaitTime_secs += 
            (pct[termpid].totalLIFEtime_secs - pct[termpid].totalCPUtime_secs);
    totalWaitTime_ns +=
            (pct[termpid].totalLIFEtime_ns - pct[termpid].totalCPUtime_ns);
    if (totalWaitTime_ns >= BILLION) {
        totalWaitTime_secs++;
        temp = totalWaitTime_ns - BILLION;
        totalWaitTime_ns = temp;
    }
    */
    
    //reclaim user's resources
    for (i=0; i<R; i++) {
        //add allocated resources back to available
        (*liveState).avail[i] += (*liveState).alloc[termpid][i];
        //remove allocated resources
        (*liveState).alloc[termpid][i] = 0;
        //remove max claims and any pending requests of terminating user
        (*liveState).max_claim[termpid][i] = 0;
        (*liveState).pending_req[termpid][i] = 0;
    }
    
    bitVector[termpid] = 0;
    numCurrentUsers--;
    printf("OSS: User %d has terminated. Users alive: %d\n",
        msg.user_sim_pid, numCurrentUsers);
}

void killchildren() {
    int sh_status, status, i;
    pid_t sh_wpid, result;
    for (i=0; i < P ; i++) {
        result = waitpid(childpids[i], &status, WNOHANG);
        if (result == 0) {//child is still alive
            kill(childpids[i], SIGINT);
            waitpid(childpids[i], &status, 0);
        }
        else if (result == -1) {//error getting child status
            //exit(0);
        }
        else {//child has already terminated
            
        }
    }
}

void clearMsg() {
    msg.msgtyp = -1;
    msg.user_sim_pid = 0;
    msg.user_sys_pid = 0;
    msg.r_type = 0;
    msg.r_qty = 0;
    msg.user_requesting = 0;
    msg.user_terminating = 0;
    msg.user_denied = 0;
    msg.user_granted = 0;
    msg.user_releasing = 0;
}

//sets length of sim time from now until next child process spawn
//AND sets variables to indicate when that time will be on the sim clock
void setTimeToNextProc() {
    unsigned int temp;
    unsigned int localsecs = *SC_secs;
    unsigned int localns = *SC_ns;
    timeToNextProcSecs = rand_r(&seed) % (maxTimeBetweenProcsSecs + 1);
    timeToNextProcNS = rand_r(&seed) % (maxTimeBetweenProcsNS + 1);
    spawnNextProcSecs = localsecs + timeToNextProcSecs;
    spawnNextProcNS = localns + timeToNextProcNS;
    if (spawnNextProcNS >= BILLION) { //roll ns to s if > bill
        spawnNextProcSecs++;
        temp = spawnNextProcNS - BILLION;
        spawnNextProcNS = temp;
    }
}

int isTimeToSpawnProc() {
    int return_val = 0;
    sem_wait(sem);
    unsigned int localsec = *SC_secs;
    unsigned int localns = *SC_ns;
    sem_post(sem);
    if ( (localsec > spawnNextProcSecs) || 
            ( (localsec >= spawnNextProcSecs) && (localns >= spawnNextProcNS) ) ) {
        return_val = 1;
    }
    return return_val;
}

int getOpenBitVector() {
    int i;
    int return_val = -1;
    for (i=0; i<P; i++) {
        if (bitVector[i] == 0) {
            return_val = i;
            break;
        }
    }
    return return_val;
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
    fprintf(mlog, "   R0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\nAVL ");
    for(i=0; i<R; i++){
        fprintf(mlog, "%i\t", (*liveState).avail[i]);
    }
    fprintf(mlog, "\nTOT ");
    for(i=0; i<R; i++){
        fprintf(mlog, "%i\t", (*liveState).resource[i]);
    }
    fprintf(mlog, "\n");
}

void incrementBlocked(unsigned int add_secs, unsigned int add_ns) {
    unsigned int temp;
    stats.tot_blocked_secs += add_secs;;
    stats.tot_blocked_ns += add_ns;
    //rollover nanoseconds offset if needed
    if (stats.tot_blocked_ns >= BILLION) {
        stats.tot_blocked_secs++;
        temp = stats.tot_blocked_ns - BILLION;
        stats.tot_blocked_ns = temp;
    }
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

void printRequests() {
    int p, r;
    printf("PENDING RESOURCE REQUESTS:\n");
    printf("\tR0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\n");
    for (p=0; p<P; p++) {
        printf("P%i\t", p);
        for (r=0; r<R; r++) {
            printf("%i\t", (*liveState).pending_req[p][r]);
        }
        printf("\n");
    }
}

void printAlloc() {
    int r, c;
    fprintf(mlog, "Current system resource allocation:\n");
    fprintf(mlog, "\tR0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\n");
    for (r=0; r<P; r++) {
        fprintf(mlog, "P%i\t", r);
        for (c=0; c<R; c++) {
            if ((*liveState).alloc[r][c] > 0){
                fprintf(mlog, "%i\t", (*liveState).alloc[r][c]);
            }
            else {
                fprintf(mlog, "-\t");
            }
        }
        fprintf(mlog, "\n");
    }
    printAvail();
}

void printBlocked(){
    int i;
    printf("Blocked Queue: ");
    for (i=0; i<P; i++) {
        printf("%i.", blocked[i]);
    }
    printf("\n");
}

void printStats() {
    fprintf(mlog, "REPORT: Shutting down at %u:%09u\n", *SC_secs, *SC_ns);
    printf("REPORT: Shutting down at %u:%09u\n", *SC_secs, *SC_ns);
    fprintf(mlog, "REPORT: Requests granted: %i\n", stats.req_granted);
    printf("REPORT: Requests granted: %i\n", stats.req_granted);
    
    fprintf(mlog, "REPORT: Deadlock avoidance algorithm runs: %i\n", stats.banker_runs);
    printf("REPORT: Deadlock avoidance algorithm runs: %i\n", stats.banker_runs);
    int totalreq = stats.req_denied + stats.req_granted;
    double percent = 100 * (double)((double)stats.req_granted / (double)totalreq);
    printf("REPORT: %f percent of requests were granted.\n", percent);
    fprintf(mlog, "REPORT: %f percent of requests were granted.\n", percent);
    
    printf("REPORT: Total time all users spent blocked: %u:%09u\n",
        stats.tot_blocked_secs, stats.tot_blocked_ns);
    fprintf(mlog, "REPORT: Total time all users spent blocked: %u:%09u\n",
        stats.tot_blocked_secs, stats.tot_blocked_ns);
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
    fflush(mlog);
    fclose(mlog);
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
    printf("OSS: Timer Interrupt Detected! signo = %d\n", signo);
    fprintf(mlog, "OSS: Terminated: Timed Out\n");
    killchildren();
    printStats();
    clearIPC();
    printf("OSS: Terminated: Timed Out\n");
    exit(0);
}

static void siginthandler(int sig_num) {
    printf("\nOSS: Interrupt detected! signo = %d\n", getpid(), sig_num);
    fprintf(mlog, "OSS: Terminated: Interrupted by SIGINT\n");
    killchildren();
    printStats();
    clearIPC();
    printf("OSS: Terminated: Interrupted\n");
    exit(0);
}