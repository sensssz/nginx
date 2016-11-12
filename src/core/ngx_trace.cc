#include <ngx_trace.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>

using std::ifstream;
using std::ofstream;
using std::getline;
using std::ofstream;
using std::vector;
using std::endl;
using std::string;
using std::to_string;
using std::set;

#define TARGET_PATH_COUNT 10
#define NUMBER_OF_FUNCTIONS 21
#define LATENCY
#define MONITOR

ulint transaction_id = 0;

class TraceTool {
private:
    static TraceTool *instance;
    /*!< Mutex for protecting instance. */
    static pthread_mutex_t instance_mutex;
    /*!< Start time of the current transaction. */
    vector<vector<int> > function_times;
    /*!< Stores the running time of the child functions
         and also transaction latency (the last one). */
    vector<ulint> transaction_start_times;
    /*!< Stores the start time of transactions. */

    TraceTool();

    TraceTool(TraceTool const &) { };
public:
    static timespec global_last_query;
    /*!< Mutex for protecting global_last_query */
    static pthread_mutex_t last_query_mutex;
    /*!< A read-write lock for protecting function_times. */
    static pthread_rwlock_t data_lock;
    static thread_local timespec trans_start;
    static thread_local ulint current_transaction_id;
    /*!< Each thread can execute only one transaction at
         a time. This is the ID of the current transactions. */

    static thread_local int path_count;
    /*!< Number of node in the function call path. Used for
         tracing running time of functions. */

    static thread_local bool is_commit;
    /*!< True if the current transactions commits. */
    static thread_local bool commit_successful; /*!< True if the current transaction successfully commits. */
    static bool should_shutdown;
    static pthread_t back_thread;
    static ofstream log_file;

    int id;

    /********************************************************************//**
    The Singleton pattern. Used for getting the instance of this class. */
    static TraceTool *get_instance();

    /********************************************************************//**
    Check if we should trace the running time of function calls. */
    static bool should_monitor();

    /********************************************************************//**
    Calculate time interval in nanoseconds. */
    static long difftime(timespec start, timespec end);

    /********************************************************************//**
    Periodically checks if any query comes in in the last 5 second.
    If no then dump all logs to disk. */
    static void *check_write_log(void *);

    /********************************************************************//**
    Get the current time in nanosecond. */
    static timespec get_time();
    /********************************************************************//**
    Get the current time in microsecond. */
    static ulint now_micro();

    /********************************************************************//**
    Start a new query. This may also start a new transaction. */
    void start_trx();
    /********************************************************************//**
    End a new query. This may also end the current transaction. */
    void end_trx();
    /********************************************************************//**
    End the current transaction. */
    void end_transaction();
    /********************************************************************//**
    Dump data about function running time and latency to log file. */
    void write_latency(string dir);
    /********************************************************************//**
    Write necessary data to log files. */
    void write_log();

    /********************************************************************//**
    Record running time of a function. */
    void add_record(int function_index, long duration);
};

TraceTool *TraceTool::instance = NULL;
pthread_mutex_t TraceTool::instance_mutex = PTHREAD_MUTEX_INITIALIZER;
thread_local ulint TraceTool::current_transaction_id = 0;

timespec TraceTool::global_last_query;
pthread_mutex_t TraceTool::last_query_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t TraceTool::data_lock = PTHREAD_RWLOCK_INITIALIZER;

ofstream TraceTool::log_file;

thread_local int TraceTool::path_count = 0;
thread_local bool TraceTool::is_commit = false;
thread_local bool TraceTool::commit_successful = true;
thread_local timespec TraceTool::trans_start;

bool TraceTool::should_shutdown = false;
pthread_t TraceTool::back_thread;

/* Define MONITOR if needs to trace running time of functions. */
#ifdef MONITOR
static thread_local timespec function_start;
static thread_local timespec function_end;
static thread_local timespec call_start;
static thread_local timespec call_end;
#endif

void log_command(const char *command) {
    TraceTool::get_instance()->log_file << "[Thread " << pthread_self() << "]: " << command << endl;
}

void QUERY_START() {
    TraceTool::get_instance()->global_last_query = TraceTool::get_time();
}

void SESSION_START() {
#ifdef LATENCY
    TraceTool::get_instance()->start_trx();
#endif
}

void SESSION_END(int successful) {
#ifdef LATENCY
    TraceTool::get_instance()->is_commit = true;
    TraceTool::get_instance()->commit_successful = successful;
    TraceTool::get_instance()->end_trx();
#endif
}

void PATH_INC() {
#ifdef LATENCY
    TraceTool::get_instance()->path_count++;
#endif
}

void PATH_DEC() {
#ifdef LATENCY
    TraceTool::get_instance()->path_count--;
#endif
}

void PATH_SET(int path_count) {
#ifdef LATENCY
    TraceTool::get_instance()->path_count = path_count;
#endif
}

int PATH_GET() {
#ifdef LATENCY
    return TraceTool::get_instance()->path_count;
#endif
}

void TRACE_FUNCTION_START() {
#ifdef MONITOR
    if (TraceTool::should_monitor()) {
        clock_gettime(CLOCK_REALTIME, &function_start);
    }
    PATH_INC();
#endif
}

void TRACE_FUNCTION_END() {
#ifdef MONITOR
    PATH_DEC();
    if (TraceTool::should_monitor()) {
        clock_gettime(CLOCK_REALTIME, &function_end);
        long duration = TraceTool::difftime(function_start, function_end);
        TraceTool::get_instance()->add_record(0, duration);
    }
#endif
}

int TRACE_START() {
#ifdef MONITOR
    PATH_DEC();
    if (TraceTool::should_monitor()) {
        clock_gettime(CLOCK_REALTIME, &call_start);
    }
    PATH_INC();
#endif
    return 0;
}

int TRACE_END(int index) {
#ifdef MONITOR
    PATH_DEC();
    if (TraceTool::should_monitor()) {
        clock_gettime(CLOCK_REALTIME, &call_end);
        long duration = TraceTool::difftime(call_start, call_end);
        TraceTool::get_instance()->add_record(index, duration);
    }
    PATH_INC();
#endif
    return 0;
}

timespec get_trx_start() {
    return TraceTool::get_instance()->trans_start;
}

/********************************************************************//**
Get the current TraceTool instance. */
TraceTool *TraceTool::get_instance() {
    if (instance == NULL) {
        pthread_mutex_lock(&instance_mutex);
        /* Check instance again after entering the ciritical section
        to prevent double initilization. */
        if (instance == NULL) {
            instance = new TraceTool;
#ifdef LATENCY
            /* Create a background thread for dumping function running time
                and latency data. */
            pthread_t write_thread;
            pthread_create(&write_thread, NULL, check_write_log, NULL);
#endif
        }
        pthread_mutex_unlock(&instance_mutex);
    }
    return instance;
}

TraceTool::TraceTool() : function_times() {
    /* Open the log file in append mode so that it won't be overwritten */
    log_file.open("/home/jiamin/nginx/vprofiler/logs/log_file", std::ofstream::out);
    const int number_of_functions = NUMBER_OF_FUNCTIONS + 2;
    vector<int> function_time;
    function_time.push_back(0);
    for (int index = 0; index < number_of_functions; index++) {
        function_times.push_back(function_time);
        function_times[index].reserve(500000);
    }
    transaction_start_times.reserve(500000);
    transaction_start_times.push_back(0);

    srand(time(0));
}

bool TraceTool::should_monitor() {
    return path_count == TARGET_PATH_COUNT;
}

void *TraceTool::check_write_log(void *arg) {
    /* Runs in an infinite loop and for every 5 seconds,
       check if there's any query comes in. If not, then
       dump data to log files. */
    while (true) {
        sleep(5);
        timespec now = get_time();
        if (now.tv_sec - global_last_query.tv_sec >= 10 && transaction_id > 0) {
            /* Create a new TraceTool instance. */
            TraceTool *old_instance = instance;
            instance = new TraceTool;
            instance->id = old_instance->id;

            /* Reset the global transaction ID. */
            transaction_id = 0;

            /* Dump data in the old instance to log files and
               reclaim memory. */
            old_instance->write_log();
            delete old_instance;
        }
    }
    return NULL;
}

timespec TraceTool::get_time() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now;
}

long TraceTool::difftime(timespec start, timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
}

ulint TraceTool::now_micro() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/********************************************************************//**
Start a new query. This may also start a new transaction. */
void TraceTool::start_trx() {
    is_commit = false;
    /* This happens when a log write happens, which marks the end of a phase. */
    if (current_transaction_id > transaction_id) {
        current_transaction_id = 0;
    }
#ifdef LATENCY
    commit_successful = true;
    /* Use a write lock here because we are appending content to the vector. */
    pthread_rwlock_wrlock(&data_lock);
    current_transaction_id = transaction_id++;
    transaction_start_times[current_transaction_id] = now_micro();
    for (vector<vector<int> >::iterator iterator = function_times.begin();
         iterator != function_times.end();
         ++iterator) {
        iterator->push_back(0);
    }
    transaction_start_times.push_back(0);
    pthread_rwlock_unlock(&data_lock);
    pthread_mutex_lock(&last_query_mutex);
    clock_gettime(CLOCK_REALTIME, &global_last_query);
    pthread_mutex_unlock(&last_query_mutex);
    trans_start = get_time();
#endif
}

void TraceTool::end_trx() {
#ifdef LATENCY
    if (is_commit) {
        end_transaction();
    }
#endif
}

void TraceTool::end_transaction() {
#ifdef LATENCY
    timespec now = get_time();
    long latency = difftime(trans_start, now);
    pthread_rwlock_rdlock(&data_lock);
    function_times.back()[current_transaction_id] = (int) latency;
    if (!commit_successful) {
        transaction_start_times[current_transaction_id] = 0;
    }
    is_commit = false;
    pthread_rwlock_unlock(&data_lock);
#endif
}

void TraceTool::add_record(int function_index, long duration) {
    if (current_transaction_id > transaction_id) {
        current_transaction_id = 0;
    }
    pthread_rwlock_rdlock(&data_lock);
    function_times[function_index][current_transaction_id] += duration;
    pthread_rwlock_unlock(&data_lock);
}

void TraceTool::write_latency(string dir) {
    ofstream tpcc_log;
    tpcc_log.open(dir + "tpcc");

    pthread_rwlock_wrlock(&data_lock);
    for (ulint index = 0; index < transaction_start_times.size(); ++index) {
        ulint start_time = transaction_start_times[index];
        if (start_time > 0) {
            tpcc_log << start_time << endl;
        }
    }

    int function_index = 0;
    for (vector<vector<int> >::iterator iterator = function_times.begin();
         iterator != function_times.end(); ++iterator) {
        ulint number_of_transactions = iterator->size();
        for (ulint index = 0; index < number_of_transactions; ++index) {
            if (transaction_start_times[index] > 0) {
                long latency = (*iterator)[index];
                tpcc_log << function_index << ',' << latency << endl;
            }
        }
        function_index++;
        vector<int>().swap(*iterator);
    }
    vector<vector<int> >().swap(function_times);
    pthread_rwlock_unlock(&data_lock);
    tpcc_log.close();
}

void TraceTool::write_log() {
//    log_file << "Write log on instance " << instance << ", id is " << id << endl;
    write_latency("/home/jiamin/nginx/vprofiler/latency/");
}
