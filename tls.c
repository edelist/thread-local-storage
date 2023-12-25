#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#define HASH_SIZE 4096 // not sure

// define TLS
typedef struct thread_local_storage {
        pthread_t tid;
        unsigned int size; // size in bytes
        unsigned int page_num; // number of pages
        struct page ** pages; // array of pointers to pages
} TLS;

// define page
struct page {
        uintptr_t address; // start address of page
        int ref_count; // counter for shared pages
};

// define hash element
struct hash_element {
        pthread_t tid;
        TLS *tls;
        struct hash_element *next;
};

// init hash table
struct hash_element* hash_table[HASH_SIZE];

// helper function to insert new TLS mapping into hash table
void hash_table_insert(pthread_t tid, TLS* tls) {
        // compute hash value for given thread id
        int hash_index = tid % HASH_SIZE;

        // create new has element
        struct hash_element* new_elem = (struct hash_element*)malloc(sizeof(struct hash_element));
        if (new_elem == NULL) {
                perror("ERROR: Failed to allocate memory for new hash element.");
                exit(1);
        }

        // init new element
        new_elem->tid = tid;
        new_elem->tls = tls;
        new_elem->next = NULL;

        // insert new element into hash table
        if (hash_table[hash_index] == NULL) {
                // no collision
                hash_table[hash_index] = new_elem;
        } else {
                // collision occured - chain
                new_elem->next = hash_table[hash_index];
                hash_table[hash_index] = new_elem;
        }
}

// init
int initialized = 0;
int page_size = 0;

// prototype for warnings
void tls_handle_page_fault(int, siginfo_t*, void*);

// init code
void tls_init() {
        struct sigaction sa;

        // get size of a page
        page_size = getpagesize();

        // install signal handler for page faults - SIGSEGV, SIGBUS
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO; // use extended signal handling
        sa.sa_sigaction = tls_handle_page_fault;

        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);

        initialized = 1; // init this
}

// page fault handler
void tls_handle_page_fault(int sig, siginfo_t* si, void* context) {
        uintptr_t p_fault = ((uintptr_t) si->si_addr) & ~(page_size-1);

        // check all TSL entries in hash table
        int i;
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element* elem = hash_table[i];
                while (elem != NULL) {
                        TLS* tls = elem->tls;

                        // check each page in current TLS
                        int j;
                        for (j=0; j<tls->page_num; j++) {
                                struct page* page = tls->pages[j];
                                if (page->address == p_fault) {
                                        // faulting address is part of this threads's TLS
                                        if (pthread_equal(pthread_self(), elem->tid)) {
                                                // current rhead is access its own TLS illegally
                                                pthread_exit(NULL);
                                        }
                                }
                        }

                        elem = elem-> next;
                }
        }

        // normal fault - install default handler and re-raise signal
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        raise(sig);
}

// create
int tls_create(unsigned int size) {
        if (!initialized) {
                tls_init();
        }

        // check if current thread already has LSA
        pthread_t current_thread = pthread_self();
        int i;
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element* elem = hash_table[i];
                while (elem != NULL) {
                        if (pthread_equal(elem->tid, current_thread)) {
                                perror("ERROR: Thread already has LSA.");
                                return -1;
                        }
                        elem = elem->next;
                }
        }

        // check if size > 0
        if (size <= 0) {
                perror("ERROR: Invalid size.");
                return -1;
        }

        // allocate TLS
        TLS* tls = (TLS*)calloc(1, sizeof(TLS));
        if (tls == NULL) {
                perror("ERROR: TLS allocation failed.");
                return -1;
        }

        // initialize TLS
        tls->tid = current_thread;
        tls->size = size;
        tls->page_num = (size + page_size - 1) / page_size; // compute # pages

        // allocate TLS->pages
        tls->pages = (struct page**)calloc(tls->page_num, sizeof(struct page*));
        if (tls->pages == NULL) {
                free(tls);
                perror("ERROR: Page allocation failed.");
                return -1;
        }

        // allocate all pages for this TLS
        for (i=0; i<tls->page_num; i++) {
                struct page* p = (struct page*)malloc(sizeof(struct page));
                if (p == NULL) {
                        // handle partial allocation
                        int j;
                        for (j=0;j<i;j++) {
                                munmap((void*)tls->pages[j]->address, page_size);
                                free(tls->pages[j]);
                        }
                        free(tls->pages);
                        free(tls);
                        return -1;

                }
                p->address = (uintptr_t)mmap(0, page_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, 0, 0);
                if ((void*)p->address == MAP_FAILED) {
                        // handle partial allocation
                        free(p);
                        int j;
                        for (j=0; j<i; j++) {
                                munmap((void*)tls->pages[j]->address, page_size);
                                free(tls->pages[j]);
                        }
                        free(tls->pages);
                        free(tls);
                        perror("ERROR: Memory mapping failed.");
                        return -1;
                }


                p->ref_count = 1;
                tls->pages[i] = p;

        }

        // add this thread id and TLS mapping to global has table
        hash_table_insert(current_thread, tls);

        return 0;
}

// tls_destroy
int tls_destroy() {
        pthread_t current_thread = pthread_self();
        TLS* tls = NULL;
        int tls_found = 0;

        // search for current threa's TLS in global hash table
        int i;
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element* elem = hash_table[i];
                while (elem != NULL) {
                        if (pthread_equal(elem->tid, current_thread)) {
                                tls = elem->tls;
                                tls_found = 1;
                                break;
                        }
                        elem = elem->next;
                }
                if (tls_found) {
                        break;
                }
        }

        // check if current thread has LSA
        if (!tls_found) {
                perror("ERROR: current thread does not have an LSA.");
                return -1;
        }

        // clean up all pages
        for (i=0; i<tls->page_num; i++) {
                struct page* p = tls->pages[i];
                if (p->ref_count == 1) {
                        munmap((void*)p->address, page_size); // page not shared - free it
                        free(p);
                } else {
                        p->ref_count--; // pge is shared - decrement count
                }
        }

        free(tls->pages); // free array of page pointers

        // remove mapping from global hash table
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element** elem = &(hash_table[i]);
                while (*elem != NULL) {
                        if (pthread_equal((*elem)->tid, current_thread)) {
                                struct hash_element* temp = *elem;
                                *elem = (*elem)->next;
                                free(temp);
                                break;
                        }
                        elem = &((*elem)->next);
                }
        }


        free(tls);
        return 0;
}


// protect helper function
void tls_protect(struct page* p) {
        if (mprotect((void*) p->address, page_size, 0)) {
                fprintf(stderr, "tls_protect: could not protect page\n");
                exit(1);
        }
}

// unprotect helper function
void tls_unprotect(struct page* p) {
        if (mprotect((void*) p->address, page_size, PROT_READ | PROT_WRITE)) {
                fprintf(stderr, "tls_unprotect: could not unprotect page\n");
                exit(1);
        }
}

// tls_read
int tls_read(unsigned int offset, unsigned int length, char *buffer) {
        pthread_t current_thread = pthread_self();
        TLS* tls = NULL;
        int tls_found = 0;

        // search for current thread's TLS in global hash table
        int i;
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element* elem = hash_table[i];
                while (elem != NULL) {
                        if (pthread_equal(elem->tid, current_thread)) {
                                tls = elem->tls;
                                tls_found = 1;
                                break;
                        }
                        elem = elem->next;
                }
                if (tls_found) {
                        break;
                }
        }

        // check if current thread has LSA
        if (!tls_found) {
                perror("ERROR: Current thread does not have an LSA.");
                return -1;
        }

        // check if offset+length is within TLS size
        if (offset + length > tls->size) {
                perror("ERROR: Requested read exceeds TLS size.");
                return -1;
        }

        // unprotect all pages belonging to thread's TLS
        for (i=0; i<tls->page_num; i++) {
                tls_unprotect(tls->pages[i]);
        }

        // perform read operation
        unsigned int cnt, idx;
        for (cnt=0, idx = offset; idx < (offset + length); ++cnt, ++idx) {
                unsigned int pn = idx / page_size;
                unsigned int poff = idx % page_size;
                struct page* p = tls->pages[pn];
                char* src = ((char*) p->address) + poff;
                buffer[cnt] = *src;
        }

        // reprotect all pages belonging to thread's TLS
        for (i=0; i<tls->page_num; i++) {
                tls_protect(tls->pages[i]);
        }

        return 0;
}

// tls_write
int tls_write(unsigned int offset, unsigned int length, char* buffer) {
        pthread_t current_thread = pthread_self();
        TLS* tls = NULL;
        int tls_found = 0;

        // search for current thread's TLS in global hash table
        int i;
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element* elem = hash_table[i];
                while (elem != NULL) {
                        if (pthread_equal(elem->tid, current_thread)) {
                                tls = elem->tls;
                                tls_found = 1;
                                break;
                        }
                        elem = elem->next;
                }
                if (tls_found) {
                        break;
                }
        }

        // check if current thread has LSA
        if (!tls_found) {
                perror("ERROR: current thread does not have an LSA.");
                return -1;
        }

        // check if offset+length is within TLS size
        if (offset + length > tls->size) {
                perror("ERROR: Requested write exceeds TLS size.");
                return -1;
        }

        // unprotect all pages belonging to thread's TLS
        for (i=0; i<tls->page_num; i++) {
                tls_unprotect(tls->pages[i]);
        }

        // perform write operation
        unsigned int cnt, idx;
        for (cnt=0, idx = offset; idx < (offset + length); ++cnt, ++idx) {
                unsigned int pn = idx / page_size;
                unsigned int poff = idx % page_size;


                //check CoW condition once per page
                if (idx % page_size == 0 || idx == offset) {
                        struct page* p = tls->pages[pn];
                        // CoW mechanism
                        if (p->ref_count > 1) {
                                // page is shared, create new private copy
                                struct page* copy = (struct page*) calloc(1, sizeof(struct page));
                                if (copy == NULL) {
                                        perror("ERROR: Memory allocation for page copy.");
                                        return -1;
                                }
                                void* new_page = mmap(0, page_size, PROT_WRITE, MAP_ANON | MAP_PRIVATE, 0, 0);
                                if (new_page == MAP_FAILED) {
                                        free(copy);
                                        perror("ERROR: mmap failed for page copy.");
                                        return -1;
                                }
                                memcpy(new_page, (void*)p->address, page_size);
                                copy->address = (uintptr_t)new_page;
                                copy->ref_count = 1;
                                tls->pages[pn] = copy;

                                // update original page
                                p->ref_count--;
                                tls_protect(p);
                                p = copy;
                        }
                }
                struct page* p = tls->pages[pn];
                char* dst = ((char*) p->address) + poff;
                *dst = buffer[cnt];
        }

        // reprotect all pages belonging to thread's TLS
        for (i=0; i<tls->page_num; i++) {
                tls_protect(tls->pages[i]);
        }

        return 0;
}

// tls_clone
int tls_clone(pthread_t tid) {
        pthread_t current_thread = pthread_self();
        TLS* current_tls = NULL;
        TLS* target_tls = NULL;
        int target_tls_found = 0;

        // check if current thread already has LSA
        int i;
        for (i=0; i<HASH_SIZE; i++) {
                struct hash_element* elem = hash_table[i];
                while (elem != NULL) {
                        if (pthread_equal(elem->tid, current_thread)) {
                                current_tls = elem->tls;
                                break;
                        }
                        if (pthread_equal(elem->tid, tid)) {
                                target_tls = elem->tls;
                                target_tls_found = 1;
                        }
                        elem = elem->next;
                }
        }

        if (current_tls != NULL) {
                perror("ERROR: current thread already has LSA.");
                return -1;
        }

        // check if target thread has LSA
        if (!target_tls_found) {
                perror("ERROR: target thread does not have LSA.");
                return -1;
        }

        // clone tls - allocate tls for current thread
        TLS* new_tls = (TLS*)calloc(1, sizeof(TLS));
        if (new_tls == NULL) {
                perror("ERROR: cloning TLS allocation failed.");
                return -1;
        }

        new_tls->tid = current_thread;
        new_tls->size = target_tls->size;
        new_tls->page_num = target_tls->page_num;
        new_tls->pages = (struct page**)calloc(new_tls->page_num, sizeof(struct page*));
        if (new_tls->pages == NULL) {
                free(new_tls);
                perror("ERROR: cloning TLS allocation failed.");
                return -1;
        }

        // copy pages, adjust reference counts
        for (i=0; i<new_tls->page_num; i++) {
                new_tls->pages[i] = target_tls->pages[i];
                new_tls->pages[i]->ref_count++;
        }

        // add this thread mapping to global hash table
        hash_table_insert(current_thread, new_tls);

        return 0;

}