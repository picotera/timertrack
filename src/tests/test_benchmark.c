#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include "../utils/millisecond_time.h"

#include "../lawn.h"
#include "../timerwheel/timeout.h"


#define SUCCESS 0
#define FAIL 1

#define TIMER_INVALID      0  // 0000...
#define TIMER_RUNNING      1  // 2^0, bit 0
#define TIMER_EXPIRED      2  // 2^1, bit 1
#define TIMER_DELETED      4  // 2^2, bit 2

#define HISTOGRAM_OFFSET  5

#define PRELOAD_OFFSET_MS  1000

typedef struct timerwheel{
    struct timeouts * wheel_ds;
    struct timeout * timers;
} Wheel;


typedef struct experimant_results{
    char type; // L for Lawn, W for wheel..
    int preload_size;
    int unique_ttls;
    int insertions;
    int deletions;
    int expirations;
    mstime_t drift;
    mstime_t max_jitter;
    mstime_t total_insertion_time;
    mstime_t total_deletion_time;
    int histogram_size;
    int* histogram;
} Results;


char* histogram_to_str(int histogram_size, int* histogram)
{
    if (histogram_size <= 0)
    {
        return "U/A";
    }
    histogram_size = histogram_size + HISTOGRAM_OFFSET;
    char buffer[64];
    char* histogram_str = malloc(histogram_size*sizeof(buffer));
    int i;
    for (i=0; i<histogram_size; ++i)
    {
        char* comma_value = (i == histogram_size-1)? "":",";
        sprintf(buffer, "[%d,%d]%s", 
            i-HISTOGRAM_OFFSET, 
            histogram[i], comma_value);
        strcat(histogram_str, buffer);
    }
    return histogram_str;

}

void print_result(Results* results)
{
    if (results == NULL)
    {
        printf(" T | preload | TTLs | insertions | deletions | expirations "
            "| jitter avg (ms) | jitter max (ms) "
            "| insert total (ms) | delete total (ms) "
            "| histogram\n");
    }
    else
    {
        printf(" %c | %d | %d | %d | %d | %d | %lu | %lu | %.3f | %.3f | %s\n", 
            results->type,
            results->preload_size,
            results->unique_ttls,
            results->insertions,
            results->deletions,
            results->expirations,
            results->drift,
            results->max_jitter,
            results->total_insertion_time,
            results->total_deletion_time,
            histogram_to_str(results->histogram_size, results->histogram));
    }
}


void cleanup(Lawn* lawn, Wheel* wheel)
{
    timeouts_close(wheel->wheel_ds);
    if (wheel->timers) free(wheel->timers);
    freeLawn(lawn);
}


// for number of timers PRELOAD in the DS, measure:
int preload(Lawn* lawn, Wheel* wheel, int preload_size, int unique_ttls) 
{

    mstime_t ttl_ms = PRELOAD_OFFSET_MS + (random() % unique_ttls) * 100; // from 5.1 second up
    
    char* idx_str = calloc(128, sizeof(char)); // placeholder for itoa
    int i;
    for (i = 0; i < preload_size; ++i)
    {
        // use i as the next id to populate
        // randomly choose a ttl for the new timer
        mstime_t ttl_ms = 3 + (random() % unique_ttls) * 100; // from 1.1 second up
        sprintf(idx_str, "%d", i);
        int idx_len = strlen(idx_str);   
        lawnAdd(lawn, idx_str, idx_len, ttl_ms);
        timeout_init(&wheel->timers[i], 0); //TIMEOUT_ABS);
        timeouts_add(wheel->wheel_ds, &wheel->timers[i], ttl_ms);
        
    }

    
}


Results* run_experimant(Lawn* lawn,
                   Wheel* wheel,
                   int timer_count,
                   int insertions,
                   int deletions,
                   int expirations,
                   int unique_ttls,
                   int histogram_size)
{
    // init status helper datastructures
    int max_timer_id = timer_count+insertions;
    char* idx_str = calloc(128, sizeof(char)); // placeholder for itoa
    int* status = calloc(max_timer_id, sizeof(int)); 
    int next_idx_to_start = timer_count;
    int i;
    for (i=0; i < next_idx_to_start; ++i)
    {
        status[i] = TIMER_RUNNING;
    }
 
    for (i=next_idx_to_start; i < max_timer_id; ++i)
    {
        status[i] = TIMER_INVALID;
    }

    mstime_t lawn_start;
    mstime_t lawn_end;
    mstime_t wheel_end;

    // init measurements
    mstime_t insertions_time_lawn = 0;
    mstime_t deletions_time_lawn = 0;
    mstime_t lawn_max_jitter = 0;
    mstime_t lawn_jitter_sum = 0;
    int lawn_expired_count = 0;
    int* lhistogram = calloc(histogram_size + HISTOGRAM_OFFSET, sizeof(int)); 
    
    mstime_t insertions_time_wheel = 0;
    mstime_t deletions_time_wheel = 0;
    mstime_t wheel_max_jitter = 0;
    mstime_t wheel_jitter_sum = 0;
    int wheel_expired_count = 0;
    int* whistogram = calloc(histogram_size + HISTOGRAM_OFFSET, sizeof(int));

    int performed_insertions = 0;
    int performed_deletions = 0;

    while ((performed_insertions <= insertions) 
        || (
            (performed_deletions <= deletions) && 
            (performed_deletions < timer_count-1) 
            ) 
        || (
            (lawn_expired_count <= expirations) && 
            (lawn_expired_count < timer_count-1)
            )
        || (
            (wheel_expired_count <= expirations) && 
            (wheel_expired_count < timer_count-1) 
            )
        )
    {
        // randomly either insert, delete, or both
        int insert = (
            (performed_insertions <= insertions) && 
            (rand() % 2 == 0 ));
        
        int delete = (
            (performed_deletions <= deletions) && 
            (performed_deletions < next_idx_to_start) && 
            (rand() % 2 == 0 ));

        if (insert)
        {
            // next id to populate is alrady in next_idx_to_start
            sprintf(idx_str, "%d", next_idx_to_start);
            int idx_len = strlen(idx_str);

            // randomly choose a ttl for the new timer
            mstime_t ttl_ms = (random() % unique_ttls) * 100; // from 0.1 second up

            lawn_start = current_time_ms();
            lawnAdd(lawn, idx_str, idx_len, ttl_ms);
            lawn_end = current_time_ms();
            timeout_init(&wheel->timers[next_idx_to_start], 0);// TIMEOUT_ABS);
            timeouts_add(wheel->wheel_ds, &wheel->timers[next_idx_to_start], ttl_ms);
            mstime_t wheel_end = current_time_ms();
            insertions_time_lawn +=  lawn_end - lawn_start;
            insertions_time_wheel += wheel_end - lawn_start;

            status[next_idx_to_start] = TIMER_RUNNING;
            ++next_idx_to_start;
            ++performed_insertions;
        }
        if (delete)
        {

            // select a random item that is still there
            int idx;
            do
            {
                idx = random() % next_idx_to_start;
            }
            while (status[idx] != TIMER_RUNNING);

            sprintf(idx_str, "%d", idx);

            lawn_start = current_time_ms();
            lawnDel(lawn, idx_str);
            lawn_end = current_time_ms();
            timeout_del(&wheel->timers[idx]);
            mstime_t wheel_end = current_time_ms();
            deletions_time_lawn +=  lawn_end - lawn_start;
            deletions_time_wheel += wheel_end - lawn_start;
            
            status[idx] = TIMER_DELETED;
            ++performed_deletions;
        }

        // pop expired items from both data structure    
        mstime_t now = current_time_ms();
        
        // advance time for wheel
        timeouts_update(wheel->wheel_ds, now);
        // pop expired timers from both DBs
        struct timeout * timer_obj;
        while (NULL != (timer_obj = timeouts_get(wheel->wheel_ds))) {
            // do some POINTER ARITHMATICS to understand what just poped
            int idx = timer_obj - &wheel->timers[0];
            mstime_t wjitter_raw = now - timer_obj->expires;
            mstime_t wjitter = abs(wjitter_raw);
            if ((wjitter_raw < 0 && wjitter < HISTOGRAM_OFFSET) ||
                (wjitter < histogram_size))
            {
                ++whistogram[wjitter_raw + HISTOGRAM_OFFSET];
            }
            wheel_max_jitter = (wheel_max_jitter > wjitter) ? wheel_max_jitter : wjitter;
            wheel_jitter_sum += wjitter;
            ++wheel_expired_count;
            status[idx] = TIMER_EXPIRED;
        }

        now = current_time_ms();
        ElementQueue* queue = lawnPop(lawn);
        while (queue->len > 0)
        {
            ElementQueueNode* node = queuePop(queue);
            int idx = atoi(node->element);
            mstime_t ljitter_raw = now - node->expiration;
            mstime_t ljitter = abs(ljitter_raw);
            if ((ljitter_raw < 0 && ljitter < HISTOGRAM_OFFSET) ||
                (ljitter < histogram_size))
            {
                ++lhistogram[ljitter_raw + HISTOGRAM_OFFSET];
            }
            lawn_max_jitter = (lawn_max_jitter > ljitter) ? lawn_max_jitter : ljitter;
            lawn_jitter_sum += ljitter;
            ++lawn_expired_count;
            status[idx] = TIMER_EXPIRED;
            freeNode(node);
        }
        freeQueue(queue);
    }
    // free(status);

    Results* results = malloc(2*sizeof(Results));

    results[0].type = 'L';
    results[0].preload_size = timer_count;
    results[0].unique_ttls = unique_ttls;
    results[0].insertions = insertions;
    results[0].deletions = deletions;
    results[0].expirations = lawn_expired_count;
    results[0].drift = lawn_jitter_sum/lawn_expired_count;
    results[0].max_jitter = lawn_max_jitter;
    results[0].total_insertion_time = insertions_time_lawn;
    results[0].total_deletion_time = deletions_time_lawn;
    results[0].histogram_size = histogram_size;
    results[0].histogram = lhistogram;

    results[1].type = 'W';
    results[1].preload_size = timer_count;
    results[1].unique_ttls = unique_ttls;
    results[1].insertions = insertions;
    results[1].deletions = deletions;
    results[1].expirations = wheel_expired_count;
    results[1].drift = wheel_jitter_sum/wheel_expired_count;
    results[1].max_jitter = wheel_max_jitter;
    results[1].total_insertion_time = insertions_time_wheel;
    results[1].total_deletion_time = deletions_time_wheel;
    results[1].histogram_size = histogram_size;
    results[1].histogram = whistogram;

    return results;
}




// elapsed time for ITERATIONS insert/del 
// jitter/drift (mean,max diviation from expiration)



int main(int argc, char* argv[]) {
    int do_help, do_verbose;    // flag variables
    // int max_ttl = 1000 * 60 * 60 * 24 * 7;  // a week in milliseconds
    int unique_ttls = 1000;
    int experimant_repetition = 3;
    int preload_size = 100 * 1000;
    int inserts = 0;
    int deletions = 0;
    int expirations = -1;
    int indels = 0;
    int histogram_size = 0;

    struct option longopts[] = {
       { "unique-ttls",    required_argument, NULL,     'u' }, 
       { "preload-size",    required_argument, NULL,    'p' },
       { "inserts",    required_argument, NULL,         'i' },
       { "deletions",      required_argument, NULL,     'd' },
       { "expirations",      required_argument, NULL,   'e' },
       { "indel-actions",      required_argument, NULL, 'a' },
       { "repeat",    required_argument, NULL,          'r' },
       { "histogram-size", required_argument, NULL,     'w' },
       { "help",    no_argument,       & do_help,    1      },
       // { "verbose", no_argument,       & do_verbose, 1   },
       { 0, 0, 0, 0 }
    };
    char c;
    while ((c = getopt_long(argc, argv, ":hu:p:i:d:e:a:r:w:", longopts, NULL)) != -1) {
        switch (c) {
        case 'u':
            unique_ttls = atoi(optarg);
            break;
        case 'p':
            preload_size = atoi(optarg);
            break;
        case 'i':
            inserts = atoi(optarg);
            break;
        case 'd':
            deletions = atoi(optarg);
            break;
        case 'e':
            expirations = atoi(optarg);
            break;
        case 'a':
            indels = atoi(optarg);
            break;
        case 'r':
            experimant_repetition = atoi(optarg);
            break;
        case 'w':
            histogram_size = atoi(optarg);
            break;
        case 'h':
            do_help = 1;
            break;
        case 'v':
            do_verbose = 1;
            break;
        case 0:     /* getopt_long() set a variable, just keep going */
            break;
    #if 0
        case 1:
            /*
             * Use this case if getopt_long() should go through all
             * arguments. If so, add a leading '-' character to optstring.
             * Actual code, if any, goes here.
             */
            break;
    #endif
        case ':':   /* missing option argument */
            fprintf(stderr, "%s: option `-%c' requires an argument\n",
                    argv[0], optopt);
            break;
        case '?':
            do_help = 1;
            break;
        default:    /* invalid option */
            fprintf(stderr, "%s: option `-%c' is invalid: ignored\n",
                    argv[0], optopt);
            break;
        }
    }

    // randomly split the indels to inserts and deletions
    if (indels)
    {
        printf("randomly splitting %d indels to inserts and deletions (overriding any user set values)\n", indels);
        inserts = random() % indels;
        deletions = indels - inserts; 
    }
    else if (inserts == 0 && deletions == 0)
    {
        inserts = 10 * 1000;
        deletions = 10 * 1000;
    }

    int n_timeouts = preload_size;
    n_timeouts += (indels > inserts)? indels: inserts;

    if (expirations < 0 || expirations > n_timeouts)
    {
        expirations = inserts;
        printf("expirations value not set or more than total timers, "
            "setting to insertions: %d (to disable explicitly set to 0 by runnig -e 0)\n", expirations);

    }

    printf("==== user input ====\n");
    printf("preload-size %d\n", preload_size);
    printf("inserts %d\n", inserts);
    printf("deletions %d\n", deletions);
    printf("expirations %d\n", expirations);
    printf("n_timeouts %d\n", n_timeouts);
    printf("experimant_repetition %d\n", experimant_repetition);
    printf("histogram-size %d\n", histogram_size);
    printf("unique-ttls %d\n", unique_ttls);


    Results** all_results = malloc(2*experimant_repetition*sizeof(Results));

    // prepare and run experimants
    int i;
    for (i = 0; i < experimant_repetition; ++i) 
    {

        // init Lawn
        Lawn* lawn = newLawn();
        
        // init Wheel
        int err;
        Wheel* wheel = malloc(sizeof(Wheel));
        wheel->wheel_ds = timeouts_open(0, &err);
        wheel->timers = calloc(n_timeouts, sizeof(struct timeout));

        mstime_t now = current_time_ms();
        timeouts_update(wheel->wheel_ds, now);
        preload(lawn, wheel, preload_size, unique_ttls);
        
        printf("running round %d\n", i+1);
        Results* measurements = run_experimant(lawn, wheel, 
            preload_size, inserts, deletions, expirations, unique_ttls, histogram_size);
        all_results[2*i] = &measurements[0];
        all_results[2*i+1] = &measurements[1];
        cleanup(lawn, wheel);
    }


    print_result(NULL); // print header
    for (i = 0; i < 2*experimant_repetition; i=i+2)
    {
        print_result(all_results[i]);
    }

    for (i = 1; i < 2*experimant_repetition; i=i+2) 
    {
        print_result(all_results[i]);
    }


    // cleanup
    // for (i = 0; i < 2*experimant_repetition; ++i)
    // {
    //     free(all_results[i]);
    // }
    // free(all_results);

    return 0;    
}