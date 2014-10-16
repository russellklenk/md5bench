/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the entry point of the benchmark application.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////
//   Includes   //
////////////////*/
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "common.hpp"
#include "timer.hpp"
#include "md5.hpp"
#include "io.hpp"

/*///////////////////
//   Local Types   //
///////////////////*/
/// @summary Define the function signature for all of our MD5 implementations.
typedef void (LLCALL_C *md5_mem_fn)(void *, void const *, size_t);

/// @summary Define the arguments used to configure a test run of the I/O manager.
struct jobmgr_config_t
{
    char const *Path;           /// The path of the directory to operate on.
    FILE       *Output;         /// The file stream to which output will be written.
    size_t      MaxConcurrent;  /// The maximum number of files to stream concurrently.
    size_t      ReadBufferSize; /// The size of the read buffer, in bytes, or IO_ONE_PAGE.
    bool        UnbufferedIO;   /// true to use unbuffered I/O.
    bool        AsynchronousIO; /// true to use asynchronous I/O calls.
};

/// @summary Define the state associated with a checksum calculation job. This
/// state is identical between the sequential and multi-threaded tasks. 
/// Calculating a checksum is a low-CPU intensity task.
struct chksum_job_t
{
    uint32_t    Id;             /// The job ID (and the job's array index.)
    uint32_t    Checksum;       /// The current checksum state.
};

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Converts a binary representation of an MD5 hash value to a 
/// printable string representation. Note that this function is not thread-safe
/// nor is it re-entrant.
/// @param digest Pointer to a 16-byte buffer containing the message digest.
/// @return A pointer to a 32 byte static buffer containing the printable MD5.
/// This buffer is guaranteed to be terminated with a zero byte.
static char* format_md5(void const *digest)
{   /// @note: not thread-safe or re-entrant.
    static char buffer[33];
    uint8_t const   *md5 = (uint8_t const*) digest;
    for (size_t i = 0, o = 0; i < 16; ++i, o += 2)
    {
        sprintf(&buffer[o], "%02x", md5[i]);
    }
    buffer[32] = '\0';
    return buffer;
}

/// @summary Attempts to load the entire contents of a file into memory. The 
/// returned buffer is allocated using malloc(), and should be freed either 
/// with free() or by calling free_file().
/// @param path The path of the file to load.
/// @param out_size On return, this location is updated with the number of 
/// bytes allocated in the returned buffer (the size of the file.)
/// @return A pointer to the file contents, or NULL.
static void* load_file(char const *path, size_t *out_size)
{
    FILE *fp  = fopen(path, "rb");
    if   (fp == NULL)
    {
        if (out_size) *out_size = 0;
        return NULL;
    }
    size_t nb = 0;
    fseek(fp, 0, SEEK_END);
    nb =  size_t(ftell(fp));
    void *mem = malloc(nb);
    if   (mem == NULL)
    {
        fclose(fp);
        if (out_size) *out_size = nb;
        return NULL;
    }
    rewind(fp);
    fread(mem, 1, nb, fp);
    fclose(fp);
    if (out_size) *out_size = nb;
    return mem;
}

/// @summary Alias function to free the memory allocated for a file using load_file(...).
/// @param file The pointer returned by the load_file(...) function.
static void free_file(void *file)
{
    if (file) free(file);
}

/// @summary Prints a formatted report with timing information for a single test.
/// @param fp the stream to write to (typically stdout or stderr).
/// @param name The name of the implementation being tested.
/// @param md5 The 16 byte MD5 calculated by the test.
/// @param a The starting time of the test, in nanoseconds.
/// @param b The finishing time of the test, in nanoseconds.
/// @param src_size The number of bytes that were hashed.
static void print_report(FILE *fp, char const *name, void const *md5, uint64_t a, uint64_t b, size_t src_size)
{
    uint64_t d = duration(a, b);
    double sec = seconds(d);
    double mbs = src_size / (sec * 1024 * 1024);
    size_t nn  = strlen(name);
    size_t nc  = 16;
    fprintf(fp , "%s:", name);
    while  (nn < nc)
    {   // name padding.
        fprintf(fp, " ");
        ++nn;
    }
    fprintf(fp , "%s    ", format_md5(md5));
    fprintf(fp , "%0.08f sec.    ", seconds(d));
    fprintf(fp , "%20" PRIu64 " ns.    ",   d);
    fprintf(fp , "%.02f MB/sec.", mbs);
    fprintf(fp , "\n");
}

/// @summary Executes and times an MD5 implementation against fully-specified 
/// input data, and prints a formatted report with the results.
/// @param fp The stream to write to (typically stdout or stderr).
/// @param name The name of the implementation being tested.
/// @param md5_func The MD5 driver to test.
/// @param src The input data buffer.
/// @param src_size The number of bytes to read from the input data buffer.
static void time_md5_mem(FILE *fp, char const *name, md5_mem_fn md5_func, void const *src, size_t src_size)
{
    uint8_t  md5[16];
    uint64_t a = time_service_read();
    md5_func(md5, src, src_size);
    uint64_t b = time_service_read();
    print_report(fp, name, md5, a, b, src_size);
}

/// @summary Prints the application banner.
/// @param fp The stream to write to (typically stdout or stderr).
static void print_banner(FILE *fp)
{
    fprintf(fp, "md5bench: Benchmark various MD5 and I/O combinations.\n");
    fprintf(fp, "\n");
}

/// @summary Prints application usage instructions.
/// @param fp The stream to write to (typically stdout or stderr).
static void print_usage(FILE *fp)
{
    fprintf(fp, "USAGE: md5bench [infile]\n");
    fprintf(fp, "infile: An optional path to a file to use as input.\n");
    fprintf(fp, "If no input file is specified, memory performance is tested.\n");
    fprintf(fp, "\n");
}

/// @summary Runs the MD5 tests against a user-specified file.
/// @param fp The stream to write the report to (typically stdout or stderr).
/// @param path The path of the file to load.
/// @return Either EXIT_SUCCESS or EXIT_FAILURE.
static int test_md5_file(FILE *fp, char const *path)
{
    size_t len  = 0;
    void *data  = load_file(path, &len);
    if   (data == NULL)
    {
        printf("ERROR: Unable to load input file %s.", path);
        return EXIT_FAILURE;
    }
    printf("Testing against %s (%u bytes).\n", path, unsigned(len));
    time_md5_mem(fp, "md5_ref", md5_ref, data, len);
    time_md5_mem(fp, "md5_ref2",md5_ref2,data, len);
    free_file(data);
    printf("\n");
    return EXIT_SUCCESS;
}

/// @summary Implements the entry point of the job completion manager thread. 
/// The job completion manager monitors the completion queue, looking for 
/// any completed jobs, and exits when all jobs have finished.
/// @param doneq The job completion queue to monitor.
/// @param job_list The set of per-job state.
/// @param job_count The total number of jobs expected to complete.
/// @param exit_signal The value to set to non-zero when all jobs have completed.
static void jcm_main(io_rddoneq_t *doneq, chksum_job_t *job_list, size_t job_count, uint32_t volatile *exit_signal, uint64_t &total_bytes)
{
    size_t fin_count = 0;
    while (fin_count < job_count)
    {
        if (io_completionq_wait_not_empty(doneq, IO_WAIT_FOREVER))
        {
            io_rdq_result_t result;
            if (io_completionq_get(doneq, result))
            {
                total_bytes += result.BytesJob;
                fin_count++;
            }
        }
    }
    *exit_signal = 1; // tell everyone else that we're done.
}

/// @summary Implements the entry point for the data processing coordinator 
/// thread, which retrieves data buffers from the successful read operation 
/// queue and processes them (or dispatches them to a thread pool for 
/// processing.) The thread exits when signaled by the job manager thread.
/// @param opq The queue to monitor for successful read operations.
/// @param job_list The set of per-job state.
/// @param job_count The number of jobs in job_list.
/// @param exit_signal When set to non-zero, the DPC thread will terminate.
static void dpc_main(io_rdopq_t *opq, chksum_job_t *job_list, size_t job_count, uint32_t volatile *exit_signal)
{
    uint32_t const wait = 16;
    while (*exit_signal == 0)
    {   //  wait a maximum of 16 milliseconds for an operation to be posted.
        //  this is necessary because we're using a flag to control termination.
        if (io_dataq_wait_not_empty(opq, wait))
        {
            io_rdop_t rop;
            if (io_dataq_get(opq, rop))
            {
                chksum_job_t  *job = &job_list[rop.Id];
                uint8_t const *buf = (uint8_t const *)(rop.DataBuffer);
                size_t  const  amt =  rop.DataAmount;
                uint8_t const *end = (uint8_t const *)(buf + amt);
                uint32_t       sum =  0;
                while  (buf != end)
                {
                    sum += *buf++;
                }
                job->Checksum += sum;
                io_return_buffer(rop.ReturnQueue, rop.DataBuffer);
            }
        }
    }
}

/// @summary Implements the entry point for the I/O processor thread, which 
/// continuously polls the concurrent read queue to drive I/O operations.
/// @param rdq The concurrent read queue to poll.
/// @param exit_signal When set to non-zero, the IOP thread will terminate.
static void iop_main(io_rdq_t *rdq, uint32_t volatile *exit_signal)
{
    uint32_t const wait = 16;
    while (*exit_signal == 0)
    {
        if (io_rdq_wait_io(rdq, wait))
        {
            io_rdq_poll(rdq);
        }
    }
}

/// @summary Implements the entry point of the job manager thread. The job 
/// manager is responsible for creating all of the queues, submitting jobs to
/// the pending queue, and spawning the job completion monitor.
/// @param config Configuration parameters for the job manager.
static void job_main(jobmgr_config_t const &config)
{
    // enumerate files in the specified directory:
    io_file_list_t files;
    io_create_file_list(&files, 0, 0);
    io_enumerate_files (&files, config.Path, "*.*", true);

    // create and initialize all of the necessary queues.
    // choosing the correct size for these queues is important.
    // the completion queue needs to be large enough to hold all jobs 
    // in case they all complete at once, which could happen due if 
    // io_rdq_cancel_all() is called. the rdopq needs to be large 
    // enough to accomodate the maximum number of buffers in flight 
    // at any given time (which is workload specific); otherwise, the
    // system will stall waiting to publish buffers to the DPC thread.
    size_t        npend = (files.PathCount + 1) / config.MaxConcurrent;
    io_rdpendq_t *pendq = io_create_jobq(npend);
    io_rdstopq_t *stopq = io_create_cancelq(10);
    io_rddoneq_t *doneq = io_create_completionq(files.PathCount + 1);
    io_rdopq_t   *rdopq = io_create_dataq(config.MaxConcurrent  * 2);

    // now configure and create the concurrent read queue, which performs 
    // the actual I/O operations and dispatches results to the DPC and JCM.
    io_rdq_config_t cfg;
    cfg.DataQueue       = rdopq;
    cfg.PendingQueue    = pendq;
    cfg.CancelQueue     = stopq;
    cfg.CompleteQueue   = doneq;
    cfg.MaxConcurrent   = config.MaxConcurrent;
    cfg.MaxBufferSize   = 4 * 1024 * 1024;       // 4MB
    cfg.MaxReadSize     = config.ReadBufferSize;
    cfg.Unbuffered      = config.UnbufferedIO;
    cfg.Asynchronous    = config.AsynchronousIO;
    io_rdq_t       *rdq = io_create_rdq(cfg);
    // the buffer size, etc. that we actually got is now specified in cfg.
    
    // initialize the per-job state.
    size_t const  njobs = files.PathCount;
    chksum_job_t  *jobs = (chksum_job_t*) malloc(njobs * sizeof(chksum_job_t));
    for (size_t i = 0;  i < njobs; ++i)
    {
        jobs[i].Id        = uint32_t(i);
        jobs[i].Checksum  = 0;
    }

    // initialize our worker threads.
    uint64_t          total_nb     = 0;
    uint32_t volatile exit_signal  = 0;
    std::thread jcm(jcm_main, doneq, jobs, njobs, &exit_signal, std::ref(total_nb)); // sets exit_signal
    std::thread dpc(dpc_main, rdopq, jobs, njobs, &exit_signal);           // read exit_signal
    std::thread iop(iop_main,   rdq, &exit_signal);                        // read exit_signal

    uint64_t beg_time = time_service_read();

    // issue jobs until we run out of work.
    for (size_t i = 0; i < njobs; /* empty */)
    {
        if (io_jobq_wait_not_full(pendq, IO_WAIT_FOREVER))
        {
            io_rdq_job_t job;
            job.JobId       = uint32_t(i);
            job.Path        = io_file_list_path(&files, i);
            job.EnqueueTime = 0;   // set by io_rdq_submit()
            job.RangeCount  = 0;   // read the entire file
            if (io_rdq_submit(rdq, job) == false)
            {   // the pending jobs queue is full.
                continue;
            }
            else ++i;
        }
    }
    
    // wait until the job completion manager exits, and clean up.
    jcm.join();
    dpc.join();
    iop.join();

    uint64_t end_time = time_service_read();
    uint64_t d = duration(beg_time, end_time);
    double   s = seconds(d);

    fprintf(config.Output, "test_io_thd: Max Active: %u, Max Read: %u, Unbuffered: %u, AIO: %u.\n", unsigned(cfg.MaxConcurrent), unsigned(cfg.MaxReadSize), unsigned(config.UnbufferedIO ? 1 : 0), unsigned(config.AsynchronousIO ? 1 : 0));
    fprintf(config.Output, "  Read %8" PRIu64 " bytes in %4.3f seconds (%4.3fMB/sec) (%8.3f bytes/sec).\n", total_nb, s, (total_nb/(1024*1024)) / s, total_nb / s);
    fprintf(config.Output, "\n");

    free(jobs);
    io_delete_rdq(rdq);
    io_delete_completionq(doneq);
    io_delete_cancelq(stopq);
    io_delete_dataq(rdopq);
    io_delete_jobq(pendq);
    io_delete_file_list(&files);
}

/// @summary Implements the entry point of the sequential job manager. The job 
/// manager is responsible for creating all of the queues, submitting jobs to
/// the pending queue, polling I/O status and scheduling new I/O operations, 
/// and polling the completion queue for job results.
/// @param config Configuration parameters for the job manager.
static void seq_main(jobmgr_config_t const &config)
{
    io_file_list_t files;
    io_create_file_list(&files, 0, 0);
    io_enumerate_files (&files, config.Path, "*.*", true);

    // create and initialize a concurrent read queue and all supporting queues.
    io_rdpendq_t *jobq    = io_create_jobq(files.PathCount);
    io_rdopq_t   *dataq   = io_create_dataq(512);
    io_rdstopq_t *cancelq = io_create_cancelq(10);
    io_rddoneq_t *finishq = io_create_completionq(files.PathCount);

    io_rdq_config_t qconf;
    qconf.DataQueue       = dataq;
    qconf.PendingQueue    = jobq;
    qconf.CancelQueue     = cancelq;
    qconf.CompleteQueue   = finishq;
    qconf.MaxConcurrent   = config.MaxConcurrent;
    qconf.MaxBufferSize   = 4 * 1024 * 1024; // 4MB
    qconf.MaxReadSize     = config.ReadBufferSize;
    qconf.Unbuffered      = config.UnbufferedIO;
    qconf.Asynchronous    = config.AsynchronousIO;
    io_rdq_t *rdq = io_create_rdq(qconf);

    size_t pend_count = files.PathCount;
    size_t jobs_count = files.PathCount;
    size_t done_count = 0;
    uint32_t max_wait = 16; // in milliseconds

    chksum_job_t  *jobs = (chksum_job_t*) malloc(jobs_count * sizeof(chksum_job_t));
    uint64_t   total_nb = 0;
    uint64_t   beg_time = time_service_read();

    while (done_count < jobs_count)
    {
        // block here until we can submit a job to be processed.
        // todo: in order to do this single-threaded when we have 
        // more jobs than we have pending queue capacity, we need 
        // to be able to poll, ie. io_jobq_poll_not_full(jobq).

        // fill up as many slots in the job queue as we can.
        while (pend_count > 0)
        {
            io_rdq_job_t job;
            job.JobId       = uint32_t(jobs_count - pend_count);
            job.Path        = io_file_list_path(&files, job.JobId);
            job.EnqueueTime = 0;
            job.RangeCount  = 0;
            if (io_rdq_submit(rdq, job) == false)
            {   // the pending jobs queue is full.
                break;
            }
            // initialize our internal state for the job we just submitted.
            jobs[job.JobId].Id = job.JobId;
            jobs[job.JobId].Checksum = 0;
            pend_count--;
        }

        // block indefinitely until an I/O has completed.
        if (io_rdq_wait_io(rdq, max_wait))
        {
            io_rdq_poll(rdq);
        }

        // process any data returned by the I/O manager.
        io_rdop_t read;
        while (io_dataq_get(dataq, read))
        {
            chksum_job_t &job  = jobs[read.Id];
            uint8_t    *bytes  = (uint8_t*) read.DataBuffer;
            uint32_t    chksum = 0;
            for (size_t i = 0; i < read.DataAmount; ++i)
            {
                chksum += *bytes++;
            }
            job.Checksum += chksum;
            io_return_buffer(read.ReturnQueue, read.DataBuffer);
        }

        // process any jobs completed by the I/O manager.
        io_rdq_result_t result;
        while (io_completionq_get(finishq, result))
        {
            total_nb += result.BytesJob;
            done_count++;
        }
    }

    uint64_t end_time = time_service_read();
    uint64_t d = duration(beg_time, end_time);
    double   s = seconds(d);

    fprintf(config.Output, "test_io_seq: Max Active: %u, Max Read: %u, Unbuffered: %u, AIO: %u.\n", unsigned(qconf.MaxConcurrent), unsigned(qconf.MaxReadSize), unsigned(config.UnbufferedIO ? 1 : 0), unsigned(config.AsynchronousIO ? 1 : 0));
    fprintf(config.Output, "  Read %8" PRIu64 " bytes in %4.3f seconds (%4.3fMB/sec) (%8.3f bytes/sec).\n", total_nb, s, (total_nb/(1024*1024)) / s, total_nb / s);
    fprintf(config.Output, "\n");
    
    free(jobs);
    io_delete_rdq(rdq);
    io_delete_completionq(finishq);
    io_delete_cancelq(cancelq);
    io_delete_dataq(dataq);
    io_delete_jobq(jobq);
    io_delete_file_list(&files);
}

/*////////////////////////
//   Public Functions   //
////////////////////////*/
/// @summary Entry point of the md5bench driver.
/// @param argc The number of command-line arguments.
/// @param argv An array of NULL-terminated strings specifying command-line arguments.
/// @return Either EXIT_SUCCESS or EXIT_FAILURE.
int main(int argc, char **argv)
{
    int exit_code = EXIT_SUCCESS;

    UNUSED_ARG(argc);
    UNUSED_ARG(argv);

    print_banner(stdout);
    print_usage (stdout);
    time_service_open ();

    if (argc > 1)
    {
        exit_code = test_md5_file(stdout, argv[1]);
    }

    /*io_file_list_t files;
    io_create_file_list(&files, 0, 0);
    io_enumerate_files (&files, "D:\\workspace", "*.*", true);
    io_format_file_list(stdout, &files);
    if (io_verify_file_list(&files))
    {
        printf("The file list verifies.\n");
    }
    else
    {
        printf("The file list contains collisions.\n");
    }
    io_delete_file_list(&files);*/

    /*jobmgr_config_t cfg;
    cfg.Path           = "C:\\foo\\bar";
    cfg.Output         = stdout;
    cfg.MaxConcurrent  = 16;
    cfg.ReadBufferSize = 64 * 1024;
    cfg.UnbufferedIO   = false;
    cfg.AsynchronousIO = true;*/

    //test_io_seq(stdout, "C:\\Users\\rklenk\\Projects\\vvv", 1, IO_ONE_PAGE, false, false);
    //test_io_seq(stdout, "C:\\Users\\rklenk\\Projects\\vvv", 1, IO_ONE_PAGE, false, true);
    //test_io_seq(stdout, "C:\\Users\\rklenk\\Projects\\vvv", 1, IO_ONE_PAGE, true , true);
    //test_io_seq(stdout, "C:\\Users\\rklenk\\Projects\\vvv", 1, IO_ONE_PAGE, true , false);
    //test_io_seq(stdout, "C:\\WinDDK", 8, 64 * 1024, true, true);
    //seq_main(cfg);
    //std::thread jmt(job_main, std::ref(cfg));
    //jmt.join();

    time_service_close();
    exit(exit_code);
}

