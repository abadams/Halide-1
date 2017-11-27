#include "printer.h"

extern "C" int pthread_self();

namespace Halide { namespace Runtime { namespace Internal {

struct work {
    halide_parallel_task_t task;

    // If we come in to the task system via do_par_for we just have a
    // halide_task_t, not a halide_loop_task_t.
    halide_task_t task_fn;

    work *next_job;
    int *parent;
    void *user_context;
    int active_workers;
    int exit_status;
    int next_semaphore;
    // which condition variable is the owner sleeping on. NULL if it isn't sleeping.
    bool owner_is_sleeping;

    bool make_runnable() {
        for (; next_semaphore < task.num_semaphores; next_semaphore++) {
            if (!halide_default_semaphore_try_acquire(task.semaphores[next_semaphore].semaphore,
                                                      task.semaphores[next_semaphore].count)) {
                // Note that we don't release the semaphores already
                // acquired. We never have two consumers contending
                // over the same semaphore, so it's not helpful to do
                // so.
                return false;
            }
        }
        // Future iterations of this task need to acquire the semaphores from scratch.
        next_semaphore = 0;
        return true;
    }

    bool running() {
        return task.extent || active_workers;
    }
};

#define MAX_THREADS 256

WEAK int clamp_num_threads(int threads) {
    if (threads > MAX_THREADS) {
        threads = MAX_THREADS;
    } else if (threads < 1) {
        threads = 1;
    }
    return threads;
}

WEAK int default_desired_num_threads() {
    int desired_num_threads = 0;
    char *threads_str = getenv("HL_NUM_THREADS");
    if (!threads_str) {
        // Legacy name for HL_NUM_THREADS
        threads_str = getenv("HL_NUMTHREADS");
    }
    if (threads_str) {
        desired_num_threads = atoi(threads_str);
    } else {
        desired_num_threads = halide_host_cpu_count();
    }
    return desired_num_threads;
}

// The work queue and thread pool is weak, so one big work queue is shared by all halide functions
struct work_queue_t {
    // all fields are protected by this mutex.
    halide_mutex mutex;

    // Singly linked list for job stack
    work *jobs;

    // The number threads created
    int threads_created;

    // The desired number threads doing work (HL_NUM_THREADS).
    int desired_threads_working;

    // Workers sleep on one of two condition variables, to make it
    // easier to wake up the right number if a small number of tasks
    // are enqueued. There are A-team workers and B-team workers. The
    // following variables track the current size and the desired size
    // of the A team.
    int a_team_size, target_a_team_size;

    // The condition variables that workers and owners sleep on. We
    // may want to wake them up independently. Any code that may
    // invalidate any of the reasons a worker or owner may have slept
    // must signal or broadcast the appropriate condition variable.
    halide_cond wake_a_team, wake_b_team, wake_owners;

    // The number of sleeping workers and owners. An over-estimate - a
    // waking-up thread may not have decremented this yet.
    int workers_sleeping, owners_sleeping;

    // Keep track of threads so they can be joined at shutdown
    halide_thread *threads[MAX_THREADS];

    // Global flags indicating the threadpool should shut down, and
    // whether the thread pool has been initialized.
    bool shutdown, initialized;
};
WEAK work_queue_t work_queue;

__attribute__((constructor))
WEAK void initialize_work_queue() {
    halide_mutex_init(&work_queue.mutex);
}

WEAK void worker_thread(void *);

WEAK void worker_thread_already_locked(work *owned_job) {
    while (owned_job ? owned_job->running() : !work_queue.shutdown) {

        // Find a job to run, prefering things near the top of the stack.
        work *job = work_queue.jobs;
        work **prev_ptr = &work_queue.jobs;
        while (job) {
            // Only schedule tasks with enough free worker threads
            // around to complete. They may get stolen later, but only
            // by tasks which can themselves use them to complete
            // work, so forward progress is made.
            int threads_that_could_assist = 1 + work_queue.workers_sleeping;
            if (!job->task.may_block) {
                threads_that_could_assist += work_queue.owners_sleeping;
            } else if (job->owner_is_sleeping) {
                threads_that_could_assist++;
            }
            bool enough_threads = job->task.min_threads <= threads_that_could_assist;
            bool may_try = ((!owned_job || job->parent == owned_job->parent || !job->task.may_block) &&
                            (!job->task.serial || (job->active_workers == 0)));
            if (may_try && enough_threads && job->make_runnable()) {
                break;
            }
            //print(NULL) << pthread_self() << " Passing on " << job->task.name << " " << enough_threads << " " << may_try << "\n";
            prev_ptr = &(job->next_job);
            job = job->next_job;
        }

        if (!job) {
            // There is no runnable job. Go to sleep.
            if (owned_job) {
                work_queue.owners_sleeping++;
                owned_job->owner_is_sleeping = true;
                //print(NULL) << pthread_self() << " Owner sleeping\n";
                halide_cond_wait(&work_queue.wake_owners, &work_queue.mutex);
                owned_job->owner_is_sleeping = false;
                work_queue.owners_sleeping--;
            } else {
                work_queue.workers_sleeping++;
                if (work_queue.a_team_size > work_queue.target_a_team_size) {
                    // Transition to B team
                    //print(NULL) << pthread_self() << " B team worker sleeping\n";
                    work_queue.a_team_size--;
                    halide_cond_wait(&work_queue.wake_b_team, &work_queue.mutex);
                    work_queue.a_team_size++;
                } else {
                    //print(NULL) << pthread_self() << " A team worker sleeping\n";
                    halide_cond_wait(&work_queue.wake_a_team, &work_queue.mutex);
                }
                work_queue.workers_sleeping--;
            }
            continue;
        }

        //print(NULL) << pthread_self() << " Working on " << job->task.name << "\n";

        // Increment the active_worker count so that other threads
        // are aware that this job is still in progress even
        // though there are no outstanding tasks for it.
        job->active_workers++;

        int result = 0;

        if (job->task.serial) {
            // Remove it from the stack while we work on it
            *prev_ptr = job->next_job;

            // Release the lock and do the task.
            halide_mutex_unlock(&work_queue.mutex);
            int iters = 1;
            while (result == 0) {
                // Claim as many iterations as possible
                while (job->task.extent > iters && job->make_runnable()) {
                    iters++;
                }
                if (iters == 0) break;

                // Do them
                result = halide_do_loop_task(job->user_context, job->task.fn,
                                             job->task.min, iters, job->task.closure);
                job->task.min += iters;
                job->task.extent -= iters;
                iters = 0;
            }
            halide_mutex_lock(&work_queue.mutex);

            // Put it back on the job stack
            if (job->task.extent > 0) {
                job->next_job = work_queue.jobs;
                work_queue.jobs = job;
            }

        } else {
            // Claim a task from it.
            work myjob = *job;
            job->task.min++;
            job->task.extent--;

            // If there were no more tasks pending for this job, remove it
            // from the stack.
            if (job->task.extent == 0) {
                *prev_ptr = job->next_job;
            }

            // Release the lock and do the task.
            halide_mutex_unlock(&work_queue.mutex);
            if (myjob.task_fn) {
                result = halide_do_task(myjob.user_context, myjob.task_fn,
                                        myjob.task.min, myjob.task.closure);
            } else {
                result = halide_do_loop_task(myjob.user_context, myjob.task.fn,
                                             myjob.task.min, 1, myjob.task.closure);
            }
            halide_mutex_lock(&work_queue.mutex);
        }

        // If this task failed, set the exit status on the job.
        if (result) {
            job->exit_status = result;
        }

        // We are no longer active on this job
        job->active_workers--;

        //print(NULL) << pthread_self() << " Completed part of " << job->task.name << "\n";

        if (!job->running() && job->owner_is_sleeping) {
            // The job is done. Wake up the owner.
            halide_cond_broadcast(&work_queue.wake_owners);
        }
    }
}

WEAK void worker_thread(void *arg) {
    halide_mutex_lock(&work_queue.mutex);
    worker_thread_already_locked((work *)arg);
    halide_mutex_unlock(&work_queue.mutex);
}

WEAK void enqueue_work_already_locked(int num_jobs, work *jobs) {
    if (!work_queue.initialized) {
        work_queue.shutdown = false;
        halide_cond_init(&work_queue.wake_a_team);
        halide_cond_init(&work_queue.wake_b_team);
        halide_cond_init(&work_queue.wake_owners);
        work_queue.jobs = NULL;

        // Compute the desired number of threads to use. Other code
        // can also mess with this value, but only when the work queue
        // is locked.
        if (!work_queue.desired_threads_working) {
            work_queue.desired_threads_working = default_desired_num_threads();
        }
        work_queue.desired_threads_working = clamp_num_threads(work_queue.desired_threads_working);
        work_queue.a_team_size = 0;
        work_queue.target_a_team_size = 0;
        work_queue.threads_created = 0;
        work_queue.workers_sleeping = 0;
        work_queue.owners_sleeping = 0;
        work_queue.initialized = true;
    }

    // Gather some information about the work.

    // Some tasks require a minimum number of threads to make forward
    // progress. Also assume the blocking tasks need to run concurrently.
    int min_threads = 0;

    // Count how many workers to wake. Start at -1 because this thread
    // will contribute.
    int workers_to_wake = -1;

    // Could stalled owners of other tasks conceivably help with one
    // of these jobs.
    bool stealable_jobs = false;

    for (int i = 0; i < num_jobs; i++) {
        if (!jobs[i].task.may_block) {
            stealable_jobs = true;
        } else {
            min_threads += jobs[i].task.min_threads;
        }
        if (jobs[i].task.serial) {
            workers_to_wake++;
        } else {
            workers_to_wake += jobs[i].task.extent;
        }
        //print(NULL) << pthread_self() << " Enqueueing " << jobs[i].task.name << "\n";
    }

    // Spawn more threads if necessary.
    while ((work_queue.threads_created < work_queue.desired_threads_working - 1) ||
           (work_queue.threads_created < min_threads - 1)) {
        // We might need to make some new threads, if work_queue.desired_threads_working has
        // increased, or if there aren't enough threads to complete this new task.
        work_queue.a_team_size++;
        work_queue.threads[work_queue.threads_created++] =
            halide_spawn_thread(worker_thread, NULL);
    }

    //print(NULL) << pthread_self() << " Workers to wake: " << workers_to_wake << "\n";
    //print(NULL) << pthread_self() << " min_threads for this task: " << min_threads << "\n";
    //print(NULL) << pthread_self() << " Threads created: " << work_queue.threads_created << "\n";

    // Store a token on the stack so that we know which jobs we
    // own. We may work on any job we own, regardless of whether it
    // blocks. The value is unimportant - we only use the address.
    int parent_id = 0;

    // Push the jobs onto the stack.
    for (int i = num_jobs - 1; i >= 0; i--) {
        // We could bubble it downwards based on some heuristics, but
        // it's not strictly necessary to do so.
        jobs[i].next_job = work_queue.jobs;
        jobs[i].parent = &parent_id;
        work_queue.jobs = jobs + i;
    }

    bool nested_parallelism =
        work_queue.owners_sleeping ||
        (work_queue.workers_sleeping < work_queue.threads_created);

    //print(NULL) << pthread_self() << " nested parallelism: " << nested_parallelism << "\n";

    // Wake up an appropriate number of threads
    if (nested_parallelism || workers_to_wake > work_queue.workers_sleeping) {
        // If there's nested parallelism going on, we just wake up
        // everyone. TODO: make this more precise.
        work_queue.target_a_team_size = work_queue.threads_created;
    } else {
        work_queue.target_a_team_size = workers_to_wake;
    }
    //print(NULL) << pthread_self() << " A team size: " << work_queue.a_team_size << "\n";
    //print(NULL) << pthread_self() << " Target A team size: " << work_queue.target_a_team_size << "\n";
    halide_cond_broadcast(&work_queue.wake_a_team);
    if (work_queue.target_a_team_size > work_queue.a_team_size) {
        halide_cond_broadcast(&work_queue.wake_b_team);
        if (stealable_jobs) {
            halide_cond_broadcast(&work_queue.wake_owners);
        }
    }
}

}}}  // namespace Halide::Runtime::Internal

using namespace Halide::Runtime::Internal;

extern "C" {

WEAK int halide_default_do_task(void *user_context, halide_task_t f, int idx,
                                uint8_t *closure) {
    return f(user_context, idx, closure);
}

WEAK int halide_default_do_loop_task(void *user_context, halide_loop_task_t f,
                                     int min, int extent, uint8_t *closure) {
    return f(user_context, min, extent, closure);
}

WEAK int halide_default_do_par_for(void *user_context, halide_task_t f,
                                   int min, int size, uint8_t *closure) {
    if (size <= 0) {
        return 0;
    }

    work job;
    job.task.fn = NULL;
    job.task.min = min;
    job.task.extent = size;
    job.task.may_block = false;
    job.task.serial = false;
    job.task.semaphores = NULL;
    job.task.num_semaphores = 0;
    job.task.closure = closure;
    job.task.min_threads = 1;
    job.task.name = NULL;
    job.task_fn = f;
    job.user_context = user_context;
    job.exit_status = 0;
    job.active_workers = 0;
    job.next_semaphore = 0;
    job.owner_is_sleeping = false;
    halide_mutex_lock(&work_queue.mutex);
    enqueue_work_already_locked(1, &job);
    worker_thread_already_locked(&job);
    halide_mutex_unlock(&work_queue.mutex);
    return job.exit_status;
}

WEAK int halide_default_do_parallel_tasks(void *user_context, int num_tasks,
                                          struct halide_parallel_task_t *tasks) {
    work *jobs = (work *)__builtin_alloca(sizeof(work) * num_tasks);

    for (int i = 0; i < num_tasks; i++) {
        if (tasks->extent <= 0) {
            // Skip extent zero jobs
            num_tasks--;
            continue;
        }
        jobs[i].task = *tasks++;
        jobs[i].task_fn = NULL;
        jobs[i].user_context = user_context;
        jobs[i].exit_status = 0;
        jobs[i].active_workers = 0;
        jobs[i].next_semaphore = 0;
        jobs[i].owner_is_sleeping = false;
    }

    if (num_tasks == 0) {
        return 0;
    }

    halide_mutex_lock(&work_queue.mutex);
    enqueue_work_already_locked(num_tasks, jobs);
    int exit_status = 0;
    for (int i = 0; i < num_tasks; i++) {
        //print(NULL) << pthread_self() << " Joining task " << jobs[i].task.name << "\n";
        // It doesn't matter what order we join the tasks in, because
        // we'll happily assist with siblings too.
        worker_thread_already_locked(jobs + i);
        if (jobs[i].exit_status != 0) {
            exit_status = jobs[i].exit_status;
        }
    }
    halide_mutex_unlock(&work_queue.mutex);
    return exit_status;
}

WEAK int halide_set_num_threads(int n) {
    if (n < 0) {
        halide_error(NULL, "halide_set_num_threads: must be >= 0.");
    }
    // Don't make this an atomic swap - we don't want to be changing
    // the desired number of threads while another thread is in the
    // middle of a sequence of non-atomic operations.
    halide_mutex_lock(&work_queue.mutex);
    if (n == 0) {
        n = default_desired_num_threads();
    }
    int old = work_queue.desired_threads_working;
    work_queue.desired_threads_working = clamp_num_threads(n);
    halide_mutex_unlock(&work_queue.mutex);
    return old;
}

WEAK void halide_shutdown_thread_pool() {
    if (work_queue.initialized) {
        // Wake everyone up and tell them the party's over and it's time
        // to go home
        halide_mutex_lock(&work_queue.mutex);
        work_queue.shutdown = true;

        halide_cond_broadcast(&work_queue.wake_a_team);
        halide_cond_broadcast(&work_queue.wake_b_team);
        halide_cond_broadcast(&work_queue.wake_owners);
        halide_mutex_unlock(&work_queue.mutex);

        // Wait until they leave
        for (int i = 0; i < work_queue.threads_created; i++) {
            halide_join_thread(work_queue.threads[i]);
        }

        // Tidy up
        halide_mutex_destroy(&work_queue.mutex);
        halide_cond_destroy(&work_queue.wake_a_team);
        halide_cond_destroy(&work_queue.wake_b_team);
        halide_cond_destroy(&work_queue.wake_owners);
        work_queue.initialized = false;
    }
}

struct halide_semaphore_impl_t {
    int value;
};

WEAK int halide_default_semaphore_init(halide_semaphore_t *s, int n) {
    //print(NULL) << "Init " << s << " " << n << "\n";
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    sem->value = n;
    return n;
}

WEAK int halide_default_semaphore_release(halide_semaphore_t *s, int n) {
    //print(NULL) << "Release " << s << " " << n << "\n";
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    int new_val = __sync_add_and_fetch(&(sem->value), n);
    if (new_val == n) {
        // We may have just made a job runnable
        halide_cond_broadcast(&work_queue.wake_a_team);
        halide_cond_broadcast(&work_queue.wake_owners);
    }
    return new_val;
}

WEAK bool halide_default_semaphore_try_acquire(halide_semaphore_t *s, int n) {
    //print(NULL) << "  Try acquire " << s << " " << n << "\n";
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    // Decrement and get new value
    int new_val = __sync_add_and_fetch(&(sem->value), -n);
    if (new_val < 0) {
        // Oops, increment and return failure
        __sync_add_and_fetch(&(sem->value), n);
        return false;
    }
    //print(NULL) << "Acquire " << s << " " << n << "\n";
    return true;
}

}
