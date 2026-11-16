// =============================================================================
//  stop_demo.cpp  -  Cooperative cancellation in C++20 (revision notes as code)
//
//  Build : g++ -std=c++20 -pthread -Wall -Wextra stop_demo.cpp -o stop_demo
//  Run   : ./stop_demo
//
//  THE BIG PICTURE
//  ---------------
//  One hidden, shared "stop state" (an atomic flag + a list of callbacks).
//  Three public handle types point to it:
//
//     std::stop_source    = the CANCEL BUTTON       (can request a stop)
//     std::stop_token     = the INDICATOR LIGHT     (can only look)
//     std::stop_callback  = a NOTE "when stopped, run this function"
//
//  Stopping is COOPERATIVE: request_stop() only sets the flag / fires callbacks.
//  Nothing is killed. The worker must check the flag and finish on its own.
//
//  MEMBER CHECKLIST (where each one is demonstrated)
//  -------------------------------------------------
//  std::stop_source
//    default ctor ........................ section 1
//    stop_source(std::nostopstate) ....... section 2
//    copy / move ctor + move assign ...... section 3
//    get_token() ......................... sections 1, 5, 7
//    request_stop()  (returns bool) ...... sections 1, 4
//    stop_requested() / stop_possible() .. sections 1, 2
//    swap() / operator== ................. section 3
//  std::stop_token
//    default ctor (empty token) .......... section 2
//    stop_requested() / stop_possible() .. sections 1, 2, 7
//    copy / move / swap / operator== ..... section 3
//  std::stop_callback
//    ctor with lvalue token .............. section 5a
//    ctor with rvalue token .............. section 5a
//    runs on the thread calling request_stop() ... section 5b
//    runs immediately if already stopped . section 5c
//    destructor = deregister ............. section 5d
//  Bonus
//    condition_variable_any + stop_token . sections 6, 7
//    std::jthread integration ............ section 8
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Small helpers so that output from several threads does not get interleaved
// ---------------------------------------------------------------------------
std::mutex g_log_mutex;

template <class... Args>
void log(const Args&... args) {            // const lvalue references to every argument
    std::lock_guard lk(g_log_mutex);
    (std::cout << ... << args) << '\n';    // C++17 fold expression
}

const char* yn(bool b) { return b ? "yes" : "no"; }

void banner(const std::string& title) {
    log("\n==================================================");
    log(title);
    log("==================================================");
}

// =============================================================================
// 1. BASICS: source -> token, request_stop, the two query functions
// =============================================================================
void section1_basics() {
    banner("1. Basics: stop_source -> stop_token");

    std::stop_source src;                    // engaged: creates a NEW shared stop state
    std::stop_token  tok = src.get_token();  // get_token() returns a std::stop_token by value
    // (a prvalue / rvalue). It points to the SAME state.

    log("before request_stop():");
    log("  src.stop_possible()  = ", yn(src.stop_possible()));   // yes: a source exists
    log("  src.stop_requested() = ", yn(src.stop_requested()));  // no
    log("  tok.stop_possible()  = ", yn(tok.stop_possible()));   // yes
    log("  tok.stop_requested() = ", yn(tok.stop_requested()));  // no

    src.request_stop();                      // set the flag in the shared state

    log("after request_stop():");
    log("  src.stop_requested() = ", yn(src.stop_requested()));  // yes
    log("  tok.stop_requested() = ", yn(tok.stop_requested()));  // yes: token sees the SAME flag

    // NOTE: a stop_token has NO request_stop(). Workers can look, not press the button.
}

// =============================================================================
// 2. stop_possible(): "can a stop EVER happen?"  + empty / disengaged objects
// =============================================================================
void section2_stop_possible() {
    banner("2. stop_possible(), empty tokens, std::nostopstate");

    // (a) default-constructed token: refers to NO stop state at all
    std::stop_token empty_tok;
    log("default-constructed token: stop_possible = ", yn(empty_tok.stop_possible()),
        ", stop_requested = ", yn(empty_tok.stop_requested()));

    // (b) token outlives its source, and nobody ever requested a stop
    std::stop_token tok;
    {
        std::stop_source temp;
        tok = temp.get_token();              // rvalue token -> move assignment
        log("while a source exists        : stop_possible = ", yn(tok.stop_possible()));
    }                                        // last stop_source destroyed here
    log("source gone, no stop requested: stop_possible = ", yn(tok.stop_possible()),
        "   <- nobody can ever request a stop now");

    // (c) token outlives its source, but a stop WAS requested before
    {
        std::stop_source temp2;
        tok = temp2.get_token();             // rvalue token -> move assignment
        temp2.request_stop();
    }
    log("source gone, stop was requested: stop_possible = ", yn(tok.stop_possible()),
        ", stop_requested = ", yn(tok.stop_requested()));

    // (d) a DISENGAGED source: it has no stop state (stop_state_ pointer is empty)
    std::stop_source none{ std::nostopstate };          // std::nostopstate is a tag value
    log("nostopstate source: stop_possible = ", yn(none.stop_possible()));
    log("nostopstate source: request_stop() returned ", yn(none.request_stop()), " (nothing to stop)");
    std::stop_token none_tok = none.get_token();       // gives an empty token
    log("token from it     : stop_possible = ", yn(none_tok.stop_possible()));
}

// =============================================================================
// 3. COPY / MOVE / SWAP / EQUALITY  (handles to the same state)
// =============================================================================
void section3_copy_move_swap() {
    banner("3. Copy, move, swap and == (handles to the same state)");

    // ---- stop_source ----
    std::stop_source a;                  // new state  #1
    std::stop_source b = a;              // copy constructor (a is an lvalue): b shares state #1
    std::stop_source c;                  // new state  #2

    log("a == b (copy of a)      : ", yn(a == b));   // yes: same state
    log("a == c (independent)    : ", yn(a == c));   // no : different states

    a.swap(c);                           // swap the two handles (not the states themselves)
    log("after a.swap(c): a == b : ", yn(a == b));   // no : a now holds state #2
    log("                 c == b : ", yn(c == b));   // yes: c now holds state #1

    std::stop_source d = std::move(a);   // std::move gives an rvalue -> MOVE constructor
    log("after move: d.stop_possible() = ", yn(d.stop_possible()));  // yes: d took the state
    log("            a.stop_possible() = ", yn(a.stop_possible()));  // no : a is now disengaged

    // ---- stop_token ----
    std::stop_source s1, s2;
    std::stop_token t1 = s1.get_token();     // rvalue token moved into t1
    std::stop_token t2 = t1;                 // copy constructor (t1 is an lvalue)
    std::stop_token t3 = s2.get_token();     // a token of a different source

    log("t1 == t2 (copy)         : ", yn(t1 == t2));   // yes
    log("t1 == t3 (other source) : ", yn(t1 == t3));   // no

    t1.swap(t3);                             // swap handles
    log("after t1.swap(t3): t1 == s2.get_token() : ", yn(t1 == s2.get_token()));  // yes
}

// =============================================================================
// 4. request_stop() RETURNS bool  ->  only the FIRST caller gets true
//    (this is why a stop_source is also a thread-safe "fire once" trigger)
// =============================================================================
void section4_request_stop_once() {
    banner("4. request_stop() returns true only for the first caller");

    std::stop_source s;
    log("1st request_stop() returned: ", yn(s.request_stop()));   // yes
    log("2nd request_stop() returned: ", yn(s.request_stop()));   // no (already stopped)

    // 4 threads race to press the button. Exactly ONE of them wins.
    std::stop_source race;
    std::atomic<int> winners{ 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&race, &winners] {
        if (race.request_stop())          // true for exactly one thread
            ++winners;
            });
    for (auto& t : threads) t.join();
    log("4 racing threads -> number of winners = ", winners.load(), "   (always exactly 1)");
}

// =============================================================================
// 5. stop_callback
//    std::stop_callback cb(token, function);
//    - constructor REGISTERS the function in the shared stop state
//    - request_stop() RUNS it (once)
//    - destructor DEREGISTERS it
//    stop_callback is neither copyable nor movable (its registration is tied to its address).
// =============================================================================
void section5_callbacks() {
    banner("5. stop_callback");

    // ---- 5a. The two constructor overloads -----------------------------------
    {
        log("\n-- 5a. lvalue token vs rvalue token --");
        std::stop_source src;
        std::stop_token  tok = src.get_token();

        // `tok` is a NAMED variable => lvalue => binds to the
        //   stop_callback(const std::stop_token& st, C&& cb)   overload
        // The lambda is an rvalue; `C&&` is a FORWARDING reference, so it is moved into the object.
        std::stop_callback cb_lvalue(tok, [] { log("   cb_lvalue fired"); });

        // `src.get_token()` is a TEMPORARY => rvalue => binds to the
        //   stop_callback(std::stop_token&& st, C&& cb)   overload (may steal the pointer)
        std::stop_callback cb_rvalue(src.get_token(), [] { log("   cb_rvalue fired"); });

        log("calling request_stop() ...");
        src.request_stop();                  // both callbacks run now (their relative order is unspecified)
        log("request_stop() returned");
    }

    // ---- 5b. WHICH THREAD runs the callback? ---------------------------------
    {
        log("\n-- 5b. the callback runs on the thread that calls request_stop() --");
        std::stop_source src;
        std::stop_callback cb(src.get_token(), [] {   // rvalue token overload
            log("   callback running on thread ", std::this_thread::get_id());
            });

        log("   main thread id      = ", std::this_thread::get_id());
        std::thread requester([&src] {
            log("   requester thread id = ", std::this_thread::get_id());
            src.request_stop();              // the callback runs HERE, inside this call
            });
        requester.join();
    }

    // ---- 5c. Stop already requested -> callback runs IMMEDIATELY -------------
    {
        log("\n-- 5c. registering after the stop already happened --");
        std::stop_source src;
        src.request_stop();
        log("   about to construct the callback ...");
        std::stop_callback cb(src.get_token(), [] {
            log("   fired immediately, inside the constructor");
            });
        log("   constructor returned");
    }

    // ---- 5d. Destroyed before the stop -> deregistered, never runs -----------
    {
        log("\n-- 5d. callback destroyed before the stop never runs --");
        std::stop_source src;
        {
            std::stop_callback cb(src.get_token(), [] { log("   !!! SHOULD NEVER PRINT !!!"); });
            log("   callback alive");
        }                                    // ~stop_callback(): deregistered
        log("   callback destroyed, now calling request_stop()");
        src.request_stop();
        log("   request_stop() done. No callback output above, as expected.");
    }
}

// =============================================================================
// 6. Interrupting a BLOCKED wait: std::condition_variable_any + stop_token
//    (a polling loop cannot help when the thread is sleeping inside wait())
// =============================================================================
void section6_condition_variable_any() {
    banner("6. condition_variable_any::wait(lock, stop_token, predicate)");

    std::mutex                   m;
    std::condition_variable_any  cv;          // must be the _any version for stop_token support
    std::queue<int>              jobs;
    std::stop_source             src;

    // `std::stop_token st` is taken BY VALUE: a copy of the token passed below.
    std::thread worker ([&](std::stop_token st) {
        std::unique_lock lk(m);
        // wait() returns the value of the predicate:
        //   true  -> a job is available
        //   false -> a stop was requested AND no job is available
        while (cv.wait (lk, st, [&] { return !jobs.empty(); })) {
            int job = jobs.front();
            jobs.pop();
            lk.unlock();                      // never do slow work while holding the lock
            log("   worker processed job ", job);
            lk.lock();
        }
        log("   worker: woken by stop request while idle -> exiting");
        }, src.get_token());                      // rvalue token, copied into the thread

    for (int j = 1; j <= 3; ++j) {
        { std::lock_guard lk(m); jobs.push(j); }
        cv.notify_one();
        std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(100ms);       // worker is now asleep inside cv.wait()
    log("   main: requesting stop");
    src.request_stop();                       // wakes the sleeping worker, no notify() needed
    worker.join();
}

// =============================================================================
// 7. REAL-TIME SCENARIO: a download manager
//
//      main thread     : owns the stop_source ("Cancel all" button)
//      3 download threads : check the token every chunk + register a callback
//                           that "closes the socket" when cancelled
//      user thread     : presses Cancel after 350 ms
//      watchdog thread : would cancel everything after 2 s (a timeout), but
//                        sleeps in a stop-aware wait, so it wakes up early
//
//      Worker 3 is fast and finishes BEFORE the cancel: its callback is
//      destroyed (deregistered) and never fires.
// =============================================================================
void section7_download_manager() {
    banner("7. Real-time scenario: download manager with cancel + timeout");

    std::stop_source cancel_all;              // the shared "Cancel button"

    struct Job {
        int                       id;
        int                       chunks;
        std::chrono::milliseconds per_chunk;
    };
    const std::vector<Job> jobs = {
        {1, 10, 60ms},                        // would need 600 ms
        {2, 10, 80ms},                        // would need 800 ms
        {3,  5, 40ms},                        // needs only 200 ms -> finishes first
    };
    std::vector<std::string> status(jobs.size());   // each worker writes only its own slot

    // --- the worker ---------------------------------------------------------
    // st  : std::stop_token taken by value (a copy of the shared handle)
    // job : Job taken by value
    auto download = [&status](std::stop_token st, Job job) {
        // Event-driven part: when a stop is requested, run this lambda.
        // `st` is an lvalue -> const std::stop_token& constructor overload.
        // [id = job.id] copies the id into the lambda (safest, no dangling references).
        std::stop_callback on_cancel(st, [id = job.id] {
            log("   [callback] worker ", id, ": closing socket (runs on thread ",
                std::this_thread::get_id(), ")");
            });

        // Polling part: check the token between chunks.
        for (int chunk = 1; chunk <= job.chunks; ++chunk) {
            if (st.stop_requested()) {
                status[job.id - 1] = "CANCELLED after " + std::to_string(chunk - 1) +
                    "/" + std::to_string(job.chunks) + " chunks";
                log("   worker ", job.id, ": stop seen, cleaning up and leaving");
                return;
            }
            std::this_thread::sleep_for(job.per_chunk);       // pretend to download a chunk
            log("   worker ", job.id, ": chunk ", chunk, "/", job.chunks, " done");
        }
        status[job.id - 1] = "COMPLETED";
        log("   worker ", job.id, ": finished the whole file");
        };   // on_cancel is destroyed here (deregistered) if we finish normally

    std::vector<std::thread> workers;
    for (const Job& job : jobs)
        workers.emplace_back(download, cancel_all.get_token(), job);   // rvalue token copied in

    // --- "user presses Cancel" ---------------------------------------------
    std::thread user([&cancel_all] {
        std::this_thread::sleep_for(350ms);
        bool first = cancel_all.request_stop();   // callbacks of unfinished workers run HERE
        log("   [user] pressed Cancel, request_stop() returned ", yn(first));
        });

    // --- watchdog with a stop-aware timeout ---------------------------------
    // The lambda captures a COPY of the stop_source (same shared state).
    // `mutable` because request_stop() is a non-const member function.
    std::thread watchdog([cancel_all]() mutable {
        std::stop_token  st = cancel_all.get_token();
        std::mutex                  m;
        std::condition_variable_any cv;
        std::unique_lock            lk(m);

        // Sleep up to 2 s. Returns early if a stop is requested. The predicate is never true.
        cv.wait_for(lk, st, 2s, [] { return false; });

        if (st.stop_requested()) {
            log("   [watchdog] someone already cancelled, exiting early");
        }
        else {
            bool first = cancel_all.request_stop();
            log("   [watchdog] TIMEOUT! request_stop() returned ", yn(first));
        }
        });

    for (auto& w : workers) w.join();
    user.join();
    watchdog.join();

    log("\n   Summary:");
    for (std::size_t i = 0; i < status.size(); ++i)
        log("     worker ", i + 1, ": ", status[i]);
}

// =============================================================================
// 8. BONUS: std::jthread already owns a stop_source for you
// =============================================================================
void section8_jthread() {
    banner("8. std::jthread = thread + built-in stop_source + auto join");

    // If the callable's FIRST parameter is std::stop_token, jthread passes one in.
    std::jthread jt([](std::stop_token st) {
        while (!st.stop_requested())
            std::this_thread::sleep_for(10ms);
        log("   jthread body: stop seen, returning");
        });

    std::stop_source ss = jt.get_stop_source();   // a COPY of the jthread's own source
    std::stop_token  tt = jt.get_stop_token();    // a token of that same state
    log("   ss and tt share the same state: ", yn(ss.get_token() == tt));

    std::this_thread::sleep_for(50ms);
    bool first = jt.request_stop();               // same as ss.request_stop()
    log("   jt.request_stop() returned ", yn(first));
    // ~jthread(): calls request_stop() again (harmless) and then join()
}

int main() {
    section1_basics();
    section2_stop_possible();
    section3_copy_move_swap();
    section4_request_stop_once();
    section5_callbacks();
    section6_condition_variable_any();
    section7_download_manager();
    section8_jthread();
    log("\nAll sections finished.");
}